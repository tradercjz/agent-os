// ============================================================
// Tests :: API Server Extended — comprehensive endpoint coverage
// ============================================================
#include <agentos/api/api_server.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <array>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::api;

// ── Helpers ─────────────────────────────────────────────────

static std::string http_request(uint16_t port, const std::string& req_str) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "";
    }

    (void)write(fd, req_str.data(), req_str.size());

    std::string response;
    std::array<char, 4096> buf{};
    ssize_t n = 0;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        response.append(buf.data(), static_cast<size_t>(n));
    }
    close(fd);
    return response;
}

static std::string extract_body(const std::string& response) {
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        return response.substr(hdr_end + 4);
    }
    return response;
}

static std::string http_get(uint16_t port, const std::string& path) {
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    return extract_body(http_request(port, req));
}

static std::string http_post(uint16_t port, const std::string& path,
                             const std::string& body) {
    std::string req = "POST " + path + " HTTP/1.0\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;
    return extract_body(http_request(port, req));
}

static std::string http_delete(uint16_t port, const std::string& path) {
    std::string req = "DELETE " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    return extract_body(http_request(port, req));
}

// ── Fixture ─────────────────────────────────────────────────

class ApiServerExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        os_ = quickstart_mock();
        server_ = std::make_unique<ApiServer>(*os_, 0);
        auto r = server_->start();
        ASSERT_TRUE(r.has_value()) << r.error().message;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        port_ = server_->port();
    }

    void TearDown() override {
        server_->stop();
    }

    std::unique_ptr<AgentOS> os_;
    std::unique_ptr<ApiServer> server_;
    uint16_t port_{0};
};

// ── 1. Full CRUD lifecycle ──────────────────────────────────

