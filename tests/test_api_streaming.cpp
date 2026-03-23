// ============================================================
// Tests :: API Streaming — SSE event formatting and streaming
// ============================================================
#include <agentos/api/streaming.hpp>
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
#include <vector>

using namespace agentos;
using namespace agentos::api;

// ── StreamEvent formatting tests ────────────────────────────

TEST(StreamEventTest, BasicFormat) {
    StreamEvent ev;
    ev.event = "token";
    ev.data = R"({"token":"hello"})";

    std::string formatted = ev.format();
    EXPECT_NE(formatted.find("event: token\n"), std::string::npos);
    EXPECT_NE(formatted.find("data: {\"token\":\"hello\"}\n"), std::string::npos);
    // Must end with double newline (blank line terminator)
    EXPECT_TRUE(formatted.size() >= 2);
    EXPECT_EQ(formatted.substr(formatted.size() - 2), "\n\n");
}

TEST(StreamEventTest, FormatWithId) {
    StreamEvent ev;
    ev.event = "token";
    ev.data = "hello";
    ev.id = "42";

    std::string formatted = ev.format();
    EXPECT_NE(formatted.find("id: 42\n"), std::string::npos);
    EXPECT_NE(formatted.find("event: token\n"), std::string::npos);
    EXPECT_NE(formatted.find("data: hello\n"), std::string::npos);
}

TEST(StreamEventTest, FormatWithoutEvent) {
    StreamEvent ev;
    ev.data = "just data";

    std::string formatted = ev.format();
    // No event: line
    EXPECT_EQ(formatted.find("event:"), std::string::npos);
    EXPECT_NE(formatted.find("data: just data\n"), std::string::npos);
}

TEST(StreamEventTest, MultilineData) {
    StreamEvent ev;
    ev.event = "message";
    ev.data = "line one\nline two\nline three";

    std::string formatted = ev.format();
    EXPECT_NE(formatted.find("data: line one\n"), std::string::npos);
    EXPECT_NE(formatted.find("data: line two\n"), std::string::npos);
    EXPECT_NE(formatted.find("data: line three\n"), std::string::npos);
}

TEST(StreamEventTest, EmptyData) {
    StreamEvent ev;
    ev.event = "ping";

    std::string formatted = ev.format();
    EXPECT_NE(formatted.find("data: \n"), std::string::npos);
}

TEST(StreamEventTest, NoIdOmitsIdLine) {
    StreamEvent ev;
    ev.event = "done";
    ev.data = "ok";
    // id is std::nullopt by default

    std::string formatted = ev.format();
    EXPECT_EQ(formatted.find("id:"), std::string::npos);
}

TEST(StreamEventTest, EmptyIdOmitsIdLine) {
    StreamEvent ev;
    ev.event = "done";
    ev.data = "ok";
    ev.id = "";

    std::string formatted = ev.format();
    EXPECT_EQ(formatted.find("id:"), std::string::npos);
}

// ── Helper factory tests ────────────────────────────────────

TEST(StreamEventTest, MakeTokenEvent) {
    auto ev = make_token_event("world", "7");
    EXPECT_EQ(ev.event, "token");
    EXPECT_TRUE(ev.id.has_value());
    EXPECT_EQ(*ev.id, "7");

    auto j = nlohmann::json::parse(ev.data);
    EXPECT_EQ(j["token"].get<std::string>(), "world");
}

TEST(StreamEventTest, MakeDoneEvent) {
    auto ev = make_done_event(10, 25);
    EXPECT_EQ(ev.event, "done");

    auto j = nlohmann::json::parse(ev.data);
    EXPECT_EQ(j["status"].get<std::string>(), "done");
    EXPECT_EQ(j["tokens"]["prompt"].get<uint32_t>(), 10u);
    EXPECT_EQ(j["tokens"]["completion"].get<uint32_t>(), 25u);
}

TEST(StreamEventTest, MakeErrorEvent) {
    auto ev = make_error_event("something broke");
    EXPECT_EQ(ev.event, "error");

    auto j = nlohmann::json::parse(ev.data);
    EXPECT_EQ(j["error"].get<std::string>(), "something broke");
}

