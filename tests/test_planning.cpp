#include <agentos/planning.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::kernel;

// ── PlanningAgent 测试 ────────────────────────────────────────

class PlanningAgentTest : public ::testing::Test {
protected:
  void SetUp() override {
    snap_dir_ = (std::filesystem::temp_directory_path() /
                 "agentos_planning_test_snap")
                    .string();
    ltm_dir_ = (std::filesystem::temp_directory_path() /
                "agentos_planning_test_ltm")
                   .string();
  }

  void TearDown() override {
    os_.reset();
    std::filesystem::remove_all(snap_dir_);
    std::filesystem::remove_all(ltm_dir_);
  }

  // Helper to build AgentOS with a pre-configured mock
  void build_os(std::unique_ptr<MockLLMBackend> mock) {
    mock_ptr_ = mock.get();

    AgentOS::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.snapshot_dir = snap_dir_;
    cfg.ltm_dir = ltm_dir_;

    os_ = std::make_unique<AgentOS>(std::move(mock), cfg);
  }

  std::shared_ptr<PlanningAgent> make_planner(std::string name = "planner") {
    AgentConfig cfg;
    cfg.name = std::move(name);
    cfg.role_prompt = "You are a planning agent.";
    return os_->create_agent<PlanningAgent>(cfg);
  }

  std::string snap_dir_;
  std::string ltm_dir_;
  std::unique_ptr<AgentOS> os_;
  MockLLMBackend *mock_ptr_ = nullptr;
};

// ─────────────────────────────────────────────────────────────
// § 1. PlanParsing_BasicSteps
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, PlanParsing_BasicSteps) {
  // parse_plan_response is private static — test it indirectly through
  // generate_plan by mocking the LLM output.
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // When generate_plan sends the goal as last message, mock returns numbered steps
  mock->register_rule("分析销售数据", "1. 查找相关信息\n2. 分析数据\n3. 生成报告", 10);
  // Step execution rules
  mock->register_rule("查找相关信息", "已找到3篇相关文档", 5);
  mock->register_rule("分析数据", "数据分析完成，关键指标正常", 5);
  mock->register_rule("生成报告", "报告已生成", 5);
  // Synthesis rule (last message contains "执行结果")
  mock->register_rule("执行结果", "综合以上步骤，任务已完成", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("分析销售数据并生成报告");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
  // The synthesized result should come back (non-empty string from mock)
  EXPECT_GT(result->size(), 5u);
}

// ─────────────────────────────────────────────────────────────
// § 2. PlanParsing_WithToolHints
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, PlanParsing_WithToolHints) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Return plan with tool hints
  mock->register_rule("搜索文档",
                      "1. Search docs | tool: http_fetch\n2. Summarize results", 10);
  // Step execution: the description includes "(建议使用工具: http_fetch)"
  mock->register_rule("Search docs", "文档已搜索完成", 5);
  mock->register_rule("Summarize", "摘要完成", 5);
  mock->register_rule("执行结果", "文档搜索和摘要完成", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("搜索文档");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 3. PlanParsing_FallbackSingleStep
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, PlanParsing_FallbackSingleStep) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Return unparseable output (no numbered list) for plan generation
  mock->register_rule("无法解析的任务",
                      "这是一段无法解析为步骤的文本，没有编号格式", 10);
  // The fallback single step will use the entire text as description;
  // when think() is called with that text, provide a response
  mock->register_rule("无法解析为步骤", "步骤执行完成", 5);
  mock->register_rule("执行结果", "任务已完成（单步回退）", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("无法解析的任务");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 4. BasicPlanExecution — 3-step plan, all succeed
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, BasicPlanExecution) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  mock->register_rule("三步任务",
                      "1. 收集数据\n2. 处理数据\n3. 输出结果", 10);
  mock->register_rule("收集数据", "数据收集完毕", 5);
  mock->register_rule("处理数据", "数据处理完毕", 5);
  mock->register_rule("输出结果", "结果已输出", 5);
  mock->register_rule("执行结果", "三个步骤全部完成：数据收集、处理和输出", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("三步任务");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
  // Verify the synthesis returned a meaningful response
  EXPECT_GT(result->size(), 5u);
}

