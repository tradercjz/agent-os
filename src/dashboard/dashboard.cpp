// ============================================================
// AgentOS :: Dashboard — HTTP server implementation
// ============================================================
#include <agentos/dashboard/dashboard.hpp>
#include <agentos/agent.hpp>
#include <agentos/core/logger.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>

#include <array>
#include <string_view>

namespace agentos::dashboard {

DashboardServer::DashboardServer(AgentOS& os, uint16_t port)
    : os_(os), port_(port) {}

DashboardServer::~DashboardServer() { stop(); }

Result<void> DashboardServer::start() {
    if (running_.load()) {
        return make_error(ErrorCode::AlreadyExists, "Dashboard server already running");
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return make_error(ErrorCode::Unknown, "Failed to create socket");
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return make_error(ErrorCode::Unknown,
                          "Failed to bind to port " + std::to_string(port_));
    }

    // Retrieve the actual port if port_ was 0 (OS-assigned)
    if (port_ == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(server_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }
    }

    if (listen(server_fd_, 16) < 0) {
        close(server_fd_);
        server_fd_ = -1;
        return make_error(ErrorCode::Unknown, "Failed to listen");
    }

    running_ = true;
    server_thread_ = std::jthread([this](std::stop_token) { serve_loop(); });
    LOG_INFO(fmt::format("Dashboard server started on port {}", port_));
    return {};
}

void DashboardServer::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // Already stopped
    }

    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.request_stop();
        server_thread_.join();
    }
    LOG_INFO("Dashboard server stopped");
}

void DashboardServer::serve_loop() {
    while (running_.load()) {
        pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500); // 500ms timeout for clean shutdown
        if (ret <= 0) continue;

        if ((pfd.revents & POLLIN) == 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (client_fd < 0) continue;

        handle_client(client_fd);
    }
}

