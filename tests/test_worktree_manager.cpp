#include <agentos/worktree/worktree_manager.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace agentos::worktree;

class WorktreeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo_root_ = fs::current_path();
        worktree_base_ = repo_root_ / "build" / "test_worktrees";
        if (fs::exists(worktree_base_)) {
            fs::remove_all(worktree_base_);
        }
    }

    void TearDown() override {
        // Clean up any worktrees created during tests
        if (mgr_) {
            auto listed = mgr_->list();
            if (listed.has_value()) {
                for (const auto& wt : listed.value()) {
                    (void)mgr_->remove(wt.name, /*force=*/true);
                }
            }
        }
        if (fs::exists(worktree_base_)) {
            fs::remove_all(worktree_base_);
        }
    }

    void make_mgr(uint32_t max_concurrent = 4) {
        mgr_ = std::make_unique<WorktreeManager>(
            WorktreeConfig{repo_root_, worktree_base_, max_concurrent});
    }

    fs::path repo_root_;
    fs::path worktree_base_;
    std::unique_ptr<WorktreeManager> mgr_;
};

// ── Skeleton smoke tests ──

TEST_F(WorktreeManagerTest, ConstructsAndCreatesBaseDir) {
    make_mgr();
    EXPECT_TRUE(fs::exists(worktree_base_));
}

TEST_F(WorktreeManagerTest, EmptyListReturnsEmpty) {
    make_mgr();
    auto res = mgr_->list();
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(res->empty());
}

TEST_F(WorktreeManagerTest, ActiveCountStartsAtZero) {
    make_mgr();
    EXPECT_EQ(mgr_->active_count(), 0u);
    EXPECT_FALSE(mgr_->at_capacity());
}

TEST_F(WorktreeManagerTest, GetNonExistentReturnsNotFound) {
    make_mgr();
    auto res = mgr_->get("nonexistent");
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, agentos::ErrorCode::NotFound);
}

// ── CRUD tests ──

TEST_F(WorktreeManagerTest, CreateWorktree) {
    make_mgr();
    auto res = mgr_->create("test-wt");
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(res->name, "test-wt");
    EXPECT_EQ(res->branch, "agent/test-wt");
    EXPECT_EQ(res->state, WorktreeState::Active);
    EXPECT_TRUE(fs::exists(res->path));
    EXPECT_TRUE(fs::exists(res->path / ".git"));
    EXPECT_EQ(mgr_->active_count(), 1u);
}

TEST_F(WorktreeManagerTest, CreateDuplicateReturnsAlreadyExists) {
    make_mgr();
    auto res1 = mgr_->create("dup-wt");
    ASSERT_TRUE(res1.has_value()) << res1.error().message;

    auto res2 = mgr_->create("dup-wt");
    ASSERT_FALSE(res2.has_value());
    EXPECT_EQ(res2.error().code, agentos::ErrorCode::AlreadyExists);
}

TEST_F(WorktreeManagerTest, CreateRespectsCapacity) {
    make_mgr(/*max_concurrent=*/2);
    ASSERT_TRUE(mgr_->create("wt-a").has_value());
    ASSERT_TRUE(mgr_->create("wt-b").has_value());
    EXPECT_TRUE(mgr_->at_capacity());

    auto res = mgr_->create("wt-c");
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, agentos::ErrorCode::WorktreeCapacityFull);
}

TEST_F(WorktreeManagerTest, RemoveWorktree) {
    make_mgr();
    auto created = mgr_->create("rm-wt");
    ASSERT_TRUE(created.has_value()) << created.error().message;
    fs::path wt_path = created->path;

    auto rm_res = mgr_->remove("rm-wt");
    ASSERT_TRUE(rm_res.has_value()) << rm_res.error().message;
    EXPECT_EQ(mgr_->active_count(), 0u);
    EXPECT_FALSE(fs::exists(wt_path));
}

TEST_F(WorktreeManagerTest, RemoveNonExistentReturnsNotFound) {
    make_mgr();
    auto res = mgr_->remove("ghost");
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, agentos::ErrorCode::NotFound);
}

TEST_F(WorktreeManagerTest, GetAfterCreate) {
    make_mgr();
    auto created = mgr_->create("get-wt");
    ASSERT_TRUE(created.has_value()) << created.error().message;

    auto got = mgr_->get("get-wt");
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(got->name, "get-wt");
    EXPECT_EQ(got->branch, created->branch);
}

TEST_F(WorktreeManagerTest, ListAfterMultipleCreates) {
    make_mgr();
    ASSERT_TRUE(mgr_->create("list-a").has_value());
    ASSERT_TRUE(mgr_->create("list-b").has_value());

    auto listed = mgr_->list();
    ASSERT_TRUE(listed.has_value());
    EXPECT_EQ(listed->size(), 2u);
}

// ── Recovery & lifecycle tests ──

TEST_F(WorktreeManagerTest, HasChangesReturnsFalseForCleanWorktree) {
    make_mgr();
    auto created = mgr_->create("clean-wt");
    ASSERT_TRUE(created.has_value()) << created.error().message;

    auto res = mgr_->has_changes("clean-wt");
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_FALSE(res.value());
}

TEST_F(WorktreeManagerTest, HasChangesReturnsTrueAfterModification) {
    make_mgr();
    auto created = mgr_->create("dirty-wt");
    ASSERT_TRUE(created.has_value()) << created.error().message;

    // Create an untracked file in the worktree
    auto test_file = created->path / "test_dirty.txt";
    std::ofstream(test_file) << "dirty";

    auto res = mgr_->has_changes("dirty-wt");
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_TRUE(res.value());
}

TEST_F(WorktreeManagerTest, HasChangesNonExistentReturnsNotFound) {
    make_mgr();
    auto res = mgr_->has_changes("nope");
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, agentos::ErrorCode::NotFound);
}

TEST_F(WorktreeManagerTest, RecoverFindsOrphanedWorktrees) {
    // Create a worktree with one manager, then recover with a fresh one
    make_mgr();
    auto created = mgr_->create("orphan-wt");
    ASSERT_TRUE(created.has_value()) << created.error().message;

    // Create a fresh manager (simulates restart — no in-memory state)
    auto mgr2 = std::make_unique<WorktreeManager>(
        WorktreeConfig{repo_root_, worktree_base_, 4});
    EXPECT_EQ(mgr2->active_count(), 0u);

    auto rec_res = mgr2->recover();
    ASSERT_TRUE(rec_res.has_value()) << rec_res.error().message;
    EXPECT_EQ(mgr2->active_count(), 1u);

    auto got = mgr2->get("orphan-wt");
    ASSERT_TRUE(got.has_value()) << got.error().message;
    EXPECT_EQ(got->branch, "agent/orphan-wt");

    // Clean up through the original manager to avoid TearDown issues
}
