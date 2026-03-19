#include <agentos/worktree/worktree_manager.hpp>
#include <agentos/core/logger.hpp>
#include <array>
#include <cstdio>
#include <sstream>

namespace agentos::worktree {

// RAII wrapper for popen/pclose
class PipeGuard {
public:
    explicit PipeGuard(const std::string& cmd)
        : pipe_(popen(cmd.c_str(), "r")) {}

    ~PipeGuard() {
        if (pipe_) (void)pclose(pipe_);
    }

    PipeGuard(const PipeGuard&) = delete;
    PipeGuard& operator=(const PipeGuard&) = delete;

    explicit operator bool() const { return pipe_ != nullptr; }

    std::string read_all() {
        std::string result;
        std::array<char, 256> buffer{};
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe_)) {
            result += buffer.data();
        }
        return result;
    }

    int close() {
        if (!pipe_) return -1;
        int rc = pclose(pipe_);
        pipe_ = nullptr;
        return rc;
    }

private:
    FILE* pipe_;
};

// Shell-escape a single argument for safe command construction
static std::string shell_escape(const std::string& arg) {
    std::string escaped = "'";
    for (char c : arg) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += "'";
    return escaped;
}

WorktreeManager::WorktreeManager(WorktreeConfig cfg)
    : cfg_(std::move(cfg)) {
    if (!std::filesystem::exists(cfg_.worktree_base)) {
        std::filesystem::create_directories(cfg_.worktree_base);
    }
}

Result<std::string> WorktreeManager::exec_git(
    const std::vector<std::string>& args) const {
    std::string cmd = "git -C " + shell_escape(cfg_.repo_root.string());
    for (const auto& arg : args) {
        cmd += " " + shell_escape(arg);
    }
    cmd += " 2>&1";

    PipeGuard pg(cmd);
    if (!pg) {
        return make_error(ErrorCode::ProcessSpawnFailed,
                          "popen() failed for git command");
    }
    std::string output = pg.read_all();
    int rc = pg.close();

    // pclose returns the exit status shifted; extract it
    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
        return make_error(ErrorCode::WorktreeError,
                          fmt::format("git failed (exit {}): {}",
                                      WEXITSTATUS(rc), output));
    }
    return output;
}

Result<WorktreeInfo> WorktreeManager::create(
    std::string name, std::optional<std::string> base_branch) {
    std::lock_guard lk(mu_);

    if (worktrees_.size() >= cfg_.max_concurrent) {
        return make_error(ErrorCode::WorktreeCapacityFull,
                          "Max concurrent worktrees reached");
    }
    if (worktrees_.contains(name)) {
        return make_error(ErrorCode::AlreadyExists,
                          fmt::format("Worktree '{}' already exists", name));
    }

    std::filesystem::path wt_path = cfg_.worktree_base / name;
    std::string branch_name = "agent/" + name;

    std::vector<std::string> args = {
        "worktree", "add", wt_path.string(), "-b", branch_name};
    if (base_branch) {
        args.push_back(*base_branch);
    }

    auto git_res = exec_git(args);
    if (!git_res) return make_unexpected(git_res.error());

    WorktreeInfo info;
    info.name = name;
    info.branch = branch_name;
    info.path = wt_path;
    info.state = WorktreeState::Active;
    info.created_at = Clock::now();

    worktrees_[name] = info;
    LOG_INFO(fmt::format("Worktree '{}' created at {}", name, wt_path.string()));
    return info;
}

Result<void> WorktreeManager::remove(const std::string& name, bool force) {
    std::lock_guard lk(mu_);

    auto it = worktrees_.find(name);
    if (it == worktrees_.end()) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("Worktree '{}' not found", name));
    }

    it->second.state = WorktreeState::Removing;

    // Remove the git worktree
    std::vector<std::string> args = {"worktree", "remove"};
    if (force) args.push_back("--force");
    args.push_back(it->second.path.string());

    auto git_res = exec_git(args);
    if (!git_res) {
        it->second.state = WorktreeState::Failed;
        return make_unexpected(git_res.error());
    }

    // Delete the branch
    std::string branch = it->second.branch;
    worktrees_.erase(it);

    std::vector<std::string> branch_args = {"branch", "-D", branch};
    (void)exec_git(branch_args);  // best-effort branch cleanup

    LOG_INFO(fmt::format("Worktree '{}' removed", name));
    return {};
}

