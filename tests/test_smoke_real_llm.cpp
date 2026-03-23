// ============================================================
// AgentOS :: Smoke Tests for Real LLM APIs
//
// These tests are gated behind AGENTOS_SMOKE_TEST=1 and require
// real API keys (OPENAI_API_KEY, ANTHROPIC_API_KEY).
// They are NOT run in normal CI — only on-demand for integration
// verification against live endpoints.
// ============================================================
#include <agentos/kernel/anthropic_backend.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace agentos;
using namespace agentos::kernel;

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

static bool smoke_test_enabled() {
    const char* val = std::getenv("AGENTOS_SMOKE_TEST");
    return val != nullptr && std::string(val) == "1";
}

static std::string get_env(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string{};
}

// Shared tools_json for tool-calling tests (OpenAI function-calling format)
static const std::string kToolsJson = R"([
  {
    "type": "function",
    "function": {
      "name": "get_weather",
      "description": "Get the current weather for a given city.",
      "parameters": {
        "type": "object",
        "properties": {
          "city": {
            "type": "string",
            "description": "City name, e.g. San Francisco"
          }
        },
        "required": ["city"]
      }
    }
  }
])";

// ─────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────

class SmokeRealLLMTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!smoke_test_enabled()) {
            GTEST_SKIP() << "AGENTOS_SMOKE_TEST=1 not set; skipping real LLM smoke tests";
        }
    }
};

// ─────────────────────────────────────────────────────────────
// OpenAI Smoke Tests
// ─────────────────────────────────────────────────────────────

TEST_F(SmokeRealLLMTest, OpenAI_SimpleCompletion) {
    auto api_key = get_env("OPENAI_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "OPENAI_API_KEY not set";
    }

    OpenAIBackend backend(api_key, "https://api.openai.com/v1", "gpt-4o-mini");

    auto req = LLMRequest::builder()
        .system("You are a helpful assistant. Reply in one short sentence.")
        .user("What is 2 + 2?")
        .model("gpt-4o-mini")
        .temperature(0.0f)
        .max_tokens(64)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "OpenAI completion failed: " << result.error().message;

    EXPECT_FALSE(result->content.empty())
        << "Expected non-empty response content";
    EXPECT_EQ(result->finish_reason, "stop");
}

TEST_F(SmokeRealLLMTest, OpenAI_TokenCounting) {
    auto api_key = get_env("OPENAI_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "OPENAI_API_KEY not set";
    }

    OpenAIBackend backend(api_key, "https://api.openai.com/v1", "gpt-4o-mini");

    auto req = LLMRequest::builder()
        .system("Reply with exactly: Hello.")
        .user("Say hello.")
        .model("gpt-4o-mini")
        .temperature(0.0f)
        .max_tokens(32)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "OpenAI completion failed: " << result.error().message;

    EXPECT_GT(result->prompt_tokens, 0u)
        << "Expected prompt_tokens > 0";
    EXPECT_GT(result->completion_tokens, 0u)
        << "Expected completion_tokens > 0";
    EXPECT_GT(result->total_tokens(), result->prompt_tokens)
        << "total_tokens should exceed prompt_tokens";
}

TEST_F(SmokeRealLLMTest, OpenAI_ToolCalling) {
    auto api_key = get_env("OPENAI_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "OPENAI_API_KEY not set";
    }

    OpenAIBackend backend(api_key, "https://api.openai.com/v1", "gpt-4o-mini");

    auto req = LLMRequest::builder()
        .system("You are a weather assistant. Use the get_weather tool to answer questions.")
        .user("What is the weather in San Francisco?")
        .model("gpt-4o-mini")
        .temperature(0.0f)
        .max_tokens(256)
        .tools_json(kToolsJson)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "OpenAI tool-calling request failed: " << result.error().message;

    EXPECT_TRUE(result->wants_tool_call())
        << "Expected model to invoke the get_weather tool";
    ASSERT_FALSE(result->tool_calls.empty());
    EXPECT_EQ(result->tool_calls[0].name, "get_weather");
    EXPECT_FALSE(result->tool_calls[0].id.empty());
    EXPECT_FALSE(result->tool_calls[0].args_json.empty());
}