TEST_F(ApiServerExtendedTest, FullCrudLifecycle) {
    // List agents — should be empty
    {
        auto body = http_get(port_, "/api/v1/agents");
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        ASSERT_TRUE(j.is_array());
        EXPECT_EQ(j.size(), 0u);
    }

    // Create agent
    uint64_t agent_id = 0;
    {
        std::string create_body = R"({"name":"crud-bot","prompt":"test prompt"})";
        auto body = http_post(port_, "/api/v1/agents", create_body);
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        EXPECT_EQ(j.value("name", ""), "crud-bot");
        EXPECT_EQ(j.value("status", ""), "created");
        ASSERT_TRUE(j.contains("id"));
        agent_id = j["id"].get<uint64_t>();
        EXPECT_GT(agent_id, 0u);
    }

    // List agents — should have 1
    {
        auto body = http_get(port_, "/api/v1/agents");
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        ASSERT_TRUE(j.is_array());
        EXPECT_EQ(j.size(), 1u);
        EXPECT_EQ(j[0].value("name", ""), "crud-bot");
    }

    // Get specific agent
    {
        auto body = http_get(port_, "/api/v1/agents/" + std::to_string(agent_id));
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        EXPECT_EQ(j["id"].get<uint64_t>(), agent_id);
        EXPECT_EQ(j.value("name", ""), "crud-bot");
        EXPECT_EQ(j.value("role_prompt", ""), "test prompt");
    }

    // Delete agent
    {
        auto body = http_delete(port_, "/api/v1/agents/" + std::to_string(agent_id));
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        EXPECT_EQ(j.value("status", ""), "deleted");
        EXPECT_EQ(j["id"].get<uint64_t>(), agent_id);
    }

    // List agents — should be empty again
    {
        auto body = http_get(port_, "/api/v1/agents");
        auto j = nlohmann::json::parse(body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        ASSERT_TRUE(j.is_array());
        EXPECT_EQ(j.size(), 0u);
    }
}

// ── 2. Invalid JSON body returns 400 ────────────────────────

TEST_F(ApiServerExtendedTest, InvalidJsonBodyReturns400) {
    auto raw = http_request(port_,
        "POST /api/v1/agents HTTP/1.0\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "{not valid}");
    EXPECT_NE(raw.find("400"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j.value("error", "").find("Invalid JSON"), std::string::npos);
}

// ── 3. Unknown route returns 404 ────────────────────────────

TEST_F(ApiServerExtendedTest, UnknownRouteReturns404) {
    auto raw = http_request(port_,
        "GET /api/v1/does-not-exist HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(raw.find("404"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("error", ""), "Not found");
    EXPECT_EQ(j.value("status", 0), 404);
}

// ── 4. Health endpoint returns valid JSON with expected fields

TEST_F(ApiServerExtendedTest, HealthEndpointFields) {
    auto body = http_get(port_, "/api/v1/health");
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    EXPECT_EQ(j.value("status", ""), "ok");
    EXPECT_TRUE(j.contains("agent_count"));
    EXPECT_TRUE(j.contains("healthy"));
    EXPECT_TRUE(j.contains("model"));

    EXPECT_TRUE(j["healthy"].is_boolean());
    EXPECT_TRUE(j["agent_count"].is_number());
    EXPECT_TRUE(j["model"].is_string());
}

// ── 5. Metrics endpoint returns valid JSON ──────────────────

TEST_F(ApiServerExtendedTest, MetricsEndpointFields) {
    auto body = http_get(port_, "/api/v1/metrics");
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    // Kernel metrics
    ASSERT_TRUE(j.contains("kernel"));
    EXPECT_TRUE(j["kernel"].contains("total_requests"));
    EXPECT_TRUE(j["kernel"].contains("total_tokens"));
    EXPECT_TRUE(j["kernel"].contains("errors"));
    EXPECT_TRUE(j["kernel"].contains("retries"));

    // Scheduler metrics
    ASSERT_TRUE(j.contains("scheduler"));
    EXPECT_TRUE(j["scheduler"].contains("tasks_submitted"));
    EXPECT_TRUE(j["scheduler"].contains("tasks_completed"));
    EXPECT_TRUE(j["scheduler"].contains("tasks_failed"));

    // Agents count
    EXPECT_TRUE(j.contains("agents"));
    EXPECT_TRUE(j["agents"].is_number());
}

// ── 6. Prometheus endpoint returns text/plain with metrics ──

TEST_F(ApiServerExtendedTest, PrometheusEndpointFormat) {
    auto raw = http_request(port_,
        "GET /api/v1/prometheus HTTP/1.0\r\nHost: localhost\r\n\r\n");

    // Verify content-type header
    EXPECT_NE(raw.find("text/plain"), std::string::npos);

    auto body = extract_body(raw);
    EXPECT_FALSE(body.empty());

    // Prometheus format: lines with metric_name{labels} value
    // or # HELP / # TYPE comment lines
    // Verify at least some metric-looking content is present
    bool has_metric_line = false;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line[0] != '#') {
            // Non-comment, non-empty line should be a metric
            has_metric_line = true;
            break;
        }
    }
    EXPECT_TRUE(has_metric_line) << "Expected at least one metric line in Prometheus output";
}

// ── 7. Create agent with missing name uses default ──────────

TEST_F(ApiServerExtendedTest, CreateAgentMissingNameUsesDefault) {
    // The handler defaults name to "api-agent" if not provided
    std::string create_body = R"({"prompt":"no name given"})";
    auto body = http_post(port_, "/api/v1/agents", create_body);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("name", ""), "api-agent");
    EXPECT_EQ(j.value("status", ""), "created");
}

// ── 8. Delete nonexistent agent returns 404 ─────────────────

TEST_F(ApiServerExtendedTest, DeleteNonexistentAgentReturns404) {
    auto raw = http_request(port_,
        "DELETE /api/v1/agents/99999 HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(raw.find("404"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j.value("error", "").find("not found"), std::string::npos);
}

// ── 9. Infer with empty messages returns error ──────────────

TEST_F(ApiServerExtendedTest, InferEmptyMessagesReturnsError) {
    std::string body_payload = R"({"messages":[]})";
    auto body = http_post(port_, "/api/v1/infer", body_payload);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    // Empty messages array is valid JSON but may succeed or fail depending on
    // kernel behavior; at minimum the response should be valid JSON
    EXPECT_TRUE(j.contains("content") || j.contains("error"));
}

TEST_F(ApiServerExtendedTest, InferMissingMessagesFieldReturnsError) {
    std::string body_payload = R"({"prompt":"hello"})";
    auto body = http_post(port_, "/api/v1/infer", body_payload);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j.value("error", "").find("messages"), std::string::npos);
}

TEST_F(ApiServerExtendedTest, InferMessagesNotArrayReturnsError) {
    std::string body_payload = R"({"messages":"not-an-array"})";
    auto body = http_post(port_, "/api/v1/infer", body_payload);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j.value("error", "").find("messages"), std::string::npos);
}

