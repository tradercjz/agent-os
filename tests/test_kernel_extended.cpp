#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>
#include <thread>

using namespace agentos;
using namespace agentos::kernel;

// ─────────────────────────────────────────────────────────────
// 1. LLMRequest builder full chain test
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, BuilderFullChain) {
  auto req = LLMRequest::builder()
                 .system("You are a helpful assistant")
                 .user("Hello, what is 2+2?")
                 .assistant("2+2 equals 4.")
                 .user("And 3+3?")
                 .model("gpt-4o")
                 .temperature(0.3f)
                 .max_tokens(512)
                 .priority(Priority::High)
                 .agent_id(42)
                 .task_id(99)
                 .request_id("test-req-001")
                 .tools({"calculator", "web_search"})
                 .tools_json(R"([{"type":"function","function":{"name":"calc"}}])")
                 .build();

  ASSERT_EQ(req.messages.size(), 4u);
  EXPECT_EQ(req.messages[0].role, Role::System);
  EXPECT_EQ(req.messages[0].content, "You are a helpful assistant");
  EXPECT_EQ(req.messages[1].role, Role::User);
  EXPECT_EQ(req.messages[2].role, Role::Assistant);
  EXPECT_EQ(req.messages[3].role, Role::User);
  EXPECT_EQ(req.messages[3].content, "And 3+3?");

  EXPECT_EQ(req.model, "gpt-4o");
  EXPECT_FLOAT_EQ(req.temperature, 0.3f);
  EXPECT_EQ(req.max_tokens, 512u);
  EXPECT_EQ(req.priority, Priority::High);
  EXPECT_EQ(req.agent_id, 42u);
  EXPECT_EQ(req.task_id, 99u);
  EXPECT_EQ(req.request_id, "test-req-001");
  ASSERT_TRUE(req.tool_names.has_value());
  EXPECT_EQ(req.tool_names->size(), 2u);
  EXPECT_EQ((*req.tool_names)[0], "calculator");
  ASSERT_TRUE(req.tools_json.has_value());
  EXPECT_FALSE(req.tools_json->empty());
}

// ─────────────────────────────────────────────────────────────
// 2. LLMResponse token counting
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, ResponseTokenCounting) {
  LLMResponse resp;
  resp.prompt_tokens = 150;
  resp.completion_tokens = 50;
  EXPECT_EQ(resp.total_tokens(), 200u);

  // Zero tokens
  LLMResponse empty_resp;
  EXPECT_EQ(empty_resp.total_tokens(), 0u);

  // Large values
  LLMResponse large_resp;
  large_resp.prompt_tokens = 100000;
  large_resp.completion_tokens = 50000;
  EXPECT_EQ(large_resp.total_tokens(), 150000u);
}

// ─────────────────────────────────────────────────────────────
// 3. LLMResponse wants_tool_call with empty/non-empty tool_calls
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, ResponseWantsToolCallEmpty) {
  LLMResponse resp;
  EXPECT_FALSE(resp.wants_tool_call());
}

TEST(KernelExtended, ResponseWantsToolCallNonEmpty) {
  LLMResponse resp;
  resp.tool_calls.push_back(ToolCallRequest{
      .id = "call_1",
      .name = "web_search",
      .args_json = R"({"query":"test"})",
  });
  EXPECT_TRUE(resp.wants_tool_call());
}

TEST(KernelExtended, ResponseWantsToolCallMultiple) {
  LLMResponse resp;
  resp.tool_calls.push_back(ToolCallRequest{"call_1", "search", "{}"});
  resp.tool_calls.push_back(ToolCallRequest{"call_2", "calc", R"({"x":1})"});
  EXPECT_TRUE(resp.wants_tool_call());
  EXPECT_EQ(resp.tool_calls.size(), 2u);
}

// ─────────────────────────────────────────────────────────────
// 4. EmbeddingRequest/EmbeddingResponse construction
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, EmbeddingRequestConstruction) {
  EmbeddingRequest req;
  req.inputs = {"hello", "world", "test"};
  req.model = "text-embedding-3-small";
  EXPECT_EQ(req.inputs.size(), 3u);
  EXPECT_EQ(req.model, "text-embedding-3-small");
}

TEST(KernelExtended, EmbeddingResponseConstruction) {
  EmbeddingResponse resp;
  resp.embeddings.push_back({0.1f, 0.2f, 0.3f});
  resp.embeddings.push_back({0.4f, 0.5f, 0.6f});
  resp.total_tokens = 10;

  EXPECT_EQ(resp.embeddings.size(), 2u);
  EXPECT_EQ(resp.embeddings[0].size(), 3u);
  EXPECT_FLOAT_EQ(resp.embeddings[0][0], 0.1f);
  EXPECT_EQ(resp.total_tokens, 10u);
}

TEST(KernelExtended, EmbeddingResponseDefaultTokens) {
  EmbeddingResponse resp;
  EXPECT_EQ(resp.total_tokens, 0u);
  EXPECT_TRUE(resp.embeddings.empty());
}

