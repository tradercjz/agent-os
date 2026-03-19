#include <agentos/mcp/mcp_server.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::mcp;

class MCPServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register a test tool
        tools::ToolSchema schema;
        schema.id = "echo";
        schema.description = "Echo back input";
        schema.params.push_back({
            .name = "message",
            .type = tools::ParamType::String,
            .description = "Message to echo",
            .required = true,
        });

        registry_.register_fn(schema,
            [](const tools::ParsedArgs& args, std::stop_token) {
                return tools::ToolResult::ok(args.values.count("message")
                    ? args.values.at("message") : "no message");
            });

        server_ = std::make_unique<MCPServer>(registry_, "test-server", "1.0.0");
    }

    tools::ToolRegistry registry_;
    std::unique_ptr<MCPServer> server_;
};

// ── Initialize ──

TEST_F(MCPServerTest, InitializeReturnsServerInfo) {
    MCPRequest req;
    req.method = "initialize";
    req.id = 1;

    auto resp = server_->handle(req);
    EXPECT_TRUE(resp.error.is_null());
    EXPECT_EQ(resp.result["serverInfo"]["name"], "test-server");
    EXPECT_EQ(resp.result["serverInfo"]["version"], "1.0.0");
    EXPECT_EQ(resp.id, 1);
}

// ── Ping ──

TEST_F(MCPServerTest, PingReturnsEmptyObject) {
    MCPRequest req;
    req.method = "ping";
    req.id = 2;

    auto resp = server_->handle(req);
    EXPECT_TRUE(resp.error.is_null());
    EXPECT_TRUE(resp.result.is_object());
}

// ── Tools list ──

TEST_F(MCPServerTest, ToolsListReturnsTool) {
    MCPRequest req;
    req.method = "tools/list";
    req.id = 3;

    auto resp = server_->handle(req);
    EXPECT_TRUE(resp.error.is_null());
    ASSERT_TRUE(resp.result["tools"].is_array());
    EXPECT_EQ(resp.result["tools"].size(), 1u);
    EXPECT_EQ(resp.result["tools"][0]["name"], "echo");
    EXPECT_EQ(resp.result["tools"][0]["description"], "Echo back input");
    EXPECT_TRUE(resp.result["tools"][0]["inputSchema"]["properties"].contains("message"));
}

// ── Tools call ──

TEST_F(MCPServerTest, ToolsCallDispatchesTool) {
    MCPRequest req;
    req.method = "tools/call";
    req.params = {{"name", "echo"}, {"arguments", {{"message", "hello"}}}};
    req.id = 4;

    auto resp = server_->handle(req);
    EXPECT_TRUE(resp.error.is_null());
    EXPECT_FALSE(resp.result["isError"].get<bool>());
    EXPECT_EQ(resp.result["content"][0]["text"], "hello");
}

TEST_F(MCPServerTest, ToolsCallMissingNameReturnsError) {
    MCPRequest req;
    req.method = "tools/call";
    req.params = Json::object();
    req.id = 5;

    auto resp = server_->handle(req);
    EXPECT_FALSE(resp.error.is_null());
    EXPECT_EQ(resp.error["code"], error_code::InvalidParams);
}

TEST_F(MCPServerTest, ToolsCallUnknownToolReturnsError) {
    MCPRequest req;
    req.method = "tools/call";
    req.params = {{"name", "nonexistent"}};
    req.id = 6;

    auto resp = server_->handle(req);
    EXPECT_FALSE(resp.error.is_null());
    EXPECT_EQ(resp.error["code"], error_code::InvalidParams);
}

// ── Unknown method ──

TEST_F(MCPServerTest, UnknownMethodReturnsError) {
    MCPRequest req;
    req.method = "resources/list";
    req.id = 7;

    auto resp = server_->handle(req);
    EXPECT_FALSE(resp.error.is_null());
    EXPECT_EQ(resp.error["code"], error_code::MethodNotFound);
}

// ── JSON string handling ──

TEST_F(MCPServerTest, HandleJsonString) {
    std::string json = R"({"jsonrpc":"2.0","method":"ping","id":8})";
    auto resp = server_->handle_json(json);
    EXPECT_TRUE(resp.error.is_null());
    EXPECT_EQ(resp.id, 8);
}

TEST_F(MCPServerTest, HandleJsonInvalidParse) {
    auto resp = server_->handle_json("not json{{{");
    EXPECT_FALSE(resp.error.is_null());
    EXPECT_EQ(resp.error["code"], error_code::ParseError);
}

TEST_F(MCPServerTest, HandleJsonMissingMethod) {
    auto resp = server_->handle_json(R"({"jsonrpc":"2.0","id":9})");
    EXPECT_FALSE(resp.error.is_null());
    EXPECT_EQ(resp.error["code"], error_code::InvalidRequest);
}

// ── Response serialization ──

TEST_F(MCPServerTest, ResponseToJson) {
    MCPRequest req;
    req.method = "ping";
    req.id = 10;

    auto resp = server_->handle(req);
    std::string json_str = resp.to_json();
    auto parsed = Json::parse(json_str);
    EXPECT_EQ(parsed["jsonrpc"], "2.0");
    EXPECT_EQ(parsed["id"], 10);
}
