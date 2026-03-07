// ============================================================
// AgentOS Integration Tests
// End-to-end agent execution + concurrency stress tests
// ============================================================
#include <agentos/agent.hpp>
#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <vector>

using namespace agentos;

// ── End-to-End Agent Flow ──────────────────────────────────

class IntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto backend = std::make_unique<kernel::MockLLMBackend>();
    backend->register_rule("hello", "Hello! How can I help you?");
    backend->register_tool_rule("search", "kv_store",
                                R"({"op":"set","key":"result","value":"found"})");

    AgentOS::Config cfg;
    cfg.snapshot_dir = "/tmp/agentos_test_integration_snap";
    cfg.ltm_dir = "/tmp/agentos_test_integration_ltm";
    cfg.enable_security = true;

    os_ = std::make_unique<AgentOS>(std::move(backend), cfg);
  }

  void TearDown() override {
    os_.reset();
    std::filesystem::remove_all("/tmp/agentos_test_integration_snap");
    std::filesystem::remove_all("/tmp/agentos_test_integration_ltm");
  }

  std::unique_ptr<AgentOS> os_;
};

TEST_F(IntegrationTest, AgentCreationAndExecution) {
  AgentConfig cfg;
  cfg.name = "test_agent";
  cfg.role_prompt = "You are a helpful assistant.";
  cfg.security_role = "standard";

  auto agent = os_->create_agent(cfg);
  ASSERT_NE(agent, nullptr);
  EXPECT_EQ(agent->config().name, "test_agent");

  // Agent should be able to think
  auto resp = agent->think("hello");
  ASSERT_TRUE(resp.has_value());
  EXPECT_FALSE(resp->content.empty());
}

TEST_F(IntegrationTest, MultiAgentCommunication) {
  AgentConfig cfg1;
  cfg1.name = "agent_a";
  cfg1.security_role = "standard";

  AgentConfig cfg2;
  cfg2.name = "agent_b";
  cfg2.security_role = "standard";

  auto a = os_->create_agent(cfg1);
  auto b = os_->create_agent(cfg2);

  // A sends to B
  a->send(b->id(), "greeting", "hello from A");

  // B receives
  auto msg = b->recv(Duration{1000});
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->payload, "hello from A");
  EXPECT_EQ(msg->topic, "greeting");
}

TEST_F(IntegrationTest, TaskSubmissionAndDependencies) {
  std::atomic<int> counter{0};

  auto t1 = os_->submit_task("task1", [&] { counter += 1; });
  auto t2 = os_->submit_task("task2", [&] { counter += 10; }, 0, Priority::Normal, {t1});

  EXPECT_TRUE(os_->scheduler().wait_for(t2, Duration{5000}));
  EXPECT_EQ(counter.load(), 11);
}

TEST_F(IntegrationTest, SecurityBlocksDangerousTool) {
  AgentConfig cfg;
  cfg.name = "restricted_agent";
  cfg.security_role = "readonly"; // Only read permissions

  auto agent = os_->create_agent(cfg);

  kernel::ToolCallRequest call;
  call.name = "shell_exec";
  call.args_json = R"({"cmd":"echo test"})";

  auto result = agent->act(call);
  // Should be blocked by RBAC
  EXPECT_FALSE(result.has_value());
}

TEST_F(IntegrationTest, AgentMemoryPersistence) {
  AgentConfig cfg;
  cfg.name = "memory_agent";
  cfg.security_role = "standard";

  auto agent = os_->create_agent(cfg);

  // Store a memory
  auto store_result = agent->remember("Important fact: the sky is blue", 0.9f);
  EXPECT_TRUE(store_result.has_value());

  // Recall should find it
  auto recall_result = agent->recall("sky color", 3);
  ASSERT_TRUE(recall_result.has_value());
  EXPECT_FALSE(recall_result->empty());
}

// ── Concurrency Stress Tests ──────────────────────────────

TEST_F(IntegrationTest, ConcurrentTaskSubmission) {
  std::atomic<int> completed{0};
  constexpr int N = 50;

  std::vector<TaskId> ids;
  for (int i = 0; i < N; ++i) {
    auto tid = os_->submit_task("concurrent_" + std::to_string(i),
                                [&] { completed++; });
    ids.push_back(tid);
  }

  // Wait for all submitted tasks
  for (auto tid : ids) {
    os_->scheduler().wait_for(tid, Duration{10000});
  }

  // All N tasks should have completed (may be more if previous test leaked)
  EXPECT_GE(completed.load(), N);
}

TEST_F(IntegrationTest, ConcurrentAgentCreation) {
  constexpr int N = 20;
  std::vector<std::future<std::shared_ptr<ReActAgent>>> futures;

  for (int i = 0; i < N; ++i) {
    futures.push_back(std::async(std::launch::async, [this, i] {
      AgentConfig cfg;
      cfg.name = "agent_" + std::to_string(i);
      cfg.security_role = "standard";
      return os_->create_agent(cfg);
    }));
  }

  std::vector<std::shared_ptr<ReActAgent>> agents;
  for (auto &f : futures) {
    auto agent = f.get();
    EXPECT_NE(agent, nullptr);
    agents.push_back(agent);
  }

  // All agents should have unique IDs
  std::set<AgentId> ids;
  for (auto &a : agents) ids.insert(a->id());
  EXPECT_EQ(ids.size(), static_cast<size_t>(N));
}

TEST_F(IntegrationTest, ConcurrentBusMessages) {
  constexpr int N = 10;
  constexpr int MSGS_PER_AGENT = 20;

  // Create N agents
  std::vector<std::shared_ptr<ReActAgent>> agents;
  for (int i = 0; i < N; ++i) {
    AgentConfig cfg;
    cfg.name = "bus_agent_" + std::to_string(i);
    cfg.security_role = "standard";
    agents.push_back(os_->create_agent(cfg));
  }

  // Each agent sends MSGS_PER_AGENT messages to agent 0
  std::vector<std::thread> threads;
  for (int i = 1; i < N; ++i) {
    threads.emplace_back([&, i] {
      for (int j = 0; j < MSGS_PER_AGENT; ++j) {
        agents[i]->send(agents[0]->id(), "data",
                        "msg_" + std::to_string(i) + "_" + std::to_string(j));
      }
    });
  }

  for (auto &t : threads) t.join();

  // Agent 0 should receive all messages
  int received = 0;
  while (auto msg = agents[0]->recv(Duration{500})) {
    received++;
  }
  EXPECT_EQ(received, (N - 1) * MSGS_PER_AGENT);
}
