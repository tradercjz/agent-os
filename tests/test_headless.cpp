#include <agentos/headless/runner.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::headless;

class HeadlessRunnerTest : public ::testing::Test {
protected:
    std::unique_ptr<HeadlessRunner> make_runner() {
        return std::make_unique<HeadlessRunner>(
            std::make_unique<kernel::MockLLMBackend>("mock"),
            AgentOS::Config::builder().scheduler_threads(1).build());
    }
};

TEST_F(HeadlessRunnerTest, RunWithMockBackend) {
    auto runner = make_runner();
    RunRequest req;
    req.task = "What is 2+2?";
    req.role_prompt = "You are a math tutor.";

    auto result = runner->run(req);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.output.empty());
}

TEST_F(HeadlessRunnerTest, EmptyTaskReturnsError) {
    auto runner = make_runner();
    RunRequest req;
    req.task = "";

    auto result = runner->run(req);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "Empty task");
}

TEST_F(HeadlessRunnerTest, FromJsonParsesCorrectly) {
    Json j;
    j["task"] = "Deploy the app";
    j["agent_name"] = "deployer";
    j["role_prompt"] = "You are a DevOps agent.";
    j["timeout_ms"] = 5000;
    j["context_limit"] = 2048;
    j["allowed_tools"] = Json::array({"shell"});

    auto req = RunRequest::from_json(j);
    EXPECT_EQ(req.task, "Deploy the app");
    EXPECT_EQ(req.agent_name, "deployer");
    EXPECT_EQ(req.role_prompt, "You are a DevOps agent.");
    EXPECT_EQ(req.timeout.count(), 5000);
    EXPECT_EQ(req.context_limit, 2048u);
    ASSERT_EQ(req.allowed_tools.size(), 1u);
    EXPECT_EQ(req.allowed_tools[0], "shell");
}

TEST_F(HeadlessRunnerTest, ToJsonRoundTrip) {
    RunRequest req;
    req.task = "test task";
    req.agent_name = "test-agent";

    auto j = req.to_json();
    auto req2 = RunRequest::from_json(j);
    EXPECT_EQ(req2.task, req.task);
    EXPECT_EQ(req2.agent_name, req.agent_name);
}

TEST_F(HeadlessRunnerTest, RunResultToJson) {
    RunResult result;
    result.success = true;
    result.output = "Done!";
    result.duration_ms = 1500;
    result.tokens_used = 42;

    auto j = result.to_json();
    EXPECT_EQ(j["success"], true);
    EXPECT_EQ(j["output"], "Done!");
    EXPECT_EQ(j["duration_ms"], 1500);
    EXPECT_EQ(j["tokens_used"], 42);
}

TEST_F(HeadlessRunnerTest, RunJsonString) {
    auto runner = make_runner();
    std::string json = R"({"task":"Hello world","agent_name":"json-agent"})";

    auto result = runner->run_json(json);
    EXPECT_TRUE(result.success);
}

TEST_F(HeadlessRunnerTest, RunJsonInvalidReturnsError) {
    auto runner = make_runner();
    auto result = runner->run_json("not valid json{{{");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, "Invalid JSON");
}

TEST_F(HeadlessRunnerTest, ToolRegistrationOnRunner) {
    auto runner = make_runner();

    // Register a tool via the runner's OS
    runner->os().register_tool(
        tools::ToolSchema{.id = "greet", .description = "Greet"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Hello!");
        });

    RunRequest req;
    req.task = "Say hello";
    auto result = runner->run(req);
    EXPECT_TRUE(result.success);
}
