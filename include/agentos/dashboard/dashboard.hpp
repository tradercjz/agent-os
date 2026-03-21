#pragma once
// ============================================================
// AgentOS :: Dashboard — Lightweight HTTP monitoring server
// ============================================================
#include <agentos/core/types.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace agentos {

class AgentOS; // forward

namespace dashboard {

class DashboardServer : private NonCopyable {
public:
    explicit DashboardServer(AgentOS& os, uint16_t port = 8080);
    ~DashboardServer();

    Result<void> start();
    void stop();
    bool is_running() const noexcept { return running_.load(); }
    uint16_t port() const noexcept { return port_; }

private:
    void serve_loop();
    void handle_client(int client_fd);

    std::string handle_request(const std::string& method, const std::string& path);

    // Route handlers
    std::string route_index();
    std::string route_api_health();
    std::string route_api_metrics();
    std::string route_api_agents();
    std::string route_api_traces();
    std::string route_metrics_prometheus();

    // HTTP helpers
    static std::string http_response(int status, const std::string& content_type,
                                     const std::string& body);
    static std::string http_json(int status, const std::string& json_body);
    static std::string http_html(int status, const std::string& html_body);

    AgentOS& os_;
    uint16_t port_;
    int server_fd_{-1};
    std::jthread server_thread_;
    std::atomic<bool> running_{false};
};

} // namespace dashboard
} // namespace agentos
