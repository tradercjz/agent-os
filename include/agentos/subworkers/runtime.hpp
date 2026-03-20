#pragma once
// ============================================================
// AgentOS :: Subworker Runtime Types
// Phase 1 public API surface for worktree-backed subworkers
// ============================================================
#include <agentos/agent.hpp>
#include <optional>

namespace agentos {

enum class SubworkerStatus {
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};

struct WorkerTemplate {
    std::string name;
    std::string description;
    AgentConfig config;
    worktree::IsolationMode isolation{worktree::IsolationMode::Worktree};
    Duration timeout{std::chrono::seconds(300)};
    bool preserve_worktree_on_failure{true};
    bool allow_parallel{false};
};

struct SubworkerRunOptions {
    std::optional<Duration> timeout;
    std::optional<bool> preserve_worktree_on_failure;
    std::optional<std::filesystem::path> preferred_worktree_base;
    std::string metadata_json{"{}"};
};

struct SubworkerResult {
    std::string run_id;
    std::string worker_name;
    std::string task;
    SubworkerStatus status{SubworkerStatus::Failed};
    std::string summary;
    std::string output;
    std::string error;
    std::filesystem::path worktree_path;
    std::chrono::milliseconds elapsed{0};
};

struct SubworkerRunRecord {
    AgentId supervisor_id{0};
    std::string run_id;
    std::string worker_name;
    std::string task;
    SubworkerStatus status{SubworkerStatus::Failed};
    std::filesystem::path worktree_path;
    std::chrono::milliseconds elapsed{0};
    TimePoint started_at{Clock::now()};
    TimePoint finished_at{Clock::now()};
    bool output_truncated{false};
    std::string summary;
    std::string output;
    std::string error;
};

} // namespace agentos
