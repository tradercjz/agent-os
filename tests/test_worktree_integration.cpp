#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace agentos;

class WorktreeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo_root_ = fs::current_path();
        wt_base_ = repo_root_ / "build" / "test_wt_integration";
        if (fs::exists(wt_base_)) fs::remove_all(wt_base_);

        auto backend = std::make_unique<kernel::MockLLMBackend>("mock");
        os_ = std::make_unique<AgentOS>(
            std::move(backend),
            AgentOS::Config::builder()
                .scheduler_threads(1)
                .repo_root(repo_root_)
                .worktree_base(wt_base_)
                .max_worktrees(3)
                .build());
    }

    void TearDown() override {
        os_.reset();
        // Clean up any leftover worktrees and branches
        std::string prune = "git -C '" + repo_root_.string()
                          + "' worktree prune 2>/dev/null";
        (void)system(prune.c_str());

        // Delete agent/* branches left behind
        std::string cleanup = "git -C '" + repo_root_.string()
                            + "' branch --list 'agent/*' | xargs -r git -C '"
                            + repo_root_.string() + "' branch -D 2>/dev/null";
        (void)system(cleanup.c_str());

        if (fs::exists(wt_base_)) {
            fs::remove_all(wt_base_);
        }
    }

    fs::path repo_root_;
    fs::path wt_base_;
    std::unique_ptr<AgentOS> os_;
};

TEST_F(WorktreeIntegrationTest, ThreadModeUsesCurrentDir) {
    auto agent = os_->create_agent(
        AgentConfig::builder()
            .name("thread-agent")
            .isolation(worktree::IsolationMode::Thread)
            .build());

    EXPECT_EQ(agent->work_dir(), fs::current_path());
}

TEST_F(WorktreeIntegrationTest, WorktreeModeCreatesWorktree) {
    auto agent = os_->create_agent(
        AgentConfig::builder()
            .name("wt-agent")
            .isolation(worktree::IsolationMode::Worktree)
            .build());

    EXPECT_NE(agent->work_dir(), fs::current_path());
    EXPECT_TRUE(fs::exists(agent->work_dir()));
    EXPECT_TRUE(fs::exists(agent->work_dir() / ".git"));
}

TEST_F(WorktreeIntegrationTest, DestroyAgentCleansUpWorktree) {
    AgentId aid;
    fs::path wt_path;
    {
        auto agent = os_->create_agent(
            AgentConfig::builder()
                .name("cleanup-agent")
                .isolation(worktree::IsolationMode::Worktree)
                .build());
        aid = agent->id();
        wt_path = agent->work_dir();
        ASSERT_TRUE(fs::exists(wt_path));
    }

    os_->destroy_agent(aid);
    // Worktree directory should be removed
    EXPECT_FALSE(fs::exists(wt_path));
}

TEST_F(WorktreeIntegrationTest, WorktreeManagerAccessible) {
    EXPECT_EQ(os_->worktree_mgr().active_count(), 0u);

    auto agent = os_->create_agent(
        AgentConfig::builder()
            .name("mgr-test")
            .isolation(worktree::IsolationMode::Worktree)
            .build());

    EXPECT_EQ(os_->worktree_mgr().active_count(), 1u);
}

TEST_F(WorktreeIntegrationTest, MultipleWorktreeAgents) {
    auto a1 = os_->create_agent(
        AgentConfig::builder()
            .name("multi-a")
            .isolation(worktree::IsolationMode::Worktree)
            .build());
    auto a2 = os_->create_agent(
        AgentConfig::builder()
            .name("multi-b")
            .isolation(worktree::IsolationMode::Worktree)
            .build());

    EXPECT_NE(a1->work_dir(), a2->work_dir());
    EXPECT_EQ(os_->worktree_mgr().active_count(), 2u);
}

TEST_F(WorktreeIntegrationTest, MixedIsolationModes) {
    auto thread_agent = os_->create_agent(
        AgentConfig::builder()
            .name("mixed-thread")
            .isolation(worktree::IsolationMode::Thread)
            .build());
    auto wt_agent = os_->create_agent(
        AgentConfig::builder()
            .name("mixed-wt")
            .isolation(worktree::IsolationMode::Worktree)
            .build());

    EXPECT_EQ(thread_agent->work_dir(), fs::current_path());
    EXPECT_NE(wt_agent->work_dir(), fs::current_path());
    EXPECT_EQ(os_->worktree_mgr().active_count(), 1u);
}
