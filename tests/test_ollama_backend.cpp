#include <agentos/kernel/ollama_backend.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::kernel;

TEST(OllamaBackendTest, ConstructionDefaults) {
    OllamaBackend backend;
    EXPECT_EQ(backend.name(), "ollama/llama3");
}

TEST(OllamaBackendTest, ConstructionCustomModel) {
    OllamaBackend backend("mistral", "http://localhost:11434");
    EXPECT_EQ(backend.name(), "ollama/mistral");
}

TEST(OllamaBackendTest, BuildChatRequestBasic) {
    OllamaBackend backend("llama3");

    LLMRequest req;
    req.messages.push_back(Message::system("You are helpful."));
    req.messages.push_back(Message::user("Hello"));
    req.temperature = 0.5f;
    req.max_tokens = 100;

    Json j = backend.build_chat_request(req, false);

    EXPECT_EQ(j["model"], "llama3");
    EXPECT_EQ(j["stream"], false);
    ASSERT_TRUE(j["messages"].is_array());
    EXPECT_EQ(j["messages"].size(), 2u);
    EXPECT_EQ(j["messages"][0]["role"], "system");
    EXPECT_EQ(j["messages"][0]["content"], "You are helpful.");
    EXPECT_EQ(j["messages"][1]["role"], "user");
    EXPECT_EQ(j["messages"][1]["content"], "Hello");
    EXPECT_FLOAT_EQ(j["options"]["temperature"].get<float>(), 0.5f);
    EXPECT_EQ(j["options"]["num_predict"], 100);
}

TEST(OllamaBackendTest, BuildChatRequestStreaming) {
    OllamaBackend backend;
    LLMRequest req;
    req.messages.push_back(Message::user("hi"));

    Json j = backend.build_chat_request(req, true);
    EXPECT_EQ(j["stream"], true);
}

TEST(OllamaBackendTest, BuildChatRequestWithModelOverride) {
    OllamaBackend backend("llama3");
    LLMRequest req;
    req.model = "codellama";
    req.messages.push_back(Message::user("code"));

    Json j = backend.build_chat_request(req, false);
    EXPECT_EQ(j["model"], "codellama");
}

TEST(OllamaBackendTest, ParseChatResponseBasic) {
    OllamaBackend backend;

    std::string response_json = R"({
        "message": {"role": "assistant", "content": "Hello!"},
        "done": true,
        "eval_count": 10,
        "prompt_eval_count": 5
    })";

    auto r = backend.parse_chat_response(response_json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content, "Hello!");
    EXPECT_EQ(r->completion_tokens, 10u);
    EXPECT_EQ(r->prompt_tokens, 5u);
    EXPECT_EQ(r->finish_reason, "stop");
}

TEST(OllamaBackendTest, ParseChatResponseWithToolCalls) {
    OllamaBackend backend;

    std::string response_json = R"({
        "message": {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                {
                    "function": {
                        "name": "get_weather",
                        "arguments": {"city": "London"}
                    }
                }
            ]
        },
        "done": true,
        "eval_count": 20,
        "prompt_eval_count": 15
    })";

    auto r = backend.parse_chat_response(response_json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->finish_reason, "tool_calls");
    ASSERT_EQ(r->tool_calls.size(), 1u);
    EXPECT_EQ(r->tool_calls[0].name, "get_weather");
    EXPECT_FALSE(r->tool_calls[0].id.empty());

    // Verify args_json is valid JSON
    Json args = Json::parse(r->tool_calls[0].args_json);
    EXPECT_EQ(args["city"], "London");
}

TEST(OllamaBackendTest, ParseChatResponseError) {
    OllamaBackend backend;

    std::string response_json = R"({"error": "model not found"})";
    auto r = backend.parse_chat_response(response_json);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("model not found"), std::string::npos);
}

TEST(OllamaBackendTest, ParseChatResponseInvalidJson) {
    OllamaBackend backend;
    auto r = backend.parse_chat_response("not json at all");
    EXPECT_FALSE(r.has_value());
}

TEST(OllamaBackendTest, CompleteToUnreachableServer) {
    OllamaBackend backend("llama3", "http://localhost:1");
    LLMRequest req;
    req.messages.push_back(Message::user("hello"));

    auto r = backend.complete(req);
    EXPECT_FALSE(r.has_value());
}

TEST(OllamaBackendTest, StreamToUnreachableServer) {
    OllamaBackend backend("llama3", "http://localhost:1");
    LLMRequest req;
    req.messages.push_back(Message::user("hello"));

    auto r = backend.stream(req, [](std::string_view) {});
    EXPECT_FALSE(r.has_value());
}

TEST(OllamaBackendTest, EmbedToUnreachableServer) {
    OllamaBackend backend("llama3", "http://localhost:1");
    EmbeddingRequest req;
    req.inputs.push_back("test text");

    auto r = backend.embed(req);
    EXPECT_FALSE(r.has_value());
}

TEST(OllamaBackendTest, BuildChatRequestWithTools) {
    OllamaBackend backend;
    LLMRequest req;
    req.messages.push_back(Message::user("weather?"));
    req.tools_json = R"([{"type":"function","function":{"name":"weather","parameters":{}}}])";

    Json j = backend.build_chat_request(req, false);
    ASSERT_TRUE(j.contains("tools"));
    EXPECT_TRUE(j["tools"].is_array());
    EXPECT_EQ(j["tools"].size(), 1u);
}
