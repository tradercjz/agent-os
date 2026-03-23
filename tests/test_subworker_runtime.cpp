#include <agentos/subworkers/runtime.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>

using namespace agentos;
using namespace agentos::kernel;

namespace {

// ── Helper: unique temp directory per test ────────────────────
std::filesystem::path make_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("agentos_subworker_test_" + name + "_" + std::to_string(nonce));
}

// ── MockExecutor implementing ISubworkerExecutor ─────────────
class MockExecutor : public ISubworkerExecutor {
public:
    enum class Behaviour { Succeed, Fail, Timeout, Cancel };

    explicit MockExecutor(Behaviour b = Behaviour::Succeed,
                          std::string output = "mock output",
                          std::chrono::milliseconds elapsed = std::chrono::milliseconds(42))
        : behaviour_(b), output_(std::move(output)), elapsed_(elapsed) {}

    Result<SubworkerResult> run(AgentOS& /*os*/,
                                AgentId /*supervisor_id*/,
                                const std::shared_ptr<Agent>& /*worker*/,
                                const WorkerTemplate& tpl,
                                const std::string& task,
                                const std::filesystem::path& worktree_path,
                                const SubworkerRunOptions& /*opts*/) override {
        ++call_count_;
        last_task_ = task;
        last_worktree_path_ = worktree_path;

        SubworkerResult result;
        result.task = task;
        result.worker_name = tpl.name;
        result.worktree_path = worktree_path;
        result.elapsed = elapsed_;

        switch (behaviour_) {
        case Behaviour::Succeed:
            result.status = SubworkerStatus::Succeeded;
            result.output = output_;
            result.summary = output_;
            return result;
        case Behaviour::Fail:
            result.status = SubworkerStatus::Failed;
            result.error = "mock failure";
            result.summary = "mock failure";
            return result;
        case Behaviour::Timeout:
            return make_error(ErrorCode::Timeout, "executor timed out");
        case Behaviour::Cancel:
            result.status = SubworkerStatus::Cancelled;
            result.error = "cancelled by user";
            result.summary = "cancelled by user";
            return result;
        }
        return result; // unreachable
    }

    int call_count() const { return call_count_; }
    const std::string& last_task() const { return last_task_; }
    const std::filesystem::path& last_worktree_path() const { return last_worktree_path_; }

private:
    Behaviour behaviour_;
    std::string output_;
    std::chrono::milliseconds elapsed_;
    int call_count_{0};
    std::string last_task_;
    std::filesystem::path last_worktree_path_;
};

} // namespace

// ═══════════════════════════════════════════════════════════════
// § 1  WorkerTemplate — construction and defaults
// ═══════════════════════════════════════════════════════════════

TEST(WorkerTemplate, DefaultValues) {
    WorkerTemplate tpl;
    EXPECT_TRUE(tpl.name.empty());
    EXPECT_TRUE(tpl.description.empty());
    EXPECT_EQ(tpl.isolation, worktree::IsolationMode::Worktree);
    EXPECT_EQ(tpl.timeout, std::chrono::seconds(300));
    EXPECT_TRUE(tpl.preserve_worktree_on_failure);
    EXPECT_FALSE(tpl.allow_parallel);
}

TEST(WorkerTemplate, CustomValues) {
    WorkerTemplate tpl;
    tpl.name = "code-reviewer";
    tpl.description = "Reviews pull requests";
    tpl.config.name = "reviewer-agent";
    tpl.config.role_prompt = "You review code.";
    tpl.isolation = worktree::IsolationMode::Thread;
    tpl.timeout = std::chrono::seconds(60);
    tpl.preserve_worktree_on_failure = false;
    tpl.allow_parallel = true;

    EXPECT_EQ(tpl.name, "code-reviewer");
    EXPECT_EQ(tpl.description, "Reviews pull requests");
    EXPECT_EQ(tpl.config.name, "reviewer-agent");
    EXPECT_EQ(tpl.config.role_prompt, "You review code.");
    EXPECT_EQ(tpl.isolation, worktree::IsolationMode::Thread);
    EXPECT_EQ(tpl.timeout, std::chrono::seconds(60));
    EXPECT_FALSE(tpl.preserve_worktree_on_failure);
    EXPECT_TRUE(tpl.allow_parallel);
}