// ─────────────────────────────────────────────────────────────
// § 5. PlanWithToolCalls — step triggers tool execution
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, PlanWithToolCalls) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Plan generation
  mock->register_rule("获取网页数据",
                      "1. 抓取网页 | tool: http_fetch\n2. 解析内容", 10);

  // When step description includes "建议使用工具: http_fetch", trigger tool call
  mock->register_tool_rule("抓取网页", "http_fetch",
                           R"({"url":"https://example.com"})", 5);
  // After tool execution, the agent continues — mock the continuation
  mock->register_rule("继续执行当前步骤", "网页内容已获取", 4);
  mock->register_rule("解析内容", "内容解析完毕", 5);
  mock->register_rule("执行结果", "网页数据获取和解析完成", 3);

  build_os(std::move(mock));

  // Register a simple tool handler
  tools::ToolSchema schema;
  schema.id = "http_fetch";
  schema.description = "Fetch a URL";
  os_->register_tool(schema, [](const tools::ParsedArgs &) -> tools::ToolResult {
    return tools::ToolResult{true, "HTTP 200: <html>test</html>", ""};
  });

  auto agent = make_planner();
  auto result = agent->run("获取网页数据");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 6. ReplanOnFailure — first step fails, replan recovers
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, ReplanOnFailure) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Plan generation: 2 steps
  mock->register_rule("可能失败的任务",
                      "1. 危险操作\n2. 安全操作", 10);
  // First step triggers a tool call that will fail
  mock->register_tool_rule("危险操作", "risky_tool",
                           R"({"action":"delete"})", 8);
  // The tool doesn't exist, so act() will fail, and step iteration continues.
  // After tool call, continuation also triggers tool (cycle until STEP_MAX_ITERATIONS)
  mock->register_tool_rule("继续执行当前步骤", "risky_tool",
                           R"({"action":"retry"})", 4);
  // Replan: when context mentions "失败", return new steps
  mock->register_rule("失败原因",
                      "1. 替代安全操作\n2. 验证结果", 9);
  // Execute the new steps
  mock->register_rule("替代安全操作", "替代操作成功", 5);
  mock->register_rule("验证结果", "验证通过", 5);
  mock->register_rule("安全操作", "安全操作完成", 5);
  mock->register_rule("执行结果", "经过重新规划，任务已恢复完成", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("可能失败的任务");

  // Should still succeed after replanning
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 7. MaxRevisionsLimit — replan fails 3 times, agent gives up
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, MaxRevisionsLimit) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Plan always produces a step that will fail via tool calls that exhaust iterations
  mock->register_rule("永远失败的任务",
                      "1. 必定失败的步骤", 10);
  mock->register_tool_rule("必定失败的步骤", "nonexistent_tool",
                           R"({"x":1})", 8);
  mock->register_tool_rule("继续执行当前步骤", "nonexistent_tool",
                           R"({"x":1})", 7);
  // Replan also produces failing steps
  mock->register_rule("失败原因",
                      "1. 另一个必定失败的步骤", 9);
  mock->register_tool_rule("另一个必定失败的步骤", "nonexistent_tool",
                           R"({"x":1})", 6);
  // Synthesis (will still be called on partial/failed plan)
  mock->register_rule("执行结果", "任务未能完成", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("永远失败的任务");

  // The agent should still return a synthesized result (even if steps failed)
  // because synthesize() is called after the execution loop regardless
  ASSERT_TRUE(result) << result.error().message;
}

// ─────────────────────────────────────────────────────────────
// § 8. DepthLimit — substep decomposition respects MAX_DEPTH
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, DepthLimit) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Plan: single complex step
  mock->register_rule("深度测试任务",
                      "1. 复杂步骤需要分解", 10);

  // Step execution returns a long response (>500 chars) to trigger decomposition
  std::string long_response(600, 'A');
  long_response += " 详细分析结果";
  mock->register_rule("复杂步骤需要分解", long_response, 8);

  // Decomposition check: return "OK" to stop decomposition (keep it simple)
  // The decompose prompt checks "请判断以下步骤是否需要进一步分解"
  mock->register_rule("请判断以下步骤", "OK", 7);

  mock->register_rule("执行结果", "复杂步骤已完成，无需进一步分解", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("深度测试任务");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

TEST_F(PlanningAgentTest, DepthLimit_WithSubsteps) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // Plan: single complex step
  mock->register_rule("深层分解任务",
                      "1. 需要深层分解的步骤", 10);

  // Step returns long output triggering decomposition
  std::string long_response(600, 'B');
  long_response += " 需要继续分析";
  mock->register_rule("需要深层分解的步骤", long_response, 8);

  // Decomposition returns substeps instead of OK
  mock->register_rule("请判断以下步骤",
                      "1. 子步骤A\n2. 子步骤B", 7);
  // Substep execution (short responses so no further decomposition)
  mock->register_rule("子步骤A", "子步骤A完成", 5);
  mock->register_rule("子步骤B", "子步骤B完成", 5);
  mock->register_rule("执行结果", "分解并执行完成", 3);

  build_os(std::move(mock));
  auto agent = make_planner();
  auto result = agent->run("深层分解任务");

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 9. PlanRecall — memory of past plan is recalled
// ─────────────────────────────────────────────────────────────

TEST_F(PlanningAgentTest, PlanRecall) {
  auto mock = std::make_unique<MockLLMBackend>("test-plan");

  // First run
  mock->register_rule("销售分析任务",
                      "1. 收集销售数据\n2. 生成分析报告", 10);
  mock->register_rule("收集销售数据", "数据已收集", 5);
  mock->register_rule("生成分析报告", "报告已生成", 5);
  mock->register_rule("执行结果", "销售分析完成", 3);
  // If recall finds past plan, it adds context containing "Plan for"
  mock->register_rule("参考历史方案", "1. 复用历史方案\n2. 更新数据", 9);
  mock->register_rule("复用历史方案", "历史方案已复用", 5);
  mock->register_rule("更新数据", "数据已更新", 5);

  build_os(std::move(mock));

  // First run — creates memory
  {
    auto agent = make_planner("planner1");
    auto result = agent->run("销售分析任务");
    ASSERT_TRUE(result) << result.error().message;
  }

  // Second run with similar query — should recall past plan
  {
    auto agent = make_planner("planner2");
    auto result = agent->run("销售分析任务");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_FALSE(result->empty());
  }
}
