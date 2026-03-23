#pragma once
// ============================================================
// AgentOS :: API — REST API server with agent management
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/api/streaming.hpp>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace agentos {

class AgentOS;

namespace api {

struct ApiRequest {
    std::string method;  // GET, POST, DELETE
    std::string path;    // /api/v1/agents
    std::string body;    // JSON body for POST
    std::unordered_map<std::string, std::string> query_params;
};

struct ApiResponse {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;

    static ApiResponse json(int status_code, const nlohmann::json& j) {
        return {status_code, "application/json", j.dump()};
    }
    static ApiResponse error(int status_code, const std::string& message) {
        nlohmann::json j;
        j["error"] = message;
        j["status"] = status_code;
        return {status_code, "application/json", j.dump()};
    }
    static ApiResponse ok(const nlohmann::json& j) { return json(200, j); }
};

using RouteHandler = std::function<ApiResponse(const ApiRequest&)>;

class ApiServer : private NonCopyable {
public:
    explicit ApiServer(AgentOS& os, uint16_t port = 9090);
    ~ApiServer();

    Result<void> start();
    void stop();
    bool is_running() const noexcept { return running_.load(); }
    uint16_t port() const noexcept { return port_; }

    // Register custom route
    void route(const std::string& method, const std::string& path,
               RouteHandler handler);

private:
    void serve_loop();
    void handle_client(int client_fd);
    ApiRequest parse_request(const std::string& raw);
    ApiResponse dispatch(const ApiRequest& req);
    bool is_stream_route(const std::string& method, const std::string& path) const;

    // Built-in routes
    void register_default_routes();

    // Route handlers
    ApiResponse handle_list_agents(const ApiRequest& req);
    ApiResponse handle_create_agent(const ApiRequest& req);
    ApiResponse handle_get_agent(const ApiRequest& req);
    ApiResponse handle_delete_agent(const ApiRequest& req);
    ApiResponse handle_infer(const ApiRequest& req);
    void handle_infer_stream(int client_fd, const ApiRequest& req);
    ApiResponse handle_health(const ApiRequest& req);
    ApiResponse handle_metrics(const ApiRequest& req);
    ApiResponse handle_prometheus(const ApiRequest& req);

    // HTTP helpers
    static std::string http_response(int status, const std::string& content_type,
                                     const std::string& body);
    static std::string status_text(int status);

    AgentOS& os_;
    uint16_t port_;
    int server_fd_{-1};
    std::jthread server_thread_;
    std::atomic<bool> running_{false};

    std::mutex routes_mu_;
    // key = "METHOD /path"
    std::unordered_map<std::string, RouteHandler> routes_;
};

} // namespace api
} // namespace agentos
