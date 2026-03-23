// ============================================================
// AgentOS :: API — REST API server implementation
// ============================================================
#include <agentos/api/api_server.hpp>
#include <agentos/agent.hpp>
#include <agentos/agentos.hpp>
#include <agentos/core/logger.hpp>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>

#include <array>
#include <sstream>
#include <string_view>

namespace agentos::api {

ApiServer::ApiServer(AgentOS& os, uint16_t port)
    : os_(os), port_(port) {
    register_default_routes();
}

ApiServer::~ApiServer() { stop(); }

Result<void> ApiServer::start() {
    if (running_.load()) {
        return make_error(ErrorCode::AlreadyExists, "API server already running");
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

    // Retrieve actual port if OS-assigned
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
    LOG_INFO(fmt::format("API server started on port {}", port_));
    return {};
}

void ApiServer::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
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
    LOG_INFO("API server stopped");
}

void ApiServer::route(const std::string& method, const std::string& path,
                      RouteHandler handler) {
    std::lock_guard lk(routes_mu_);
    routes_[method + " " + path] = std::move(handler);
}

void ApiServer::serve_loop() {
    while (running_.load()) {
        pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
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

void ApiServer::handle_client(int client_fd) {
    // Read up to 8KB
    std::array<char, 8192> buf{};
    ssize_t n = read(client_fd, buf.data(), buf.size() - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    std::string raw(buf.data(), static_cast<size_t>(n));
    auto req = parse_request(raw);

    // Streaming routes bypass normal dispatch — they write directly to the fd
    if (is_stream_route(req.method, req.path)) {
        handle_infer_stream(client_fd, req);
        close(client_fd);
        return;
    }

    auto resp = dispatch(req);

    std::string http_resp = http_response(resp.status, resp.content_type, resp.body);
    (void)write(client_fd, http_resp.data(), http_resp.size());
    close(client_fd);
}

bool ApiServer::is_stream_route(const std::string& method,
                                const std::string& path) const {
    return method == "POST" && path == "/api/v1/infer/stream";
}

ApiRequest ApiServer::parse_request(const std::string& raw) {
    ApiRequest req;

    // Parse first line: "METHOD /path HTTP/1.x\r\n"
    std::string_view sv(raw);
    auto line_end = sv.find('\r');
    if (line_end == std::string_view::npos) {
        line_end = sv.find('\n');
    }
    if (line_end == std::string_view::npos) {
        return req;
    }

    std::string_view first_line = sv.substr(0, line_end);

    // Split into method and path
    auto sp1 = first_line.find(' ');
    if (sp1 == std::string_view::npos) return req;
    req.method = std::string(first_line.substr(0, sp1));

    auto rest = first_line.substr(sp1 + 1);
    auto sp2 = rest.find(' ');
    std::string full_path(rest.substr(0, sp2));

    // Split path and query string
    auto qmark = full_path.find('?');
    if (qmark != std::string::npos) {
        req.path = full_path.substr(0, qmark);
        std::string qs = full_path.substr(qmark + 1);
        // Parse query params: key=val&key2=val2
        size_t pos = 0;
        while (pos < qs.size()) {
            auto amp = qs.find('&', pos);
            std::string pair = (amp != std::string::npos)
                ? qs.substr(pos, amp - pos) : qs.substr(pos);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                req.query_params[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
            pos = (amp != std::string::npos) ? amp + 1 : qs.size();
        }
    } else {
        req.path = full_path;
    }

    // Extract body after \r\n\r\n for POST requests
    if (req.method == "POST" || req.method == "PUT") {
        auto body_start = raw.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            req.body = raw.substr(body_start + 4);
        }
    }

    return req;
}

ApiResponse ApiServer::dispatch(const ApiRequest& req) {
    std::lock_guard lk(routes_mu_);

    // Try exact match
    std::string key = req.method + " " + req.path;
    auto it = routes_.find(key);
    if (it != routes_.end()) {
        return it->second(req);
    }

    // Try pattern match for parameterized routes (/api/v1/agents/:id)
    // Strip last path segment and try with parent + wildcard
    auto last_slash = req.path.rfind('/');
    if (last_slash != std::string::npos && last_slash > 0) {
        std::string parent = req.path.substr(0, last_slash) + "/:id";
        std::string parent_key = req.method + " " + parent;
        auto pit = routes_.find(parent_key);
        if (pit != routes_.end()) {
            return pit->second(req);
        }
    }

    return ApiResponse::error(404, "Not found");
}

// ── Default route registration ──────────────────────────────

void ApiServer::register_default_routes() {
    route("GET", "/api/v1/health",
          [this](const ApiRequest& r) { return handle_health(r); });
    route("GET", "/api/v1/metrics",
          [this](const ApiRequest& r) { return handle_metrics(r); });
    route("GET", "/api/v1/agents",
          [this](const ApiRequest& r) { return handle_list_agents(r); });
    route("POST", "/api/v1/agents",
          [this](const ApiRequest& r) { return handle_create_agent(r); });
    route("GET", "/api/v1/agents/:id",
          [this](const ApiRequest& r) { return handle_get_agent(r); });
    route("DELETE", "/api/v1/agents/:id",
          [this](const ApiRequest& r) { return handle_delete_agent(r); });
    route("POST", "/api/v1/infer",
          [this](const ApiRequest& r) { return handle_infer(r); });
    route("GET", "/api/v1/prometheus",
          [this](const ApiRequest& r) { return handle_prometheus(r); });
}

// ── Route handlers ──────────────────────────────────────────

ApiResponse ApiServer::handle_health(const ApiRequest& /*req*/) {
    auto h = os_.health();
    nlohmann::json j;
    j["status"] = "ok";
    j["agent_count"] = h.active_agents;
    j["healthy"] = h.healthy;
    j["model"] = h.model;
    return ApiResponse::ok(j);
}

ApiResponse ApiServer::handle_metrics(const ApiRequest& /*req*/) {
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

    return ApiResponse::ok(j);
}

ApiResponse ApiServer::handle_list_agents(const ApiRequest& /*req*/) {
    nlohmann::json arr = nlohmann::json::array();

    // Iterate through agent IDs to find existing agents
    // We use find_agent which is thread-safe
    size_t count = os_.agent_count();
    // Scan reasonable range of IDs
    for (AgentId id = 1; id <= count + 100 && arr.size() < count; ++id) {
        auto agent = os_.find_agent(id);
        if (agent) {
            nlohmann::json aj;
            aj["id"] = agent->id();
            aj["name"] = agent->config().name;
            arr.push_back(aj);
        }
    }

    return ApiResponse::ok(arr);
}

ApiResponse ApiServer::handle_create_agent(const ApiRequest& req) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error& e) {
        return ApiResponse::error(400, std::string("Invalid JSON: ") + e.what());
    }

    auto name = j.value("name", "api-agent");
    auto prompt = j.value("prompt", "");

    auto agent = make_agent(os_, name).prompt(prompt).create();

    nlohmann::json resp;
    resp["id"] = agent->id();
    resp["name"] = name;
    resp["status"] = "created";
    return ApiResponse::ok(resp);
}

ApiResponse ApiServer::handle_get_agent(const ApiRequest& req) {
    // Extract ID from path: /api/v1/agents/42
    auto last_slash = req.path.rfind('/');
    if (last_slash == std::string::npos) {
        return ApiResponse::error(400, "Invalid agent path");
    }

    std::string id_str = req.path.substr(last_slash + 1);
    AgentId id = 0;
    try {
        id = std::stoull(id_str);
    } catch (const std::exception&) {
        return ApiResponse::error(400, "Invalid agent ID");
    }

    auto agent = os_.find_agent(id);
    if (!agent) {
        return ApiResponse::error(404, "Agent not found");
    }

    nlohmann::json j;
    j["id"] = agent->id();
    j["name"] = agent->config().name;
    j["role_prompt"] = agent->config().role_prompt;
    j["context_limit"] = agent->config().context_limit;
    j["security_role"] = agent->config().security_role;
    return ApiResponse::ok(j);
}

ApiResponse ApiServer::handle_delete_agent(const ApiRequest& req) {
    auto last_slash = req.path.rfind('/');
    if (last_slash == std::string::npos) {
        return ApiResponse::error(400, "Invalid agent path");
    }

    std::string id_str = req.path.substr(last_slash + 1);
    AgentId id = 0;
    try {
        id = std::stoull(id_str);
    } catch (const std::exception&) {
        return ApiResponse::error(400, "Invalid agent ID");
    }

    auto agent = os_.find_agent(id);
    if (!agent) {
        return ApiResponse::error(404, "Agent not found");
    }

    os_.destroy_agent(id);

    nlohmann::json j;
    j["id"] = id;
    j["status"] = "deleted";
    return ApiResponse::ok(j);
}

ApiResponse ApiServer::handle_infer(const ApiRequest& req) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error& e) {
        return ApiResponse::error(400, std::string("Invalid JSON: ") + e.what());
    }

