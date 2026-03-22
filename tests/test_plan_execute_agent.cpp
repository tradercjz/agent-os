#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

// ── PlanExecStep struct tests ───────────────────────────────

TEST(PlanExecuteAgentTest, PlanStepDefaultValues) {
    PlanExecStep step;
    EXPECT_EQ(step.step_number, 0);
    EXPECT_EQ(step.status, "pending");
    EXPECT_TRUE(step.description.empty());
    EXPECT_TRUE(step.result.empty());
}

TEST(PlanExecuteAgentTest, PlanStepInitialization) {
    PlanExecStep step{1, "Do something", "pending", ""};
    EXPECT_EQ(step.step_number, 1);
    EXPECT_EQ(step.description, "Do something");
    EXPECT_EQ(step.status, "pending");
}

// ── Integration tests with mock backend ─────────────────────

TEST(PlanExecuteAgentTest, RunWithMockBackend) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "planner")
                     .prompt("You plan tasks")
                     .create_plan_execute();
    auto result = agent->run("Organize a meeting");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
    // Plan should have been generated (at least fallback single step)
    EXPECT_GE(agent->current_plan().size(), 1u);
}

TEST(PlanExecuteAgentTest, MaxStepsConfigurable) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "planner2")
                     .prompt("plan")
                     .create_plan_execute();
    agent->set_max_steps(5);
    auto result = agent->run("simple task");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(PlanExecuteAgentTest, MaxReplansConfigurable) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "planner3")
                     .prompt("plan")
                     .create_plan_execute();
    agent->set_max_replan_attempts(1);
    auto result = agent->run("another task");
    ASSERT_TRUE(result.has_value());
}

TEST(PlanExecuteAgentTest, CreateViaTemplateMethod) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "pe-tmpl")
                     .prompt("plan")
                     .create<PlanExecuteAgent>();
    auto result = agent->run("hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(PlanExecuteAgentTest, PlanAccessibleAfterRun) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "pe-inspect")
                     .prompt("You plan carefully")
                     .create_plan_execute();

    // Before run, plan should be empty
    EXPECT_TRUE(agent->current_plan().empty());

    auto result = agent->run("Build a house");
    ASSERT_TRUE(result.has_value());

    // After run, plan should be populated
    EXPECT_FALSE(agent->current_plan().empty());

    // All steps should have terminal status
    for (const auto& step : agent->current_plan()) {
        EXPECT_TRUE(step.status == "done" || step.status == "failed");
    }
}
