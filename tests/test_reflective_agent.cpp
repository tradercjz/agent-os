#include <agentos/reflective_agent.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>

using namespace agentos;
using namespace agentos::kernel;

namespace {

std::filesystem::path make_reflective_agent_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("agentos_ref_test_" + name + "_" + std::to_string(nonce));
}

}

// ── ReflectiveAgentOS 集成测试 ────────────────────────────────────────

class ReflectiveAgentOSTest : public ::testing::Test {
protected:
  std::filesystem::path snapshot_dir_ = make_reflective_agent_test_dir("snap");
  std::filesystem::path ltm_dir_ = make_reflective_agent_test_dir("ltm");

  void SetUp() override {
    auto mock = std::make_unique<MockLLMBackend>("test-llm");
    // Register rules for typical thought process
    mock->register_rule("你好", "你好！我是反思助手。");

    // Tools rule for first step thought
    mock->register_tool_rule("搜索", "http_fetch",
                             R"({"url":"https://example.com"})");

    // Reflection rule
    // MockLLMBackend will be asked to reflect on: 计划执行工具调用: http_fetch({"url":"https://example.com"});
    // We can simulate an "OK" reflection or a corrective reflection.
    // If it says "OK", reflection passes.
    // We register a rule for reflection text:
    mock->register_rule("如果计划很完善，请只回复 \"OK\"", "OK");

    // For timeout testing, we create an infinite tool call loop
    mock->register_tool_rule("无限搜索", "infinite_fetch", R"({})");

    mock_ptr_ = mock.get();

    AgentOS::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.snapshot_dir = snapshot_dir_.string();
    cfg.ltm_dir = ltm_dir_.string();

    os_ = std::make_unique<AgentOS>(std::move(mock), cfg);

    // Register the tools in the OS tools manager so that act() works without failure
    os_->register_tool(
        tools::ToolSchema{"http_fetch", "Fetches URL",
                          {{"url", tools::ParamType::String, "The URL", true, std::nullopt}}},
        [](const tools::ParsedArgs &) -> tools::ToolResult { return tools::ToolResult::ok("Fetched content"); });

    os_->register_tool(
        tools::ToolSchema{"infinite_fetch", "Fetches infinitely",
                          {}},
        [](const tools::ParsedArgs &) -> tools::ToolResult { return tools::ToolResult::ok("Infinite fetch content"); });
  }

  void TearDown() override {
    os_.reset();
    std::filesystem::remove_all(snapshot_dir_);
    std::filesystem::remove_all(ltm_dir_);
  }

  std::unique_ptr<AgentOS> os_;
  MockLLMBackend *mock_ptr_;
};

TEST_F(ReflectiveAgentOSTest, CreateAndDestroyAgent) {
  AgentConfig cfg;
  cfg.name = "TestReflectiveAgent";
  cfg.role_prompt = "You are a reflective test agent.";

  auto agent = os_->create_agent<ReflectiveReActAgent>(cfg);
  ASSERT_NE(agent, nullptr);
  EXPECT_GT(agent->id(), 0u);

  os_->destroy_agent(agent->id());
}

TEST_F(ReflectiveAgentOSTest, AgentThinkProducesResponse) {
  AgentConfig cfg;
  cfg.name = "ThinkReflectiveAgent";
  cfg.role_prompt = "You are helpful.";

  auto agent = os_->create_agent<ReflectiveReActAgent>(cfg);
  auto resp = agent->run("你好");
  ASSERT_TRUE(resp);
  EXPECT_EQ(*resp, "你好！我是反思助手。");
}

TEST_F(ReflectiveAgentOSTest, ReflectiveToolCallWithOK) {
  AgentConfig cfg;
  cfg.name = "ReflectiveToolAgent";
  cfg.role_prompt = "You can use tools.";

  auto agent = os_->create_agent<ReflectiveReActAgent>(cfg);

  // Here we use mock capabilities to simulate tool usage:
  // "搜索" triggers a tool call response in MockLLMBackend.
  // The backend will generate a tool call for http_fetch.
  // Then the agent reflects: the prompt contains "如果计划很完善，请只回复 "OK"".
  // Our MockLLMBackend matches this and returns "OK".
  // The agent then acts on the tool call.
  // Note: the mock backend tool_rule returns a tool call, and on the NEXT step (the next `think`),
  // if no explicit rule matches, the MockLLMBackend will just return a default text (e.g. "Mock response length...").

  // To make the run loop finish, we need the NEXT thought to not contain a tool call.
  // MockLLMBackend by default doesn't generate tool calls unless a tool_rule matches.
  // So step 1: "搜索" -> tool call.
  // step 2: "[继续]" -> no tool call, returns mock default text.

  auto resp = agent->run("搜索");
  ASSERT_TRUE(resp);
  // It should successfully complete and return the mock default text.
  EXPECT_FALSE(resp->empty());
}

TEST_F(ReflectiveAgentOSTest, TimeoutLimit) {
  AgentConfig cfg;
  cfg.name = "TimeoutReflectiveAgent";
  cfg.role_prompt = "You are stuck in a loop.";

  auto agent = os_->create_agent<ReflectiveReActAgent>(cfg);

  // Here we test the MAX_STEPS timeout.
  // We need the mock to ALWAYS return a tool call when "[继续]" is asked, or we can just ask "无限搜索"
  // Actually, MockLLMBackend returns what matches.
  // "无限搜索" will match the tool rule. The NEXT thought is "[继续]".
  // If we map "[继续]" to ALSO return a tool call, it will loop forever.
  mock_ptr_->register_tool_rule("[继续]", "infinite_fetch", R"({})");

  auto resp = agent->run("无限搜索");

  // Should fail with Timeout error
  ASSERT_FALSE(resp);
  EXPECT_EQ(resp.error().code, ErrorCode::Timeout);
}

// Optionally, test when reflection fails (requires correction).
TEST_F(ReflectiveAgentOSTest, ReflectiveCorrection) {
  AgentConfig cfg;
  cfg.name = "CorrectionReflectiveAgent";
  cfg.role_prompt = "You can use tools.";

  auto agent = os_->create_agent<ReflectiveReActAgent>(cfg);

  // We change the reflection rule specifically for this test to NOT return "OK".
  // The mock's priority needs to be higher so it overrides the "OK" rule set in SetUp().
  mock_ptr_->register_rule("如果计划很完善，请只回复 \"OK\"", "参数缺失，需要补充", 100);

  // The agent will get the tool call for "搜索", reflect on it, get "参数缺失，需要补充",
  // and append a system message, then continue to the next iteration without ACTING.
  // On the next iteration, step=1, input="[继续]".
  // We map "[继续]" to NOT be a tool call so it finishes.
  mock_ptr_->register_rule("[继续]", "我已经修正了参数。", 100);

  auto resp = agent->run("搜索");
  ASSERT_TRUE(resp);
  EXPECT_EQ(*resp, "我已经修正了参数。");

  // Check that the reflection actually occurred by inspecting the context.
  const auto& history = os_->ctx().get_window(agent->id()).messages();
  bool reflection_found = false;
  for (const auto& msg : history) {
      if (msg.role == kernel::Role::System && msg.content.find("【自我反思驱动的修正指令】") != std::string::npos) {
          reflection_found = true;
          break;
      }
  }
  EXPECT_TRUE(reflection_found);
}