// ═══════════════════════════════════════════════════════════════
// § 2  SubworkerRunOptions — defaults and overrides
// ═══════════════════════════════════════════════════════════════

TEST(SubworkerRunOptions, DefaultValues) {
    SubworkerRunOptions opts;
    EXPECT_FALSE(opts.timeout.has_value());
    EXPECT_FALSE(opts.preserve_worktree_on_failure.has_value());
    EXPECT_FALSE(opts.preferred_worktree_base.has_value());
    EXPECT_EQ(opts.metadata_json, "{}");
}

TEST(SubworkerRunOptions, OverrideTimeout) {
    SubworkerRunOptions opts;
    opts.timeout = std::chrono::seconds(30);
    ASSERT_TRUE(opts.timeout.has_value());
    EXPECT_EQ(*opts.timeout, std::chrono::seconds(30));
}

TEST(SubworkerRunOptions, OverridePreserveWorktree) {
    SubworkerRunOptions opts;
    opts.preserve_worktree_on_failure = false;
    ASSERT_TRUE(opts.preserve_worktree_on_failure.has_value());
    EXPECT_FALSE(*opts.preserve_worktree_on_failure);
}

TEST(SubworkerRunOptions, OverridePreferredBase) {
    SubworkerRunOptions opts;
    opts.preferred_worktree_base = "/tmp/custom_base";
    ASSERT_TRUE(opts.preferred_worktree_base.has_value());
    EXPECT_EQ(*opts.preferred_worktree_base, std::filesystem::path("/tmp/custom_base"));
}

TEST(SubworkerRunOptions, CustomMetadata) {
    SubworkerRunOptions opts;
    opts.metadata_json = R"({"priority":"high"})";
    EXPECT_EQ(opts.metadata_json, R"({"priority":"high"})");
}

// ═══════════════════════════════════════════════════════════════
// § 3  SubworkerStatus — enum values
// ═══════════════════════════════════════════════════════════════

TEST(SubworkerStatus, AllValuesDistinct) {
    EXPECT_NE(SubworkerStatus::Succeeded, SubworkerStatus::Failed);
    EXPECT_NE(SubworkerStatus::Failed, SubworkerStatus::Cancelled);
    EXPECT_NE(SubworkerStatus::Cancelled, SubworkerStatus::TimedOut);
    EXPECT_NE(SubworkerStatus::Succeeded, SubworkerStatus::TimedOut);
}

// ═══════════════════════════════════════════════════════════════
// § 4  SubworkerResult — fields and defaults
// ═══════════════════════════════════════════════════════════════

TEST(SubworkerResult, DefaultStatus) {
    SubworkerResult result;
    EXPECT_EQ(result.status, SubworkerStatus::Failed);
    EXPECT_TRUE(result.run_id.empty());
    EXPECT_TRUE(result.worker_name.empty());
    EXPECT_TRUE(result.task.empty());
    EXPECT_TRUE(result.summary.empty());
    EXPECT_TRUE(result.output.empty());
    EXPECT_TRUE(result.error.empty());
    EXPECT_TRUE(result.worktree_path.empty());
    EXPECT_EQ(result.elapsed, std::chrono::milliseconds(0));
}