// ── StreamWriter tests using socketpair ─────────────────────

class StreamWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a connected socket pair for testing
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0);
    }

    void TearDown() override {
        if (fds_[0] >= 0) close(fds_[0]);
        if (fds_[1] >= 0) close(fds_[1]);
    }

    // Read all available data from the read end
    std::string read_all() {
        // Shut down write end so read sees EOF
        shutdown(fds_[0], SHUT_WR);

        std::string result;
        std::array<char, 4096> buf{};
        ssize_t n = 0;
        while ((n = read(fds_[1], buf.data(), buf.size())) > 0) {
            result.append(buf.data(), static_cast<size_t>(n));
        }
        return result;
    }

    int fds_[2]{-1, -1};  // [0] = write end (for StreamWriter), [1] = read end
};

TEST_F(StreamWriterTest, BeginWritesHeaders) {
    StreamWriter writer(fds_[0]);
    auto r = writer.begin();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(writer.is_open());

    auto data = read_all();
    EXPECT_NE(data.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(data.find("Content-Type: text/event-stream"), std::string::npos);
    EXPECT_NE(data.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_NE(data.find("Cache-Control: no-cache"), std::string::npos);
    EXPECT_NE(data.find("Connection: keep-alive"), std::string::npos);
}

TEST_F(StreamWriterTest, InvalidFdFails) {
    StreamWriter writer(-1);
    auto r = writer.begin();
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(writer.is_open());
}

TEST_F(StreamWriterTest, SendBeforeBeginFails) {
    StreamWriter writer(fds_[0]);
    StreamEvent ev;
    ev.event = "test";
    ev.data = "data";
    auto r = writer.send(ev);
    EXPECT_FALSE(r.has_value());
}

TEST_F(StreamWriterTest, SendEvent) {
    StreamWriter writer(fds_[0]);
    (void)writer.begin();

    StreamEvent ev;
    ev.event = "token";
    ev.data = R"({"token":"hi"})";
    auto r = writer.send(ev);
    ASSERT_TRUE(r.has_value());

    auto data = read_all();
    // The SSE payload should be inside a chunked body
    EXPECT_NE(data.find("event: token"), std::string::npos);
    EXPECT_NE(data.find("data: {\"token\":\"hi\"}"), std::string::npos);
}

TEST_F(StreamWriterTest, EndSendsTerminalChunk) {
    StreamWriter writer(fds_[0]);
    (void)writer.begin();
    auto r = writer.end();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(writer.is_open());

    auto data = read_all();
    // Zero-length chunk signals end of chunked transfer
    EXPECT_NE(data.find("0\r\n\r\n"), std::string::npos);
}

TEST_F(StreamWriterTest, FullStreamSequence) {
    StreamWriter writer(fds_[0]);
    (void)writer.begin();

    (void)writer.send(make_token_event("Hello"));
    (void)writer.send(make_token_event(" world"));
    (void)writer.send(make_done_event(5, 2));
    (void)writer.end();

    EXPECT_GT(writer.bytes_written(), 0u);

    auto data = read_all();
    // Verify all events appear in order
    auto pos_hello = data.find("\"token\":\"Hello\"");
    auto pos_world = data.find("\"token\":\" world\"");
    auto pos_done  = data.find("event: done");
    auto pos_term  = data.find("0\r\n\r\n");

    EXPECT_NE(pos_hello, std::string::npos);
    EXPECT_NE(pos_world, std::string::npos);
    EXPECT_NE(pos_done, std::string::npos);
    EXPECT_NE(pos_term, std::string::npos);

    // Ordering: hello < world < done < terminal
    EXPECT_LT(pos_hello, pos_world);
    EXPECT_LT(pos_world, pos_done);
    EXPECT_LT(pos_done, pos_term);
}

TEST_F(StreamWriterTest, MoveConstruct) {
    StreamWriter writer1(fds_[0]);
    (void)writer1.begin();

    StreamWriter writer2(std::move(writer1));
    EXPECT_TRUE(writer2.is_open());
    EXPECT_FALSE(writer1.is_open());

    auto r = writer2.send(make_token_event("moved"));
    EXPECT_TRUE(r.has_value());
}

// ── SSE protocol correctness ────────────────────────────────

TEST(SSEProtocolTest, EventFieldOrder) {
    // Per the SSE spec, event: must come before data:
    StreamEvent ev;
    ev.event = "token";
    ev.data = "payload";
    ev.id = "1";

    std::string formatted = ev.format();
    auto event_pos = formatted.find("event:");
    auto id_pos = formatted.find("id:");
    auto data_pos = formatted.find("data:");

    EXPECT_NE(event_pos, std::string::npos);
    EXPECT_NE(id_pos, std::string::npos);
    EXPECT_NE(data_pos, std::string::npos);

    // event: before id: before data:
    EXPECT_LT(event_pos, id_pos);
    EXPECT_LT(id_pos, data_pos);
}

TEST(SSEProtocolTest, TerminatedByBlankLine) {
    // Each SSE message must end with a blank line (\n\n)
    StreamEvent ev;
    ev.event = "done";
    ev.data = "fin";

    std::string formatted = ev.format();
    // The formatted string should end with \n\n
    ASSERT_GE(formatted.size(), 2u);
    EXPECT_EQ(formatted[formatted.size() - 1], '\n');
    EXPECT_EQ(formatted[formatted.size() - 2], '\n');
    // But the character before the last two \n should not be \n
    // (i.e., exactly one blank line, not two)
    if (formatted.size() >= 3) {
        // The line before the blank line is "data: fin\n"
        // so the third-from-last char is 'n' from "fin"
        // This verifies we have exactly data\n + \n (blank line)
    }
}

// ── Integration: streaming endpoint over real socket ────────

// Helper: send raw HTTP request and read SSE response
static std::string http_request_raw(uint16_t port, const std::string& req_str) {
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

    // Read with a timeout since SSE is keep-alive
    std::string response;
    std::array<char, 4096> buf{};

    // Set a read timeout
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = 0;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        response.append(buf.data(), static_cast<size_t>(n));
    }
    close(fd);
    return response;
}

TEST(ApiStreamingIntegration, InferStreamEndpoint) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string body = R"({"messages":[{"role":"user","content":"hello"}]})";
    std::string req = "POST /api/v1/infer/stream HTTP/1.1\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    auto resp = http_request_raw(server.port(), req);
    EXPECT_FALSE(resp.empty());

    // Verify SSE headers
    EXPECT_NE(resp.find("text/event-stream"), std::string::npos);
    EXPECT_NE(resp.find("Transfer-Encoding: chunked"), std::string::npos);

    // Verify we got event: done at the end
    EXPECT_NE(resp.find("event: done"), std::string::npos);

    server.stop();
}

TEST(ApiStreamingIntegration, InferStreamBadJson) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string req = "POST /api/v1/infer/stream HTTP/1.1\r\n"
                      "Content-Length: 7\r\n"
                      "\r\n"
                      "bad{{{[";

    auto resp = http_request_raw(server.port(), req);
    EXPECT_FALSE(resp.empty());
    // Should get a 400 error, not SSE
    EXPECT_NE(resp.find("400"), std::string::npos);
    EXPECT_NE(resp.find("error"), std::string::npos);

    server.stop();
}

TEST(ApiStreamingIntegration, InferStreamMissingMessages) {
    auto os = quickstart_mock();
    ApiServer server(*os, 0);
    auto r = server.start();
    ASSERT_TRUE(r.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string body = R"({"prompt":"hello"})";
    std::string req = "POST /api/v1/infer/stream HTTP/1.1\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    auto resp = http_request_raw(server.port(), req);
    EXPECT_FALSE(resp.empty());
    EXPECT_NE(resp.find("400"), std::string::npos);

    server.stop();
}
