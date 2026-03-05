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

  // Test embedding interface (Mock backend doesn't implement embed by default,
  // it returns error)
  EmbeddingRequest ereq;
  ereq.inputs = {"test string"};
  auto emb_res = mock.embed(ereq);
  ASSERT_FALSE(
      emb_res); // Expected to fail with "Embedding not implicitly supported"
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