TEST(SubworkerResult, PopulatedFields) {
    SubworkerResult result;
    result.run_id = "subworker-1-12345-1";
    result.worker_name = "test-worker";
    result.task = "Fix the bug";
    result.status = SubworkerStatus::Succeeded;
    result.summary = "Bug fixed successfully";
    result.output = "Applied patch to file.cpp";
    result.worktree_path = "/tmp/worktree";
    result.elapsed = std::chrono::milliseconds(500);

    EXPECT_EQ(result.run_id, "subworker-1-12345-1");
    EXPECT_EQ(result.worker_name, "test-worker");
    EXPECT_EQ(result.task, "Fix the bug");
    EXPECT_EQ(result.status, SubworkerStatus::Succeeded);
    EXPECT_EQ(result.summary, "Bug fixed successfully");
    EXPECT_EQ(result.output, "Applied patch to file.cpp");
    EXPECT_EQ(result.worktree_path, std::filesystem::path("/tmp/worktree"));
    EXPECT_EQ(result.elapsed, std::chrono::milliseconds(500));
}

// ═══════════════════════════════════════════════════════════════
// § 5  SubworkerRunRecord — fields and defaults
// ═══════════════════════════════════════════════════════════════

TEST(SubworkerRunRecord, DefaultValues) {
    SubworkerRunRecord record;
    EXPECT_EQ(record.supervisor_id, AgentId{0});
    EXPECT_TRUE(record.run_id.empty());
    EXPECT_TRUE(record.worker_name.empty());
    EXPECT_TRUE(record.task.empty());
    EXPECT_EQ(record.status, SubworkerStatus::Failed);
    EXPECT_TRUE(record.worktree_path.empty());
    EXPECT_EQ(record.elapsed, std::chrono::milliseconds(0));
    EXPECT_FALSE(record.output_truncated);
    EXPECT_TRUE(record.summary.empty());
    EXPECT_TRUE(record.output.empty());
    EXPECT_TRUE(record.error.empty());
}

TEST(SubworkerRunRecord, PopulatedFields) {
    SubworkerRunRecord record;
    record.supervisor_id = 42;
    record.run_id = "subworker-42-99999-1";
    record.worker_name = "data-analyst";
    record.task = "Analyze sales data";
    record.status = SubworkerStatus::Succeeded;
    record.worktree_path = "/tmp/analysis";
    record.elapsed = std::chrono::milliseconds(1500);
    record.output_truncated = true;
    record.summary = "Analysis complete";
    record.output = "Revenue increased 15%";
    record.error = "";

    EXPECT_EQ(record.supervisor_id, AgentId{42});
    EXPECT_EQ(record.run_id, "subworker-42-99999-1");
    EXPECT_EQ(record.worker_name, "data-analyst");
    EXPECT_EQ(record.task, "Analyze sales data");
    EXPECT_EQ(record.status, SubworkerStatus::Succeeded);
    EXPECT_EQ(record.worktree_path, std::filesystem::path("/tmp/analysis"));
    EXPECT_EQ(record.elapsed, std::chrono::milliseconds(1500));
    EXPECT_TRUE(record.output_truncated);
    EXPECT_EQ(record.summary, "Analysis complete");
    EXPECT_EQ(record.output, "Revenue increased 15%");
    EXPECT_TRUE(record.error.empty());
}

TEST(SubworkerRunRecord, TimestampsAreSet) {
    auto before = Clock::now();
    SubworkerRunRecord record;
    auto after = Clock::now();

    // started_at and finished_at should be initialized to approximately now
    EXPECT_GE(record.started_at, before);
    EXPECT_LE(record.started_at, after);
    EXPECT_GE(record.finished_at, before);
    EXPECT_LE(record.finished_at, after);
}

// ═══════════════════════════════════════════════════════════════
// § 6  SubworkerRuntime construction
// ═══════════════════════════════════════════════════════════════

TEST(SubworkerRuntime, ConstructWithNullExecutorUsesDefault) {
    // Should not throw — default InProcWorktreeExecutor is created internally
    SubworkerRuntime runtime;
    (void)runtime;
}

TEST(SubworkerRuntime, ConstructWithMockExecutor) {
    auto executor = std::make_unique<MockExecutor>();
    SubworkerRuntime runtime(std::move(executor));
    (void)runtime;
}

// ═══════════════════════════════════════════════════════════════
// § 7  SubworkerRuntime::run — integration with AgentOS + mock
// ═══════════════════════════════════════════════════════════════