Result<std::vector<WorktreeInfo>> WorktreeManager::list() const {
    std::lock_guard lk(mu_);
    std::vector<WorktreeInfo> result;
    result.reserve(worktrees_.size());
    for (const auto& [_, info] : worktrees_) {
        result.push_back(info);
    }
    return result;
}

Result<WorktreeInfo> WorktreeManager::get(const std::string& name) const {
    std::lock_guard lk(mu_);
    auto it = worktrees_.find(name);
    if (it == worktrees_.end()) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("Worktree '{}' not found", name));
    }
    return it->second;
}

Result<bool> WorktreeManager::has_changes(const std::string& name) const {
    std::lock_guard lk(mu_);
    auto it = worktrees_.find(name);
    if (it == worktrees_.end()) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("Worktree '{}' not found", name));
    }

    // Run git status in the worktree directory
    std::string cmd = "git -C " + shell_escape(it->second.path.string())
                    + " status --short 2>&1";
    PipeGuard pg(cmd);
    if (!pg) {
        return make_error(ErrorCode::ProcessSpawnFailed,
                          "popen() failed for git status");
    }
    std::string output = pg.read_all();
    (void)pg.close();

    return !output.empty();
}

Result<void> WorktreeManager::recover() {
    // Parse `git worktree list --porcelain` to reconcile in-memory state
    auto git_res = exec_git({"worktree", "list", "--porcelain"});
    if (!git_res) return make_unexpected(git_res.error());

    const std::string& output = git_res.value();
    std::lock_guard lk(mu_);

    // Parse porcelain output: blocks separated by blank lines
    // Each block: "worktree <path>\nHEAD <sha>\nbranch refs/heads/<name>\n"
    std::istringstream stream(output);
    std::string line;
    std::string wt_path;
    std::string branch;

    while (std::getline(stream, line)) {
        if (line.starts_with("worktree ")) {
            wt_path = line.substr(9);
            branch.clear();
        } else if (line.starts_with("branch refs/heads/")) {
            branch = line.substr(18);
        } else if (line.empty() && !wt_path.empty()) {
            // End of a worktree block — check if it's in our managed base
            std::filesystem::path p(wt_path);
            auto base_str = cfg_.worktree_base.string();
            if (wt_path.starts_with(base_str) && branch.starts_with("agent/")) {
                std::string name = branch.substr(6);  // strip "agent/"
                if (!worktrees_.contains(name)) {
                    WorktreeInfo info;
                    info.name = name;
                    info.branch = branch;
                    info.path = p;
                    info.state = WorktreeState::Active;
                    worktrees_[name] = info;
                    LOG_INFO(fmt::format("Recovered worktree '{}'", name));
                }
            }
            wt_path.clear();
            branch.clear();
        }
    }

    // Handle last block (output may not end with blank line)
    if (!wt_path.empty() && branch.starts_with("agent/")) {
        std::filesystem::path p(wt_path);
        auto base_str = cfg_.worktree_base.string();
        if (wt_path.starts_with(base_str)) {
            std::string name = branch.substr(6);
            if (!worktrees_.contains(name)) {
                WorktreeInfo info;
                info.name = name;
                info.branch = branch;
                info.path = p;
                info.state = WorktreeState::Active;
                worktrees_[name] = info;
                LOG_INFO(fmt::format("Recovered worktree '{}'", name));
            }
        }
    }

    return {};
}

bool WorktreeManager::at_capacity() const {
    std::lock_guard lk(mu_);
    return worktrees_.size() >= cfg_.max_concurrent;
}

uint32_t WorktreeManager::active_count() const {
    std::lock_guard lk(mu_);
    return static_cast<uint32_t>(worktrees_.size());
}

} // namespace agentos::worktree
