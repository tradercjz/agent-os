// ============================================================
// Tests :: API Server — REST API with agent management
// ============================================================
#include <agentos/api/api_server.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <array>
#include <string>
#include <thread>

using namespace agentos;
using namespace agentos::api;

// Helper: send raw HTTP request, return full response including headers
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

// Helper: extract body after HTTP headers
static std::string extract_body(const std::string& response) {
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        return response.substr(hdr_end + 4);
    }
    return response;
}

// Helper: send GET and return body
static std::string http_get(uint16_t port, const std::string& path) {
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    return extract_body(http_request(port, req));
}

// Helper: send POST and return body
static std::string http_post(uint16_t port, const std::string& path,
                             const std::string& body) {
    std::string req = "POST " + path + " HTTP/1.0\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;
    return extract_body(http_request(port, req));
}

// Helper: send DELETE and return body
static std::string http_delete(uint16_t port, const std::string& path) {
    std::string req = "DELETE " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    return extract_body(http_request(port, req));
}

TEST(ApiServerTest, ConstructWithoutStart) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    EXPECT_FALSE(server.is_running());
}

TEST(ApiServerTest, StartAndStop) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(server.is_running());
    EXPECT_GT(server.port(), 0);

    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(ApiServerTest, DoubleStartReturnsError) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r1 = server.start();
    ASSERT_TRUE(r1.has_value());

    auto r2 = server.start();
    EXPECT_FALSE(r2.has_value());

    server.stop();
}

TEST(ApiServerTest, HealthEndpoint) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/v1/health");
    EXPECT_FALSE(body.empty());

    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("status", ""), "ok");
    EXPECT_TRUE(j.contains("agent_count"));

    server.stop();
}

TEST(ApiServerTest, MetricsEndpoint) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/v1/metrics");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("kernel"));
    EXPECT_TRUE(j.contains("scheduler"));

    server.stop();
}

TEST(ApiServerTest, ListAgentsEmpty) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/v1/agents");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);

    server.stop();
}

TEST(ApiServerTest, CreateAndListAgent) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Create agent
    std::string create_body = R"({"name":"test-bot","prompt":"You are a test bot."})";
    auto create_resp = http_post(server.port(), "/api/v1/agents", create_body);
    auto cj = nlohmann::json::parse(create_resp, nullptr, false);
    EXPECT_FALSE(cj.is_discarded());
    EXPECT_EQ(cj.value("name", ""), "test-bot");
    EXPECT_EQ(cj.value("status", ""), "created");
    EXPECT_TRUE(cj.contains("id"));

    // List agents
    auto list_body = http_get(server.port(), "/api/v1/agents");
    auto lj = nlohmann::json::parse(list_body, nullptr, false);
    EXPECT_FALSE(lj.is_discarded());
    EXPECT_TRUE(lj.is_array());
    EXPECT_GE(lj.size(), 1u);

    server.stop();
}

TEST(ApiServerTest, GetAgent) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "lookup-bot").prompt("Hello").create();
    AgentId aid = agent->id();

    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(),
                         "/api/v1/agents/" + std::to_string(aid));
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("name", ""), "lookup-bot");
    EXPECT_EQ(j["id"].get<AgentId>(), aid);

    server.stop();
}

TEST(ApiServerTest, DeleteAgent) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "doomed-bot").create();
    AgentId aid = agent->id();
    agent.reset(); // release our reference

    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_delete(server.port(),
                            "/api/v1/agents/" + std::to_string(aid));
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("status", ""), "deleted");

    // Verify agent is gone
    EXPECT_EQ(os->find_agent(aid), nullptr);

    server.stop();
}

TEST(ApiServerTest, InferEndpoint) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string body = R"({"messages":[{"role":"user","content":"hello"}]})";
    auto resp = http_post(server.port(), "/api/v1/infer", body);
    auto j = nlohmann::json::parse(resp, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("content"));
    EXPECT_TRUE(j.contains("tokens"));

    server.stop();
}

TEST(ApiServerTest, InferBadJson) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = http_post(server.port(), "/api/v1/infer", "not-json{{{");
    auto j = nlohmann::json::parse(resp, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("error"));

    server.stop();
}

TEST(ApiServerTest, NotFoundReturns404) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto raw = http_request(server.port(),
                            "GET /nonexistent HTTP/1.0\r\n\r\n");
    EXPECT_NE(raw.find("404"), std::string::npos);

    auto body = extract_body(raw);
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("error", ""), "Not found");

    server.stop();
}

TEST(ApiServerTest, GetNonexistentAgent) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto raw = http_request(server.port(),
                            "GET /api/v1/agents/99999 HTTP/1.0\r\n\r\n");
    EXPECT_NE(raw.find("404"), std::string::npos);

    server.stop();
}

TEST(ApiServerTest, PrometheusEndpoint) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/v1/prometheus");
    EXPECT_FALSE(body.empty());
    EXPECT_GT(body.size(), 0u);

    server.stop();
}

TEST(ApiServerTest, CustomRoute) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);

    server.route("GET", "/api/v1/ping",
                 [](const ApiRequest&) -> ApiResponse {
                     nlohmann::json j;
                     j["pong"] = true;
                     return ApiResponse::ok(j);
                 });

    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/v1/ping");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.value("pong", false));

    server.stop();
}

TEST(ApiServerTest, StartApiOnAgentOS) {
    auto os = quickstart_mock();
    auto r = os->start_api(0);
    if (r.has_value()) {
        // If start succeeded, the server should be running
        // We just verify it doesn't crash
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Destructor cleans up
}
