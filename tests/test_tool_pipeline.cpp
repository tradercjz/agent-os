#include <agentos/tools/tool_pipeline.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::tools;

class ToolPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        os_ = quickstart_mock();
    }
    std::unique_ptr<AgentOS> os_;
};

TEST_F(ToolPipelineTest, SingleStepPipeline) {
    ToolPipeline pipe(os_->tools());
    pipe.then("kv_store", [](const std::string& input) {
        nlohmann::json j;
        j["op"] = "set";
        j["key"] = "test";
        j["value"] = input;
        return j.dump();
    });

    auto result = pipe.execute("hello");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.steps_executed, 1u);
}

TEST_F(ToolPipelineTest, ChainedPipeline) {
    // Set then get
    ToolPipeline pipe(os_->tools());
    pipe.then("kv_store", [](const std::string&) {
        return R"({"op":"set","key":"pipe_test","value":"chained"})";
    }).then("kv_store", [](const std::string&) {
        return R"({"op":"get","key":"pipe_test"})";
    });

    auto result = pipe.execute("");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.steps_executed, 2u);
    EXPECT_NE(result.final_output.find("chained"), std::string::npos);
}

TEST_F(ToolPipelineTest, ConditionalStep) {
    ToolPipeline pipe(os_->tools());
    pipe.then("kv_store", [](const std::string&) {
        return R"({"op":"set","key":"cond","value":"yes"})";
    }).then_if(
        [](const std::string& prev) { return prev.find("yes") != std::string::npos; },
        "kv_store",
        [](const std::string&) {
            return R"({"op":"get","key":"cond"})";
        }
    );

    auto result = pipe.execute("");
    EXPECT_TRUE(result.success);
}

TEST_F(ToolPipelineTest, FailingStepStopsPipeline) {
    ToolPipeline pipe(os_->tools());
    pipe.then("nonexistent_tool")  // will fail
        .then("kv_store");         // should not execute

    auto result = pipe.execute("");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.steps_executed, 1u);
}

TEST_F(ToolPipelineTest, EmptyPipelineSucceeds) {
    ToolPipeline pipe(os_->tools());
    auto result = pipe.execute("input");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.final_output, "input");
}

TEST_F(ToolPipelineTest, StepCount) {
    ToolPipeline pipe(os_->tools());
    pipe.then("a").then("b").then("c");
    EXPECT_EQ(pipe.step_count(), 3u);
    pipe.clear();
    EXPECT_EQ(pipe.step_count(), 0u);
}

// Parallel tests
TEST_F(ToolPipelineTest, ParallelExecution) {
    ToolParallel par(os_->tools());
    par.add("kv_store", R"({"op":"set","key":"p1","value":"v1"})");
    par.add("kv_store", R"({"op":"set","key":"p2","value":"v2"})");

    auto result = par.execute();
    EXPECT_TRUE(result.all_success);
    EXPECT_EQ(result.results.size(), 2u);
}

TEST_F(ToolPipelineTest, ParallelWithFailure) {
    ToolParallel par(os_->tools());
    par.add("kv_store", R"({"op":"set","key":"ok","value":"v"})");
    par.add("nonexistent", "{}");

    auto result = par.execute();
    EXPECT_FALSE(result.all_success);
    EXPECT_EQ(result.results.size(), 2u);
}
