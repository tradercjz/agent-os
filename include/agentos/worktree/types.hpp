#pragma once
// ============================================================
// AgentOS :: Worktree Types
// Worktree 隔离相关的核心类型定义
// ============================================================
#include <agentos/core/types.hpp>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace agentos::worktree {

enum class IsolationMode { Thread, Worktree };
enum class WorktreeState { Creating, Active, Merging, Removing, Failed };

struct WorktreeInfo {
    std::string name;
    std::string branch;
    std::filesystem::path path;
    WorktreeState state{WorktreeState::Creating};
    AgentId owner_agent{0};
    std::optional<int> pid;  // placeholder for Phase 3
    TimePoint created_at{Clock::now()};
    bool has_changes{false};
};

} // namespace agentos::worktree
