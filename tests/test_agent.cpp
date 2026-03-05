#include <agentos/agent.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::kernel;

// ── AgentOS 集成测试 ────────────────────────────────────────

class AgentOSTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto mock = std::make_unique<MockLLMBackend>("test-llm");
    // Register rules
    mock->register_rule("你好", "你好！我是测试助手。");
    mock->register_rule("what is", "The answer is 42.");
    mock->register_tool_rule("搜索", "http_fetch",
                             R"({"url":"https://example.com"})");

    mock_ptr_ = mock.get();

    AgentOS::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.snapshot_dir = (std::filesystem::temp_directory_path() /
                        "agentos_agent_test_snap")
                           .string();
    cfg.ltm_dir = (std::filesystem::temp_directory_path() /
                   "agentos_agent_test_ltm")
                      .string();

    os_ = std::make_unique<AgentOS>(std::move(mock), cfg);
  }

  void TearDown() override {
    os_.reset();
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() / "agentos_agent_test_snap");
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() / "agentos_agent_test_ltm");
  }

  std::unique_ptr<AgentOS> os_;
  MockLLMBackend *mock_ptr_;
};

TEST_F(AgentOSTest, CreateAndDestroyAgent) {
  AgentConfig cfg;
  cfg.name = "TestAgent";
  cfg.role_prompt = "You are a test agent.";

  auto agent = os_->create_agent(cfg);
  ASSERT_NE(agent, nullptr);
  EXPECT_GT(agent->id(), 0);

  os_->destroy_agent(agent->id());
}

TEST_F(AgentOSTest, AgentThinkProducesResponse) {
  AgentConfig cfg;
  cfg.name = "ThinkAgent";
  cfg.role_prompt = "You are helpful.";

  auto agent = os_->create_agent(cfg);
  auto resp = agent->think("你好");
  ASSERT_TRUE(resp);
  EXPECT_EQ(resp->content, "你好！我是测试助手。");
}

TEST_F(AgentOSTest, AgentToolCallDispatched) {
  AgentConfig cfg;
  cfg.name = "ToolAgent";
  cfg.role_prompt = "You can use tools.";

  auto agent = os_->create_agent(cfg);
  auto resp = agent->think("搜索一下");
  ASSERT_TRUE(resp);
  EXPECT_TRUE(resp->wants_tool_call());
  EXPECT_EQ(resp->tool_calls[0].name, "http_fetch");

  // Act on the tool call
  auto tool_result = agent->act(resp->tool_calls[0]);
  ASSERT_TRUE(tool_result);
}

TEST_F(AgentOSTest, ReActAgentRunLoop) {
  AgentConfig cfg;
  cfg.name = "ReActTestAgent";
  cfg.role_prompt = "You answer questions.";

  auto agent = os_->create_agent(cfg);
  auto result = agent->run("你好");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->empty());
}

TEST_F(AgentOSTest, SecurityRoleEnforced) {
  AgentConfig cfg;
  cfg.name = "ReadonlyAgent";
  cfg.security_role = "readonly";

  auto agent = os_->create_agent(cfg);
  EXPECT_TRUE(os_->security()->may(agent->id(), security::Permission::ToolReadOnly));
  EXPECT_FALSE(os_->security()->may(agent->id(), security::Permission::ToolWrite));
}

TEST_F(AgentOSTest, AgentBusCommunication) {
  AgentConfig cfg_a;
  cfg_a.name = "AgentA";
  auto agent_a = os_->create_agent(cfg_a);

  AgentConfig cfg_b;
  cfg_b.name = "AgentB";
  auto agent_b = os_->create_agent(cfg_b);

  // A sends to B
  agent_a->send(agent_b->id(), "hello", "hi from A");

  auto msg = agent_b->recv(Duration{2000});
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->payload, "hi from A");
}

TEST_F(AgentOSTest, SystemStatus) {
  auto status = os_->status();
  EXPECT_TRUE(status.find("AgentOS") != std::string::npos);
  EXPECT_TRUE(status.find("agents=0") != std::string::npos);
}

TEST_F(AgentOSTest, SubmitAsyncTask) {
  std::atomic<int> counter{0};
  auto tid = os_->submit_task("test_task", [&] { counter++; });
  EXPECT_GT(tid, 0);

  os_->scheduler().wait_for(tid, Duration{5000});
  EXPECT_EQ(counter.load(), 1);
}
