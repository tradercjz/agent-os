#include <agentos/kernel/anthropic_backend.hpp>
#include <gtest/gtest.h>

using namespace agentos::kernel;

TEST(AnthropicBackendTest, NameReturnsAnthropic) {
    AnthropicBackend backend("test-key");
    EXPECT_EQ(backend.name(), "anthropic");
}

TEST(AnthropicBackendTest, BuildRequestExtractsSystemPrompt) {
    AnthropicBackend backend("test-key");
    LLMRequest req;
    req.messages.push_back(Message::system("You are helpful"));
    req.messages.push_back(Message::user("Hello"));

    auto j = backend.build_request(req);
    EXPECT_EQ(j["system"], "You are helpful");
    EXPECT_EQ(j["messages"].size(), 1u);
    EXPECT_EQ(j["messages"][0]["role"], "user");
}

TEST(AnthropicBackendTest, BuildRequestSetsModel) {
    AnthropicBackend backend("key", "claude-sonnet-4-20250514");
    LLMRequest req;
    req.messages.push_back(Message::user("hi"));
    auto j = backend.build_request(req);
    EXPECT_EQ(j["model"], "claude-sonnet-4-20250514");
    EXPECT_TRUE(j.contains("max_tokens"));
}

TEST(AnthropicBackendTest, ParseTextResponse) {
    AnthropicBackend backend("key");
    std::string body = R"({
        "content": [{"type": "text", "text": "Hello world!"}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })";

    auto r = backend.parse_response(body);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content, "Hello world!");
    EXPECT_EQ(r->prompt_tokens, 10u);
    EXPECT_EQ(r->completion_tokens, 5u);
    EXPECT_EQ(r->finish_reason, "end_turn");
    EXPECT_FALSE(r->wants_tool_call());
}

TEST(AnthropicBackendTest, ParseToolUseResponse) {
    AnthropicBackend backend("key");
    std::string body = R"({
        "content": [
            {"type": "text", "text": "I'll check the weather."},
            {"type": "tool_use", "id": "toolu_01", "name": "get_weather", "input": {"city": "SF"}}
        ],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 15, "output_tokens": 20}
    })";

    auto r = backend.parse_response(body);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content, "I'll check the weather.");
    EXPECT_TRUE(r->wants_tool_call());
    ASSERT_EQ(r->tool_calls.size(), 1u);
    EXPECT_EQ(r->tool_calls[0].name, "get_weather");
    EXPECT_EQ(r->tool_calls[0].id, "toolu_01");
    auto args = nlohmann::json::parse(r->tool_calls[0].args_json);
    EXPECT_EQ(args["city"], "SF");
    EXPECT_EQ(r->finish_reason, "tool_calls");
}

TEST(AnthropicBackendTest, ParseMultipleToolCalls) {
    AnthropicBackend backend("key");
    std::string body = R"({
        "content": [
            {"type": "tool_use", "id": "t1", "name": "search", "input": {"q": "hello"}},
            {"type": "tool_use", "id": "t2", "name": "fetch", "input": {"url": "http://x"}}
        ],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 5, "output_tokens": 10}
    })";

    auto r = backend.parse_response(body);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->tool_calls.size(), 2u);
    EXPECT_TRUE(r->wants_tool_call());
}

TEST(AnthropicBackendTest, ParseErrorResponse) {
    AnthropicBackend backend("key");
    std::string body = R"({"error": {"message": "invalid api key", "type": "authentication_error"}})";

    auto r = backend.parse_response(body);
    EXPECT_FALSE(r.has_value());
}

TEST(AnthropicBackendTest, ParseInvalidJson) {
    AnthropicBackend backend("key");
    auto r = backend.parse_response("not json{{{");
    EXPECT_FALSE(r.has_value());
}

TEST(AnthropicBackendTest, CompleteToUnreachableServer) {
    AnthropicBackend backend("key", "claude-sonnet-4-20250514", "http://localhost:1");
    LLMRequest req;
    req.messages.push_back(Message::user("hello"));
    auto r = backend.complete(req);
    EXPECT_FALSE(r.has_value());
}