// ─────────────────────────────────────────────────────────────
// 5. TokenBucketRateLimiter: consume within limit
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, RateLimiterConsumeWithinLimit) {
  TokenBucketRateLimiter limiter(10000); // 10k TPM
  auto result = limiter.try_consume(100);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.wait_ms.count(), 0);

  // After consuming, available should decrease
  EXPECT_LE(limiter.available_tokens(), 10000u);
}

TEST(KernelExtended, RateLimiterConsumeExactlyAtLimit) {
  TokenBucketRateLimiter limiter(1000);
  auto result = limiter.try_consume(1000);
  EXPECT_TRUE(result.ok);

  // Bucket should be empty now
  EXPECT_EQ(limiter.available_tokens(), 0u);
}

// ─────────────────────────────────────────────────────────────
// 6. TokenBucketRateLimiter: consume exceeding limit returns wait time
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, RateLimiterConsumeExceedingLimit) {
  TokenBucketRateLimiter limiter(1000);
  // Drain the bucket
  auto r1 = limiter.try_consume(1000);
  EXPECT_TRUE(r1.ok);

  // Now try to consume more -- should fail with wait time
  auto r2 = limiter.try_consume(500);
  EXPECT_FALSE(r2.ok);
  EXPECT_GT(r2.wait_ms.count(), 0);
}

TEST(KernelExtended, RateLimiterConsumeWayOverLimit) {
  TokenBucketRateLimiter limiter(100);
  // Request more than total capacity
  auto result = limiter.try_consume(200);
  EXPECT_FALSE(result.ok);
  EXPECT_GT(result.wait_ms.count(), 0);
}

// ─────────────────────────────────────────────────────────────
// 7. TokenBucketRateLimiter: refill over time
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, RateLimiterRefillOverTime) {
  // Use a high TPM so refill is noticeable even in a short sleep
  TokenBucketRateLimiter limiter(600000); // 10000 tokens/sec

  // Drain most of the bucket
  auto r1 = limiter.try_consume(600000);
  EXPECT_TRUE(r1.ok);

  // Wait a bit for refill
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // try_consume triggers refill; after 0.1s at 10000 tok/sec, ~1000 tokens refilled
  auto r2 = limiter.try_consume(1);
  EXPECT_TRUE(r2.ok) << "Expected some tokens refilled after 100ms sleep";
}

TEST(KernelExtended, RateLimiterSetRate) {
  TokenBucketRateLimiter limiter(10000);
  EXPECT_EQ(limiter.available_tokens(), 10000u);

  // Reduce rate -- bucket should be capped
  limiter.set_rate(5000);
  EXPECT_LE(limiter.available_tokens(), 5000u);
}

// ─────────────────────────────────────────────────────────────
// 8. MockBackend: multiple sequential infer calls
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, MockBackendSequentialCalls) {
  MockLLMBackend mock("test-model");

  std::vector<std::string> prompts = {
      "First question",
      "Second question",
      "Third question",
  };

  for (const auto &prompt : prompts) {
    LLMRequest req;
    req.messages = {Message::user(prompt)};
    auto result = mock.complete(req);
    ASSERT_TRUE(result) << "Failed for prompt: " << prompt;
    EXPECT_FALSE(result->content.empty());
    EXPECT_EQ(result->finish_reason, "stop");
    EXPECT_GT(result->prompt_tokens, 0u);
    EXPECT_GT(result->completion_tokens, 0u);
  }
}

TEST(KernelExtended, MockBackendNameReturnsModelName) {
  MockLLMBackend mock1("gpt-4");
  EXPECT_EQ(mock1.name(), "gpt-4");

  MockLLMBackend mock2; // default name
  EXPECT_EQ(mock2.name(), "mock-gpt");
}

// ─────────────────────────────────────────────────────────────
// 9. MockBackend: custom response configuration
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, MockBackendCustomRule) {
  MockLLMBackend mock;
  mock.register_rule("weather", "It is sunny today.", 0, false);

  LLMRequest req;
  req.messages = {Message::user("What is the weather?")};
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->content, "It is sunny today.");
  EXPECT_EQ(result->finish_reason, "stop");
}

TEST(KernelExtended, MockBackendToolRule) {
  MockLLMBackend mock;
  mock.register_tool_rule("search for", "web_search", R"({"query":"test"})", 0, false);

  LLMRequest req;
  req.messages = {Message::user("Please search for cats")};
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->wants_tool_call());
  ASSERT_EQ(result->tool_calls.size(), 1u);
  EXPECT_EQ(result->tool_calls[0].name, "web_search");
  EXPECT_EQ(result->tool_calls[0].args_json, R"({"query":"test"})");
  EXPECT_EQ(result->finish_reason, "tool_calls");
}

TEST(KernelExtended, MockBackendRulePriority) {
  MockLLMBackend mock;
  mock.register_rule("hello", "Low priority response", 0, false);
  mock.register_rule("hello", "High priority response", 10, false);

  LLMRequest req;
  req.messages = {Message::user("hello world")};
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  // Higher priority rule should match first
  EXPECT_EQ(result->content, "High priority response");
}