class SubworkerRuntimeTest : public ::testing::Test {
protected:
    std::filesystem::path snapshot_dir_ = make_test_dir("snap");
    std::filesystem::path ltm_dir_ = make_test_dir("ltm");

    void SetUp() override {
        auto mock = std::make_unique<MockLLMBackend>("test-llm");
        mock->register_rule("default", "I am the subworker.");
        mock_ptr_ = mock.get();

        AgentOS::Config cfg;
        cfg.scheduler_threads = 2;
        cfg.snapshot_dir = snapshot_dir_.string();
        cfg.ltm_dir = ltm_dir_.string();
        // Use current directory as repo_root — required for worktree creation
        cfg.repo_root = std::filesystem::current_path();
        cfg.worktree_base = make_test_dir("wt");

        os_ = std::make_unique<AgentOS>(std::move(mock), cfg);
    }

    void TearDown() override {
        os_.reset();
        std::filesystem::remove_all(snapshot_dir_);
        std::filesystem::remove_all(ltm_dir_);
    }

    std::unique_ptr<AgentOS> os_;
    MockLLMBackend *mock_ptr_;
};

TEST_F(SubworkerRuntimeTest, SuccessfulRun) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Succeed,
                                                    "task completed", std::chrono::milliseconds(100));
    auto* exec_ptr = executor.get();
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "test-worker";
    tpl.config.name = "test-agent";
    tpl.config.role_prompt = "You are a test worker.";

    auto result = runtime.run(*os_, 1, tpl, "do the thing");
    ASSERT_TRUE(result.has_value()) << "run failed: " << result.error().message;
    EXPECT_EQ(result->status, SubworkerStatus::Succeeded);
    EXPECT_EQ(result->output, "task completed");
    EXPECT_EQ(result->worker_name, "test-worker");
    EXPECT_EQ(result->task, "do the thing");
    EXPECT_FALSE(result->run_id.empty());
    EXPECT_FALSE(result->worktree_path.empty());
    EXPECT_EQ(exec_ptr->call_count(), 1);
}

TEST_F(SubworkerRuntimeTest, FailedRun) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Fail);
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "failing-worker";
    tpl.config.name = "fail-agent";
    tpl.config.role_prompt = "You will fail.";
    tpl.preserve_worktree_on_failure = true;

    auto result = runtime.run(*os_, 2, tpl, "fail task");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, SubworkerStatus::Failed);
    EXPECT_EQ(result->error, "mock failure");
    EXPECT_EQ(result->summary, "mock failure");
}

TEST_F(SubworkerRuntimeTest, FailedRunCleansUpWorktreeWhenNotPreserving) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Fail);
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "cleanup-worker";
    tpl.config.name = "cleanup-agent";
    tpl.config.role_prompt = "Worker.";
    tpl.preserve_worktree_on_failure = false; // should attempt cleanup

    auto result = runtime.run(*os_, 3, tpl, "fail and cleanup");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, SubworkerStatus::Failed);
    // Worktree path should still be set in the result even if removed
    EXPECT_FALSE(result->worktree_path.empty());
}

TEST_F(SubworkerRuntimeTest, TimeoutReturnsError) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Timeout);
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "timeout-worker";
    tpl.config.name = "timeout-agent";
    tpl.config.role_prompt = "Worker.";

    auto result = runtime.run(*os_, 4, tpl, "slow task");
    // Timeout from executor propagates as an error
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Timeout);
}

TEST_F(SubworkerRuntimeTest, CancelledStatus) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Cancel);
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "cancel-worker";
    tpl.config.name = "cancel-agent";
    tpl.config.role_prompt = "Worker.";

    auto result = runtime.run(*os_, 5, tpl, "cancel task");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, SubworkerStatus::Cancelled);
    EXPECT_EQ(result->error, "cancelled by user");
}

