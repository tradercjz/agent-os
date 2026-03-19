#pragma once
// ============================================================
// AgentOS :: WorktreeManager
// Git worktree 池管理：创建、销毁、恢复
// ============================================================
#include <agentos/worktree/types.hpp>
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

    Result<WorktreeInfo> create(std::string name,
                                std::optional<std::string> base_branch = {});
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