    if (!j.contains("messages") || !j["messages"].is_array()) {
        return ApiResponse::error(400, "Missing or invalid 'messages' array");
    }

    kernel::LLMRequest llm_req;
    for (const auto& msg : j["messages"]) {
        auto role_str = msg.value("role", "user");
        auto content = msg.value("content", "");
        if (role_str == "system") {
            llm_req.messages.push_back(kernel::Message::system(content));
        } else if (role_str == "assistant") {
            llm_req.messages.push_back(kernel::Message::assistant(content));
        } else {
            llm_req.messages.push_back(kernel::Message::user(content));
        }
    }
    if (j.contains("model")) {
        llm_req.model = j["model"].get<std::string>();
    }

    auto result = os_.kernel().infer(llm_req);
    if (!result.has_value()) {
        return ApiResponse::error(500, result.error().message);
    }

    nlohmann::json resp;
    resp["content"] = result->content;
    resp["tokens"]["prompt"] = result->prompt_tokens;
    resp["tokens"]["completion"] = result->completion_tokens;
    return ApiResponse::ok(resp);
}

void ApiServer::handle_infer_stream(int client_fd, const ApiRequest& req) {
    StreamWriter writer(client_fd);

    // Parse request body
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error& e) {
        // Send error as normal HTTP response (stream not started yet)
        auto err_resp = http_response(400, "application/json",
            nlohmann::json({{"error", std::string("Invalid JSON: ") + e.what()},
                            {"status", 400}}).dump());
        (void)write(client_fd, err_resp.data(), err_resp.size());
        return;
    }

    if (!j.contains("messages") || !j["messages"].is_array()) {
        auto err_resp = http_response(400, "application/json",
            nlohmann::json({{"error", "Missing or invalid 'messages' array"},
                            {"status", 400}}).dump());
        (void)write(client_fd, err_resp.data(), err_resp.size());
        return;
    }

    // Build LLM request
    kernel::LLMRequest llm_req;
    for (const auto& msg : j["messages"]) {
        auto role_str = msg.value("role", "user");
        auto content = msg.value("content", "");
        if (role_str == "system") {
            llm_req.messages.push_back(kernel::Message::system(content));
        } else if (role_str == "assistant") {
            llm_req.messages.push_back(kernel::Message::assistant(content));
        } else {
            llm_req.messages.push_back(kernel::Message::user(content));
        }
    }
    if (j.contains("model")) {
        llm_req.model = j["model"].get<std::string>();
    }

    // Begin SSE stream
    auto begin_r = writer.begin();
    if (!begin_r.has_value()) {
        LOG_ERROR(fmt::format("SSE stream begin failed: {}", begin_r.error().message));
        return;
    }

    // Perform inference
    auto result = os_.kernel().infer(llm_req);
    if (!result.has_value()) {
        // Send error event then close
        (void)writer.send(make_error_event(result.error().message));
        (void)writer.end();
        return;
    }

    // Stream the content as token events.
    // Since the underlying kernel returns a complete response, we simulate
    // token-by-token streaming by splitting on whitespace boundaries.
    const auto& content = result->content;
    if (!content.empty()) {
        std::istringstream iss(content);
        std::string word;
        bool first = true;
        while (iss >> word) {
            std::string token = first ? word : " " + word;
            first = false;
            auto sr = writer.send(make_token_event(token));
            if (!sr.has_value()) {
                LOG_WARN("SSE client disconnected during streaming");
                return;
            }
        }
    }

    // Send done event with token counts
    (void)writer.send(make_done_event(result->prompt_tokens,
                                       result->completion_tokens));
    (void)writer.end();
}

ApiResponse ApiServer::handle_prometheus(const ApiRequest& /*req*/) {
    return {200, "text/plain; charset=utf-8", os_.metrics_prometheus()};
}

// ── HTTP helpers ────────────────────────────────────────────

std::string ApiServer::status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Error";
    }
}

std::string ApiServer::http_response(int status, const std::string& content_type,
                                     const std::string& body) {
    return fmt::format(
        "HTTP/1.0 {} {}\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n"
        "{}",
        status, status_text(status), content_type, body.size(), body);
}

} // namespace agentos::api

// ── AgentOS::start_api implementation ──────────────────────
namespace agentos {

Result<void> AgentOS::start_api(uint16_t port) {
    if (api_server_) {
        return make_error(ErrorCode::AlreadyExists, "API server already started");
    }
    api_server_ = std::make_unique<api::ApiServer>(*this, port);
    return api_server_->start();
}

} // namespace agentos
