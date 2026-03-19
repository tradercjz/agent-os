# Worktree Isolation Phase 1: Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `WorktreeManager` to provide directory-level isolation for agents using Git worktrees.

**Architecture:** 
- `WorktreeManager` manages a pool of git worktrees in a dedicated directory (`.agentos/worktrees/`).
- Each worktree is associated with a unique branch.
- Provides CRUD operations and crash recovery (reconciling disk state with git state).
- Includes a `PipeGuard` utility for safe, RAII-based execution of git commands.

**Tech Stack:** C++23, Google Test, POSIX (for process execution), Git CLI.

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `include/agentos/core/types.hpp` | **Modify** | Add worktree-related error codes |
| `include/agentos/worktree/types.hpp` | **Create** | Define `IsolationMode`, `WorktreeState`, `WorktreeInfo` |
| `include/agentos/worktree/worktree_manager.hpp` | **Create** | `WorktreeManager` class definition |
| `src/worktree/worktree_manager.cpp` | **Create** | `WorktreeManager` implementation & `PipeGuard` utility |
| `tests/test_worktree_manager.cpp` | **Create** | Unit tests for worktree CRUD and recovery |
| `CMakeLists.txt` | **Modify** | Add new source and test files |

---

## Task 1: Core Types & Error Codes

**Files:**
- Modify: `include/agentos/core/types.hpp`
- Create: `include/agentos/worktree/types.hpp`

- [ ] **Step 1: Add new error codes**

In `include/agentos/core/types.hpp`, add the following to `ErrorCode` enum:
```cpp
WorktreeError,
WorktreeCapacityFull,
ProcessSpawnFailed,
ProcessDied,
HandshakeFailed,
```

- [ ] **Step 2: Create worktree types**

Create `include/agentos/worktree/types.hpp`:
```cpp
#pragma once
#include <string>
#include <filesystem>
#include <optional>
#include <chrono>
#include <agentos/core/types.hpp>

namespace agentos::worktree {

enum class IsolationMode { Thread, Worktree };
enum class WorktreeState { Creating, Active, Merging, Removing, Failed };

struct WorktreeInfo {
    std::string name;
    std::string branch;
    std::filesystem::path path;
    WorktreeState state{WorktreeState::Creating};
    AgentId owner_agent{0};
    std::optional<int> pid; // placeholder for Phase 3
    TimePoint created_at{Clock::now()};
    bool has_changes{false};
};

} // namespace agentos::worktree
```

- [ ] **Step 3: Commit Task 1**
```bash
git add include/agentos/core/types.hpp include/agentos/worktree/types.hpp
git commit -m "feat(worktree): define core worktree types and error codes"
```

---

## Task 2: WorktreeManager Interface & Skeleton

**Files:**
- Create: `include/agentos/worktree/worktree_manager.hpp`
- Create: `src/worktree/worktree_manager.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create WorktreeManager header**

Create `include/agentos/worktree/worktree_manager.hpp`:
```cpp
#pragma once
#include <agentos/worktree/types.hpp>
#include <agentos/core/types.hpp>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace agentos::worktree {

struct WorktreeConfig {
    std::filesystem::path repo_root;
    std::filesystem::path worktree_base;
    uint32_t max_concurrent{10};
    bool auto_cleanup{true};
};

class WorktreeManager {
public:
    explicit WorktreeManager(WorktreeConfig cfg);

    Result<WorktreeInfo> create(std::string name, std::optional<std::string> base_branch = {});
    Result<void> remove(const std::string& name, bool force = false);
    Result<std::vector<WorktreeInfo>> list() const;
    Result<WorktreeInfo> get(const std::string& name) const;

    Result<bool> has_changes(const std::string& name) const;
    Result<void> recover();

    bool at_capacity() const;
    uint32_t active_count() const;

private:
    WorktreeConfig cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, WorktreeInfo> worktrees_;

    Result<std::string> exec_git(const std::vector<std::string>& args) const;
};

} // namespace agentos::worktree
```

- [ ] **Step 2: Create implementation skeleton with PipeGuard**

Create `src/worktree/worktree_manager.cpp`:
```cpp
#include <agentos/worktree/worktree_manager.hpp>
#include <agentos/core/logger.hpp>
#include <fmt/format.h>
#include <cstdio>
#include <array>
#include <memory>
#include <stdexcept>

namespace agentos::worktree {

// Internal helper for command execution
class PipeGuard {
public:
    explicit PipeGuard(const std::string& cmd) : pipe_(popen(cmd.c_str(), "r")) {
        if (!pipe_) throw std::runtime_error("popen() failed");
    }
    ~PipeGuard() { if (pipe_) pclose(pipe_); }
    