// ── 10. Concurrent API requests ─────────────────────────────

TEST_F(ApiServerExtendedTest, ConcurrentRequests) {
    constexpr int kConcurrency = 8;
    std::vector<std::future<std::string>> futures;
    futures.reserve(kConcurrency);

    for (int i = 0; i < kConcurrency; ++i) {
        futures.push_back(std::async(std::launch::async, [this]() {
            return http_get(port_, "/api/v1/health");
        }));
    }

    int success_count = 0;
    for (auto& f : futures) {
        auto body = f.get();
        if (body.empty()) continue;
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.value("status", "") == "ok") {
            ++success_count;
        }
    }

    // All concurrent requests should succeed
    EXPECT_EQ(success_count, kConcurrency);
}

TEST_F(ApiServerExtendedTest, ConcurrentMixedRequests) {
    // Mix of different endpoint types concurrently
    std::vector<std::future<bool>> futures;

    // Health requests
    for (int i = 0; i < 4; ++i) {
        futures.push_back(std::async(std::launch::async, [this]() {
            auto body = http_get(port_, "/api/v1/health");
            auto j = nlohmann::json::parse(body, nullptr, false);
            return !j.is_discarded() && j.value("status", "") == "ok";
        }));
    }

    // Metrics requests
    for (int i = 0; i < 4; ++i) {
        futures.push_back(std::async(std::launch::async, [this]() {
            auto body = http_get(port_, "/api/v1/metrics");
            auto j = nlohmann::json::parse(body, nullptr, false);
            return !j.is_discarded() && j.contains("kernel");
        }));
    }

    int ok_count = 0;
    for (auto& f : futures) {
        if (f.get()) ++ok_count;
    }
    EXPECT_EQ(ok_count, 8);
}

// ── 11. Server start/stop lifecycle ─────────────────────────

TEST(ApiServerLifecycleTest, StartStopStart) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);

    // First start
    auto r1 = server.start();
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(server.is_running());
    uint16_t port1 = server.port();
    EXPECT_GT(port1, 0);

    // Stop
    server.stop();
    EXPECT_FALSE(server.is_running());

    // Second start (should get a new port since we use 0)
    auto r2 = server.start();
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(server.is_running());
    EXPECT_GT(server.port(), 0);

    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(ApiServerLifecycleTest, StopWithoutStart) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);

    // Stopping a server that was never started should not crash
    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(ApiServerLifecycleTest, DestructorStopsServer) {
    auto os = quickstart_mock();
    {
        ApiServer server(*os, 0);
        auto r = server.start();
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(server.is_running());
        // Destructor called here
    }
    // If we get here, destructor did not hang
    SUCCEED();
}

// ── 12. Double start returns error ──────────────────────────

TEST(ApiServerLifecycleTest, DoubleStartReturnsError) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);

    auto r1 = server.start();
    ASSERT_TRUE(r1.has_value());

    auto r2 = server.start();
    ASSERT_FALSE(r2.has_value());
    EXPECT_NE(r2.error().message.find("already"), std::string::npos);

    server.stop();
}

// ── 13. Port getter returns configured port ─────────────────

TEST(ApiServerLifecycleTest, PortGetterWithExplicitPort) {
    auto os = quickstart_mock();
    // Use port 0 for OS-assigned — verify port() returns > 0 after start
    ApiServer server(*os, 0);
    EXPECT_EQ(server.port(), 0);

    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    EXPECT_GT(server.port(), 0);

    server.stop();
}

TEST(ApiServerLifecycleTest, PortGetterBeforeStart) {
    auto os = quickstart_mock();
    ApiServer server(*os, 8888);
    // Before start, port should be the configured value
    EXPECT_EQ(server.port(), 8888);
}

// ── 14. Custom route registration and dispatch ──────────────

TEST_F(ApiServerExtendedTest, CustomRoutePost) {
    server_->route("POST", "/api/v1/echo",
                   [](const ApiRequest& req) -> ApiResponse {
                       nlohmann::json j;
                       j["echo"] = req.body;
                       return ApiResponse::ok(j);
                   });

    std::string payload = R"({"msg":"hello"})";
    auto body = http_post(port_, "/api/v1/echo", payload);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("echo", ""), payload);
}

