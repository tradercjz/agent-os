// ============================================================
// Tests :: Dashboard — HTTP monitoring server
// ============================================================
#include <agentos/dashboard/dashboard.hpp>
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
using namespace agentos::dashboard;

// Helper: simple HTTP GET request, returns raw response body
static std::string http_get(uint16_t port, const std::string& path) {
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

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    (void)write(fd, req.data(), req.size());

    std::string response;
    std::array<char, 4096> buf{};
    ssize_t n = 0;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        response.append(buf.data(), static_cast<size_t>(n));
    }
    close(fd);

    // Strip HTTP headers — find \r\n\r\n
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end != std::string::npos) {
        return response.substr(hdr_end + 4);
    }
    return response;
}

TEST(DashboardTest, ConstructWithoutStart) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    EXPECT_FALSE(server.is_running());
}

TEST(DashboardTest, StartAndStop) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0); // OS-assigned port
    auto r = server.start();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(server.is_running());
    EXPECT_GT(server.port(), 0);

    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST(DashboardTest, DoubleStartReturnsError) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r1 = server.start();
    ASSERT_TRUE(r1.has_value());

    auto r2 = server.start();
    EXPECT_FALSE(r2.has_value());

    server.stop();
}

TEST(DashboardTest, DoubleStopIsSafe) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());

    server.stop();
    server.stop(); // Should not crash
    EXPECT_FALSE(server.is_running());
}

TEST(DashboardTest, ApiHealthEndpoint) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());

    // Give server thread a moment to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/health");
    EXPECT_FALSE(body.empty());

    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("healthy"));
    EXPECT_TRUE(j.contains("agents"));

    server.stop();
}

TEST(DashboardTest, ApiMetricsEndpoint) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/metrics");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("kernel"));
    EXPECT_TRUE(j.contains("scheduler"));

    server.stop();
}

TEST(DashboardTest, ApiTracesEndpoint) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/traces");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.is_array());

    server.stop();
}

TEST(DashboardTest, PrometheusMetricsEndpoint) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/metrics");
    EXPECT_FALSE(body.empty());
    // Prometheus format contains "# HELP" or "# TYPE" lines typically
    // At minimum it should return some text
    EXPECT_GT(body.size(), 0u);

    server.stop();
}

TEST(DashboardTest, IndexReturnsHtml) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/");
    EXPECT_NE(body.find("AgentOS Dashboard"), std::string::npos);
    EXPECT_NE(body.find("<html>"), std::string::npos);

    server.stop();
}

TEST(DashboardTest, NotFoundRoute) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/nonexistent");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("error", ""), "Not found");

    server.stop();
}

TEST(DashboardTest, ApiAgentsEndpoint) {
    auto os = quickstart_mock();
    DashboardServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto body = http_get(server.port(), "/api/agents");
    auto j = nlohmann::json::parse(body, nullptr, false);
    EXPECT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.is_array());

    server.stop();
}