TEST(KernelExtended, MockBackendRegexRule) {
  MockLLMBackend mock;
  mock.register_rule(R"(\d+\s*\+\s*\d+)", "Math detected!", 0, true);

  LLMRequest req;
  req.messages = {Message::user("What is 42 + 58?")};
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->content, "Math detected!");
}

TEST(KernelExtended, MockBackendDefaultResponseNoRules) {
  MockLLMBackend mock("my-model");

  LLMRequest req;
  req.messages = {Message::user("Random input that matches no rule")};
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  // Default response format: [MockLLM:model_name] ...
  EXPECT_NE(result->content.find("MockLLM"), std::string::npos);
  EXPECT_NE(result->content.find("my-model"), std::string::npos);
}

TEST(KernelExtended, MockBackendEmptyMessages) {
  MockLLMBackend mock;
  LLMRequest req;
  // No messages
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->finish_reason, "stop");
}

// ─────────────────────────────────────────────────────────────
// 10. Message factory methods preserve content
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, MessageFactoryPreservesContent) {
  std::string sys_text = "You are a helpful AI assistant.";
  auto sys = Message::system(sys_text);
  EXPECT_EQ(sys.role, Role::System);
  EXPECT_EQ(sys.content, sys_text);
  EXPECT_TRUE(sys.name.empty());
  EXPECT_TRUE(sys.tool_call_id.empty());
  EXPECT_TRUE(sys.tool_calls.empty());
  EXPECT_TRUE(sys.attachments.empty());

  std::string user_text = "What is the meaning of life?";
  auto usr = Message::user(user_text);
  EXPECT_EQ(usr.role, Role::User);
  EXPECT_EQ(usr.content, user_text);
  EXPECT_TRUE(usr.attachments.empty());

  std::string asst_text = "The meaning of life is 42.";
  auto asst = Message::assistant(asst_text);
  EXPECT_EQ(asst.role, Role::Assistant);
  EXPECT_EQ(asst.content, asst_text);
  EXPECT_TRUE(asst.attachments.empty());
}

TEST(KernelExtended, MessageFactoryWithImagePreservesContent) {
  auto msg = Message::user_with_image("Look at this", "https://example.com/img.png");
  EXPECT_EQ(msg.role, Role::User);
  EXPECT_EQ(msg.content, "Look at this");
  ASSERT_EQ(msg.attachments.size(), 1u);
  EXPECT_EQ(msg.attachments[0].url, "https://example.com/img.png");
  EXPECT_EQ(msg.attachments[0].mime_type, "image/png");
  EXPECT_TRUE(msg.name.empty());
  EXPECT_TRUE(msg.tool_call_id.empty());
}

TEST(KernelExtended, MessageFactoryWithAudioPreservesContent) {
  auto msg = Message::user_with_audio("Listen to this", "base64audiodata", "audio/mp3");
  EXPECT_EQ(msg.role, Role::User);
  EXPECT_EQ(msg.content, "Listen to this");
  ASSERT_EQ(msg.attachments.size(), 1u);
  EXPECT_EQ(msg.attachments[0].data, "base64audiodata");
  EXPECT_EQ(msg.attachments[0].mime_type, "audio/mp3");
  EXPECT_TRUE(msg.attachments[0].is_base64);
}

// ─────────────────────────────────────────────────────────────
// Extra: LLMKernel with MockBackend integration
// ─────────────────────────────────────────────────────────────

TEST(KernelExtended, KernelInferWithMockBackend) {
  auto mock = std::make_unique<MockLLMBackend>("test");
  mock->register_rule("ping", "pong");

  LLMKernel kernel(std::move(mock), 100000, 1);

  LLMRequest req;
  req.messages = {Message::user("ping")};
  req.max_tokens = 100;

  auto result = kernel.infer(req);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->content, "pong");
  EXPECT_EQ(kernel.metrics().total_requests.load(), 1u);
  EXPECT_GT(kernel.metrics().total_tokens.load(), 0u);
}

TEST(KernelExtended, KernelModelName) {
  auto mock = std::make_unique<MockLLMBackend>("custom-model-v2");
  LLMKernel kernel(std::move(mock));
  EXPECT_EQ(kernel.model_name(), "custom-model-v2");
}

TEST(KernelExtended, EstimateTokensHeuristic) {
  // ~4 chars per token + 1
  EXPECT_EQ(ILLMBackend::estimate_tokens(""), 1u);
  EXPECT_EQ(ILLMBackend::estimate_tokens("abcd"), 2u);    // 4/4+1
  EXPECT_EQ(ILLMBackend::estimate_tokens("abcdefgh"), 3u); // 8/4+1

  // Longer text
  std::string text(400, 'x');
  EXPECT_EQ(ILLMBackend::estimate_tokens(text), 101u); // 400/4+1
}

TEST(KernelExtended, MessageTokensCachesResult) {
  auto msg = Message::user("This is a test message for token counting");
  EXPECT_EQ(msg.cached_tokens, 0u);

  TokenCount t1 = msg.tokens();
  EXPECT_GT(t1, 0u);
  EXPECT_EQ(msg.cached_tokens, t1); // Cached after first call

  TokenCount t2 = msg.tokens();
  EXPECT_EQ(t1, t2); // Same value from cache
}