TEST_F(SmokeRealLLMTest, OpenAI_RetryViaKernel) {
    auto api_key = get_env("OPENAI_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "OPENAI_API_KEY not set";
    }

    // Use LLMKernel to verify retry + rate-limit infrastructure works
    // with a real backend. A successful call means the retry path
    // executed without errors (attempt 0 succeeds, no retries needed).
    auto backend = std::make_unique<OpenAIBackend>(
        api_key, "https://api.openai.com/v1", "gpt-4o-mini");
    LLMKernel kernel(std::move(backend), /*tpm_limit=*/100000, /*max_retries=*/2);

    auto req = LLMRequest::builder()
        .user("Reply with the single word: pong")
        .model("gpt-4o-mini")
        .temperature(0.0f)
        .max_tokens(16)
        .build();

    auto result = kernel.infer(req);
    ASSERT_TRUE(result.has_value())
        << "LLMKernel infer failed: " << result.error().message;

    EXPECT_FALSE(result->content.empty());
    EXPECT_GE(kernel.metrics().total_requests.load(), 1u);
    EXPECT_GT(kernel.metrics().total_tokens.load(), 0u);
    // Retries counter should be 0 if the first attempt succeeds
    // (we don't assert ==0 because a transient blip is always possible)
}

// ─────────────────────────────────────────────────────────────
// Anthropic Smoke Tests
// ─────────────────────────────────────────────────────────────

TEST_F(SmokeRealLLMTest, Anthropic_SimpleCompletion) {
    auto api_key = get_env("ANTHROPIC_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "ANTHROPIC_API_KEY not set";
    }

    AnthropicBackend backend(api_key, "claude-sonnet-4-20250514");

    auto req = LLMRequest::builder()
        .system("You are a helpful assistant. Reply in one short sentence.")
        .user("What is 2 + 2?")
        .temperature(0.0f)
        .max_tokens(64)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "Anthropic completion failed: " << result.error().message;

    EXPECT_FALSE(result->content.empty())
        << "Expected non-empty response content";
}

TEST_F(SmokeRealLLMTest, Anthropic_TokenCounting) {
    auto api_key = get_env("ANTHROPIC_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "ANTHROPIC_API_KEY not set";
    }

    AnthropicBackend backend(api_key, "claude-sonnet-4-20250514");

    auto req = LLMRequest::builder()
        .system("Reply with exactly: Hello.")
        .user("Say hello.")
        .temperature(0.0f)
        .max_tokens(32)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "Anthropic completion failed: " << result.error().message;

    EXPECT_GT(result->prompt_tokens, 0u)
        << "Expected prompt_tokens (input_tokens) > 0";
    EXPECT_GT(result->completion_tokens, 0u)
        << "Expected completion_tokens (output_tokens) > 0";
}

TEST_F(SmokeRealLLMTest, Anthropic_ToolCalling) {
    auto api_key = get_env("ANTHROPIC_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "ANTHROPIC_API_KEY not set";
    }

    AnthropicBackend backend(api_key, "claude-sonnet-4-20250514");

    auto req = LLMRequest::builder()
        .system("You are a weather assistant. Use the get_weather tool to answer questions.")
        .user("What is the weather in San Francisco?")
        .temperature(0.0f)
        .max_tokens(256)
        .tools_json(kToolsJson)
        .build();

    auto result = backend.complete(req);
    ASSERT_TRUE(result.has_value())
        << "Anthropic tool-calling request failed: " << result.error().message;

    EXPECT_TRUE(result->wants_tool_call())
        << "Expected model to invoke the get_weather tool";
    ASSERT_FALSE(result->tool_calls.empty());
    EXPECT_EQ(result->tool_calls[0].name, "get_weather");
    EXPECT_FALSE(result->tool_calls[0].id.empty());
    EXPECT_FALSE(result->tool_calls[0].args_json.empty());
}

TEST_F(SmokeRealLLMTest, Anthropic_RetryViaKernel) {
    auto api_key = get_env("ANTHROPIC_API_KEY");
    if (api_key.empty()) {
        GTEST_SKIP() << "ANTHROPIC_API_KEY not set";
    }

    auto backend = std::make_unique<AnthropicBackend>(
        api_key, "claude-sonnet-4-20250514");
    LLMKernel kernel(std::move(backend), /*tpm_limit=*/100000, /*max_retries=*/2);

    auto req = LLMRequest::builder()
        .user("Reply with the single word: pong")
        .temperature(0.0f)
        .max_tokens(16)
        .build();

    auto result = kernel.infer(req);
    ASSERT_TRUE(result.has_value())
        << "LLMKernel infer (Anthropic) failed: " << result.error().message;

    EXPECT_FALSE(result->content.empty());
    EXPECT_GE(kernel.metrics().total_requests.load(), 1u);
    EXPECT_GT(kernel.metrics().total_tokens.load(), 0u);
}
