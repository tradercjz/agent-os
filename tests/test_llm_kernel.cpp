#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::kernel;

class LLMKernelTest : public ::testing::Test {
protected:
  void SetUp() override {
    // ...
  }
};

TEST_F(LLMKernelTest, MockBackendTokenLimits) {
  MockLLMBackend mock;

  LLMRequest req;
  req.messages = {
      Message::system("You are an assistant."),
      Message::user(
          "Please echo this back to me, but don't exceed the token limit.")};
  req.max_tokens = 5; // Very small limit

  auto res = mock.complete(req);
  ASSERT_TRUE(res);

  // Mock Backend generated text length will be bounded roughly by token limit
  // simulation Currently, MockLLMBackend generates standard lengths, but let's
  // test the interface correctness and ensure no crashes happen.
  EXPECT_FALSE(res->content.empty());

  // Test embedding interface — MockBackend now generates deterministic vectors
  EmbeddingRequest ereq;
  ereq.inputs = {"test string"};
  auto emb_res = mock.embed(ereq);
  ASSERT_TRUE(emb_res);
  ASSERT_EQ(emb_res->embeddings.size(), 1u);
  EXPECT_EQ(emb_res->embeddings[0].size(), 1536u); // default dim
}

// ── MockBackend embed() 测试 ──────────────────────────────

TEST_F(LLMKernelTest, MockEmbedDeterministic) {
  MockLLMBackend mock;
  EmbeddingRequest req;
  req.inputs = {"hello world"};

  auto r1 = mock.embed(req);
  auto r2 = mock.embed(req);
  ASSERT_TRUE(r1);
  ASSERT_TRUE(r2);
  // 同一输入生成的向量应完全相同
  EXPECT_EQ(r1->embeddings[0], r2->embeddings[0]);
}

TEST_F(LLMKernelTest, MockEmbedNormalized) {
  MockLLMBackend mock;
  mock.set_embed_dim(128); // 小维度，加速测试
  EmbeddingRequest req;
  req.inputs = {"test normalization"};

  auto res = mock.embed(req);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->embeddings[0].size(), 128u);

  // 验证 L2 归一化：||v|| ≈ 1.0
  float norm_sq = 0.0f;
  for (float v : res->embeddings[0])
    norm_sq += v * v;
  EXPECT_NEAR(norm_sq, 1.0f, 1e-4f);
}

TEST_F(LLMKernelTest, MockEmbedMultipleInputs) {
  MockLLMBackend mock;
  mock.set_embed_dim(64);
  EmbeddingRequest req;
  req.inputs = {"alpha", "beta", "gamma"};

  auto res = mock.embed(req);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->embeddings.size(), 3u);

  // 不同输入产生不同向量
  EXPECT_NE(res->embeddings[0], res->embeddings[1]);
  EXPECT_NE(res->embeddings[1], res->embeddings[2]);
}

TEST_F(LLMKernelTest, MockEmbedTokenAccounting) {
  MockLLMBackend mock;
  EmbeddingRequest req;
  req.inputs = {"short", "a longer test sentence for token estimation"};

  auto res = mock.embed(req);
  ASSERT_TRUE(res);
  EXPECT_GT(res->total_tokens, 0u);
}

TEST_F(LLMKernelTest, MessageConstruction) {
  auto sys_msg = Message::system("System prompt");
  EXPECT_EQ(sys_msg.role, Role::System);
  EXPECT_EQ(sys_msg.content, "System prompt");

  auto user_msg = Message::user("User message");
  EXPECT_EQ(user_msg.role, Role::User);

  auto asst_msg = Message::assistant("Assistant reply");
  EXPECT_EQ(asst_msg.role, Role::Assistant);

  auto tool_req = ToolCallRequest{"call_abc", "web_search", "{}"};
  Message tool_msg;
  tool_msg.role = Role::Tool;
  tool_msg.content = "Tool result data";
  tool_msg.tool_call_id = tool_req.id;
  EXPECT_EQ(tool_msg.role, Role::Tool);
  EXPECT_EQ(tool_msg.tool_call_id, "call_abc");
}
