#include <agentos/agent.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::kernel;

TEST(FluentAPITest, LLMRequestBuilderTest) {
  auto req = LLMRequest::builder()
    .model("gpt-4o")
    .temperature(0.5f)
    .max_tokens(1024)
    .system("You are a helpful assistant.")
    .user("What is the capital of France?")
    .build();

  EXPECT_EQ(req.model, "gpt-4o");
  EXPECT_FLOAT_EQ(req.temperature, 0.5f);
  EXPECT_EQ(req.max_tokens, 1024u);
  ASSERT_EQ(req.messages.size(), 2u);
  EXPECT_EQ(req.messages[0].role, Role::System);
  EXPECT_EQ(req.messages[0].content, "You are a helpful assistant.");
  EXPECT_EQ(req.messages[1].role, Role::User);
  EXPECT_EQ(req.messages[1].content, "What is the capital of France?");
}

TEST(FluentAPITest, AgentConfigBuilderTest) {
  auto cfg = AgentOS::Config::builder()
    .scheduler_threads(8)
    .tpm_limit(50000)
    .snapshot_dir("/tmp/snapshots")
    .enable_security(false)
    .build();

  EXPECT_EQ(cfg.scheduler_threads, 8u);
  EXPECT_EQ(cfg.tpm_limit, 50000u);
  EXPECT_EQ(cfg.snapshot_dir, "/tmp/snapshots");
  EXPECT_FALSE(cfg.enable_security);
}

TEST(FluentAPITest, IndividualAgentConfigBuilderTest) {
  auto cfg = AgentConfig::builder()
    .name("researcher")
    .role_prompt("Expert analyst")
    .context_limit(16384)
    .persist_memory(true)
    .build();

  EXPECT_EQ(cfg.name, "researcher");
  EXPECT_EQ(cfg.role_prompt, "Expert analyst");
  EXPECT_EQ(cfg.context_limit, 16384u);
  EXPECT_TRUE(cfg.persist_memory);
}