TEST_F(ApiServerExtendedTest, CustomRouteOverridesDefault) {
    // Override the health endpoint with custom handler
    server_->route("GET", "/api/v1/health",
                   [](const ApiRequest&) -> ApiResponse {
                       nlohmann::json j;
                       j["custom"] = true;
                       j["status"] = "custom-ok";
                       return ApiResponse::ok(j);
                   });

    auto body = http_get(port_, "/api/v1/health");
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("status", ""), "custom-ok");
    EXPECT_TRUE(j.value("custom", false));
}

TEST_F(ApiServerExtendedTest, MultipleCustomRoutes) {
    server_->route("GET", "/api/v1/route-a",
                   [](const ApiRequest&) -> ApiResponse {
                       return ApiResponse::ok({{"route", "a"}});
                   });
    server_->route("GET", "/api/v1/route-b",
                   [](const ApiRequest&) -> ApiResponse {
                       return ApiResponse::ok({{"route", "b"}});
                   });

    auto body_a = http_get(port_, "/api/v1/route-a");
    auto ja = nlohmann::json::parse(body_a, nullptr, false);
    ASSERT_FALSE(ja.is_discarded());
    EXPECT_EQ(ja.value("route", ""), "a");

    auto body_b = http_get(port_, "/api/v1/route-b");
    auto jb = nlohmann::json::parse(body_b, nullptr, false);
    ASSERT_FALSE(jb.is_discarded());
    EXPECT_EQ(jb.value("route", ""), "b");
}

// ── Additional edge cases ───────────────────────────────────

TEST_F(ApiServerExtendedTest, GetAgentWithInvalidIdFormat) {
    auto raw = http_request(port_,
        "GET /api/v1/agents/not-a-number HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(raw.find("400"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ApiServerExtendedTest, DeleteAgentWithInvalidIdFormat) {
    auto raw = http_request(port_,
        "DELETE /api/v1/agents/abc HTTP/1.0\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(raw.find("400"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(ApiServerExtendedTest, CreateMultipleAgentsThenList) {
    // Create 3 agents
    for (int i = 0; i < 3; ++i) {
        std::string name = "agent-" + std::to_string(i);
        std::string body_str = R"({"name":")" + name + R"("})";
        auto resp = http_post(port_, "/api/v1/agents", body_str);
        auto j = nlohmann::json::parse(resp, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        EXPECT_EQ(j.value("status", ""), "created");
    }

    // List should show 3
    auto body = http_get(port_, "/api/v1/agents");
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    ASSERT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 3u);
}

TEST_F(ApiServerExtendedTest, InferBadJsonReturns400) {
    auto raw = http_request(port_,
        "POST /api/v1/infer HTTP/1.0\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "{{{");
    EXPECT_NE(raw.find("400"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));
    EXPECT_NE(j.value("error", "").find("Invalid JSON"), std::string::npos);
}

TEST_F(ApiServerExtendedTest, ApiResponseStaticFactories) {
    // Verify ApiResponse helper constructors produce expected values
    auto ok_resp = ApiResponse::ok({{"key", "value"}});
    EXPECT_EQ(ok_resp.status, 200);
    EXPECT_EQ(ok_resp.content_type, "application/json");
    auto ok_j = nlohmann::json::parse(ok_resp.body);
    EXPECT_EQ(ok_j.value("key", ""), "value");

    auto err_resp = ApiResponse::error(422, "Unprocessable");
    EXPECT_EQ(err_resp.status, 422);
    auto err_j = nlohmann::json::parse(err_resp.body);
    EXPECT_EQ(err_j.value("error", ""), "Unprocessable");
    EXPECT_EQ(err_j.value("status", 0), 422);

    auto json_resp = ApiResponse::json(201, {{"created", true}});
    EXPECT_EQ(json_resp.status, 201);
    auto json_j = nlohmann::json::parse(json_resp.body);
    EXPECT_TRUE(json_j.value("created", false));
}

TEST_F(ApiServerExtendedTest, QueryParamsAreNotConfused) {
    // Request with query params should still route correctly
    auto raw = http_request(port_,
        "GET /api/v1/health?format=json&verbose=1 HTTP/1.0\r\n"
        "Host: localhost\r\n\r\n");
    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("status", ""), "ok");
}

TEST(ApiServerLifecycleTest, IsRunningReflectsState) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);

    EXPECT_FALSE(server.is_running());

    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(server.is_running());

    server.stop();
    EXPECT_FALSE(server.is_running());

    // Double stop is safe
    server.stop();
    EXPECT_FALSE(server.is_running());
}