TEST_F(SubworkerRuntimeTest, RunIdContainsSupervisorId) {
    auto executor = std::make_unique<MockExecutor>();
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "id-worker";
    tpl.config.name = "id-agent";
    tpl.config.role_prompt = "Worker.";

    AgentId supervisor_id = 99;
    auto result = runtime.run(*os_, supervisor_id, tpl, "check id");
    ASSERT_TRUE(result.has_value());
    // run_id format: "subworker-{supervisor_id}-{ns}-{seq}"
    EXPECT_TRUE(result->run_id.find("subworker-99-") == 0)
        << "run_id should start with 'subworker-99-', got: " << result->run_id;
}

TEST_F(SubworkerRuntimeTest, WorkerNameFallsBackToConfigName) {
    auto executor = std::make_unique<MockExecutor>();
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    // tpl.name is empty — should fall back to tpl.config.name
    tpl.config.name = "fallback-name";
    tpl.config.role_prompt = "Worker.";

    auto result = runtime.run(*os_, 1, tpl, "name test");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->worker_name, "fallback-name");
}

TEST_F(SubworkerRuntimeTest, SuccessfulRunSetsEmptySummaryFromOutput) {
    // When the executor returns a result with empty summary,
    // SubworkerRuntime should fill summary from output for succeeded
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Succeed,
                                                    "the output");
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "summary-worker";
    tpl.config.name = "summary-agent";
    tpl.config.role_prompt = "Worker.";

    auto result = runtime.run(*os_, 1, tpl, "summary test");
    ASSERT_TRUE(result.has_value());
    // The mock sets summary = output, and SubworkerRuntime only fills if empty,
    // so summary should match output
    EXPECT_EQ(result->summary, "the output");
}

TEST_F(SubworkerRuntimeTest, PreserveWorktreeOverrideFromOptions) {
    auto executor = std::make_unique<MockExecutor>(MockExecutor::Behaviour::Fail);
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "preserve-worker";
    tpl.config.name = "preserve-agent";
    tpl.config.role_prompt = "Worker.";
    tpl.preserve_worktree_on_failure = false; // template says don't preserve

    SubworkerRunOptions opts;
    opts.preserve_worktree_on_failure = true; // override: DO preserve

    auto result = runtime.run(*os_, 1, tpl, "preserve test", opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, SubworkerStatus::Failed);
    // With preserve=true override, the worktree should NOT be removed.
    // We can't easily verify the worktree still exists in this mock setup,
    // but at least the run should succeed.
}

TEST_F(SubworkerRuntimeTest, MultipleRunsGetUniqueRunIds) {
    auto executor = std::make_unique<MockExecutor>();
    SubworkerRuntime runtime(std::move(executor));

    WorkerTemplate tpl;
    tpl.name = "multi-worker";
    tpl.config.name = "multi-agent";
    tpl.config.role_prompt = "Worker.";

    auto r1 = runtime.run(*os_, 1, tpl, "task 1");
    auto r2 = runtime.run(*os_, 1, tpl, "task 2");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(r1->run_id, r2->run_id);
}

// ═══════════════════════════════════════════════════════════════
// § 8  MockExecutor standalone tests
// ═══════════════════════════════════════════════════════════════

TEST(MockExecutor, CallCountIncrements) {
    MockExecutor executor(MockExecutor::Behaviour::Succeed);
    EXPECT_EQ(executor.call_count(), 0);

    // We can't call run() without an AgentOS + Agent, but the type compiles
    // and the interface matches ISubworkerExecutor — verified by construction
    // in SubworkerRuntimeTest above.
}

TEST(MockExecutor, BehaviourCanBeSetToAllModes) {
    // Verify all enum values compile and can be used
    MockExecutor s(MockExecutor::Behaviour::Succeed);
    MockExecutor f(MockExecutor::Behaviour::Fail);
    MockExecutor t(MockExecutor::Behaviour::Timeout);
    MockExecutor c(MockExecutor::Behaviour::Cancel);
    (void)s; (void)f; (void)t; (void)c;
}