void DashboardServer::handle_client(int client_fd) {
    // Read up to 4KB — sufficient for simple HTTP request lines
    std::array<char, 4096> buf{};
    ssize_t n = read(client_fd, buf.data(), buf.size() - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    std::string_view request(buf.data(), static_cast<size_t>(n));

    // Parse first line: "METHOD /path HTTP/1.x\r\n"
    auto line_end = request.find('\r');
    if (line_end == std::string_view::npos) {
        line_end = request.find('\n');
    }
    if (line_end == std::string_view::npos) {
        close(client_fd);
        return;
    }

    std::string_view first_line = request.substr(0, line_end);

    // Split into method and path
    auto sp1 = first_line.find(' ');
    if (sp1 == std::string_view::npos) {
        close(client_fd);
        return;
    }
    std::string method(first_line.substr(0, sp1));

    auto rest = first_line.substr(sp1 + 1);
    auto sp2 = rest.find(' ');
    std::string path(rest.substr(0, sp2));

    std::string response = handle_request(method, path);

    // Write response — ignore partial writes for this simple server
    (void)write(client_fd, response.data(), response.size());
    close(client_fd);
}

std::string DashboardServer::handle_request(const std::string& method,
                                            const std::string& path) {
    if (method != "GET") {
        return http_json(405, R"({"error":"Method not allowed"})");
    }

    if (path == "/") return route_index();
    if (path == "/api/health") return route_api_health();
    if (path == "/api/metrics") return route_api_metrics();
    if (path == "/api/agents") return route_api_agents();
    if (path == "/api/traces") return route_api_traces();
    if (path == "/metrics") return route_metrics_prometheus();

    return http_json(404, R"({"error":"Not found"})");
}

// ── Route implementations ──────────────────────────────────────

std::string DashboardServer::route_index() {
    static const std::string html = R"html(<!DOCTYPE html>
<html>
<head><title>AgentOS Dashboard</title>
<meta http-equiv="refresh" content="5">
<style>
body { font-family: monospace; margin: 20px; background: #1a1a2e; color: #eee; }
.card { background: #16213e; padding: 15px; margin: 10px 0; border-radius: 8px; }
h1 { color: #e94560; }
h2 { color: #0f3460; margin-top: 0; }
pre { background: #0f3460; padding: 10px; border-radius: 4px; overflow-x: auto; }
</style>
</head>
<body>
<h1>AgentOS Dashboard</h1>
<div class="card"><h2>Health</h2><pre id="health">Loading...</pre></div>
<div class="card"><h2>Metrics</h2><pre id="metrics">Loading...</pre></div>
<div class="card"><h2>Agents</h2><pre id="agents">Loading...</pre></div>
<script>
async function refresh() {
    try {
        let h = await fetch('/api/health').then(r=>r.json());
        document.getElementById('health').textContent = JSON.stringify(h, null, 2);
        let m = await fetch('/api/metrics').then(r=>r.json());
        document.getElementById('metrics').textContent = JSON.stringify(m, null, 2);
        let a = await fetch('/api/agents').then(r=>r.json());
        document.getElementById('agents').textContent = JSON.stringify(a, null, 2);
    } catch(e) { /* ignore fetch errors during refresh */ }
}
refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>
)html";
    return http_html(200, html);
}

std::string DashboardServer::route_api_health() {
    return http_json(200, os_.health().to_json());
}

std::string DashboardServer::route_api_metrics() {
    const auto& km = os_.kernel().metrics();
    const auto& sm = os_.scheduler().metrics();

    nlohmann::json j;
    j["kernel"]["total_requests"] = km.total_requests.load();
    j["kernel"]["total_tokens"] = km.total_tokens.load();
    j["kernel"]["errors"] = km.errors.load();
    j["kernel"]["retries"] = km.retries.load();
    j["scheduler"]["tasks_submitted"] = sm.tasks_submitted.load();
    j["scheduler"]["tasks_completed"] = sm.tasks_completed.load();
    j["scheduler"]["tasks_failed"] = sm.tasks_failed.load();
    j["agents"] = os_.agent_count();

    return http_json(200, j.dump());
}

std::string DashboardServer::route_api_agents() {
    nlohmann::json arr = nlohmann::json::array();

    // We can only report count — individual agent iteration
    // requires internal access. Report summary.
    nlohmann::json info;
    info["count"] = os_.agent_count();
    arr.push_back(info);

    return http_json(200, arr.dump());
}

std::string DashboardServer::route_api_traces() {
    auto traces = os_.tracer().recent_traces(10);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : traces) {
        nlohmann::json tj;
        tj["trace_id"] = t.trace_id;
        tj["agent_id"] = t.agent_id;
        tj["goal"] = t.goal;
        tj["spans"] = t.spans.size();
        tj["total_tokens"] = t.total_tokens;
        tj["success"] = t.success;
        tj["duration_ms"] = t.duration().count();
        arr.push_back(tj);
    }
    return http_json(200, arr.dump());
}

std::string DashboardServer::route_metrics_prometheus() {
    return http_response(200, "text/plain; charset=utf-8",
                         os_.metrics_prometheus());
}

// ── HTTP helpers ───────────────────────────────────────────────

std::string DashboardServer::http_response(int status,
                                           const std::string& content_type,
                                           const std::string& body) {
    std::string status_text;
    switch (status) {
        case 200: status_text = "OK"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        default:  status_text = "Error"; break;
    }

    return fmt::format(
        "HTTP/1.0 {} {}\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n"
        "{}",
        status, status_text, content_type, body.size(), body);
}

std::string DashboardServer::http_json(int status, const std::string& json_body) {
    return http_response(status, "application/json", json_body);
}

std::string DashboardServer::http_html(int status, const std::string& html_body) {
    return http_response(status, "text/html; charset=utf-8", html_body);
}

} // namespace agentos::dashboard

// ── AgentOS::start_dashboard implementation ──────────────────
namespace agentos {

Result<void> AgentOS::start_dashboard(uint16_t port) {
    if (dashboard_) {
        return make_error(ErrorCode::AlreadyExists, "Dashboard already started");
    }
    dashboard_ = std::make_unique<dashboard::DashboardServer>(*this, port);
    return dashboard_->start();
}

} // namespace agentos