    std::string read_all() {
        std::string result;
        std::array<char, 128> buffer;
        while (fgets(buffer.data(), buffer.size(), pipe_) != nullptr) {
            result += buffer.data();
        }
        return result;
    }
private:
    FILE* pipe_;
};

WorktreeManager::WorktreeManager(WorktreeConfig cfg) : cfg_(std::move(cfg)) {
    if (!std::filesystem::exists(cfg_.worktree_base)) {
        std::filesystem::create_directories(cfg_.worktree_base);
    }
}

Result<std::string> WorktreeManager::exec_git(const std::vector<std::string>& args) const {
    std::string cmd = "git";
    for (const auto& arg : args) cmd += " " + arg;
    cmd += " 2>&1";
    
    try {
        PipeGuard pg(cmd);
        return pg.read_all();
    } catch (const std::exception& e) {
        return make_error(ErrorCode::WorktreeError, e.what());
    }
}

// Stubs for now
Result<WorktreeInfo> WorktreeManager::create(std::string, std::optional<std::string>) { 
    return make_error(ErrorCode::NotImplemented); 
}
Result<void> WorktreeManager::remove(const std::string&, bool) { 
    return make_error(ErrorCode::NotImplemented); 
}
Result<std::vector<WorktreeInfo>> WorktreeManager::list() const { 
    return std::vector<WorktreeInfo>{}; 
}
Result<WorktreeInfo> WorktreeManager::get(const std::string&) const { 
    return make_error(ErrorCode::NotFound); 
}
Result<bool> WorktreeManager::has_changes(const std::string&) const { return false; }
Result<void> WorktreeManager::recover() { return {}; }
bool WorktreeManager::at_capacity() const { return false; }
uint32_t WorktreeManager::active_count() const { return 0; }

} // namespace agentos::worktree
```

- [ ] **Step 3: Update CMakeLists.txt**

Add `src/worktree/worktree_manager.cpp` to `agentos` library sources and add a new test target.

- [ ] **Step 4: Commit Task 2**
```bash
git add include/agentos/worktree/worktree_manager.hpp src/worktree/worktree_manager.cpp CMakeLists.txt
git commit -m "feat(worktree): add WorktreeManager skeleton and PipeGuard utility"
```

---

## Task 3: Implement Worktree CRUD (create/remove)

**Files:**
- Modify: `src/worktree/worktree_manager.cpp`
- Create: `tests/test_worktree_manager.cpp`

- [ ] **Step 1: Write failing test for create()**

Create `tests/test_worktree_manager.cpp`:
```cpp
#include <agentos/worktree/worktree_manager.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos::worktree;

class WorktreeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo_root = std::filesystem::current_path();
        worktree_base = repo_root / "build" / "test_worktrees";
        if (std::filesystem::exists(worktree_base)) {
            std::filesystem::remove_all(worktree_base);
        }
    }
    std::filesystem::path repo_root;
    std::filesystem::path worktree_base;
};

TEST_F(WorktreeManagerTest, CreateWorktree) {
    WorktreeManager mgr({repo_root, worktree_base, 2});
    auto res = mgr.create("test-wt");
    ASSERT_TRUE(res.has_value()) << res.error().message;
    EXPECT_EQ(res->name, "test-wt");
    EXPECT_TRUE(std::filesystem::exists(res->path));
    EXPECT_TRUE(std::filesystem::exists(res->path / ".git"));
}
```

- [ ] **Step 2: Implement create()**

In `src/worktree/worktree_manager.cpp`:
```cpp
Result<WorktreeInfo> WorktreeManager::create(std::string name, std::optional<std::string> base_branch) {
    std::lock_guard lk(mu_);
    if (worktrees_.size() >= cfg_.max_concurrent) {
        return make_error(ErrorCode::WorktreeCapacityFull, "Max worktrees reached");
    }

    std::filesystem::path wt_path = cfg_.worktree_base / name;
    std::string branch_name = "agent/" + name;
    
    std::vector<std::string> args = {"worktree", "add", wt_path.string(), "-b", branch_name};
    if (base_branch) args.push_back(*base_branch);

    auto git_res = exec_git(args);
    if (!git_res) return git_res.error();

    WorktreeInfo info;
    info.name = name;
    info.branch = branch_name;
    info.path = wt_path;
    info.state = WorktreeState::Active;
    
    worktrees_[name] = info;
    return info;
}
```

- [ ] **Step 3: Run test and verify it passes**

- [ ] **Step 4: Implement remove() and tests**

- [ ] **Step 5: Commit Task 3**

---

## Task 4: Recovery & Lifecycle

**Files:**
- Modify: `src/worktree/worktree_manager.cpp`
- Modify: `tests/test_worktree_manager.cpp`

- [ ] **Step 1: Implement recover()**
Reconcile memory state by running `git worktree list --porcelain`.

- [ ] **Step 2: Implement has_changes()**
Check `git status --short` in the worktree directory.

- [ ] **Step 3: Add recovery tests**

- [ ] **Step 4: Commit Task 4**

---

## Task 5: Final Verification

- [ ] **Step 1: Run all tests**
- [ ] **Step 2: Final code review**
- [ ] **Step 3: Commit Task 5**
