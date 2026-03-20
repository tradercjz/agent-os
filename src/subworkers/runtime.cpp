#include <agentos/subworkers/runtime.hpp>

#include <atomic>
#include <cctype>

namespace agentos {
namespace {

std::atomic<uint64_t> g_subworker_run_seq{1};

SubworkerStatus map_error_to_status(const Error& error) {
    switch (error.code) {
    case ErrorCode::Timeout:
        return SubworkerStatus::TimedOut;
    default:
        return SubworkerStatus::Failed;
    }
}

bool should_preserve_failed_worktree(const WorkerTemplate& tpl,
                                     const SubworkerRunOptions& opts) {
    return opts.preserve_worktree_on_failure.value_or(
        tpl.preserve_worktree_on_failure);
}

std::string sanitize_worktree_name(std::string value) {
    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '-' && ch != '_') {
            ch = '-';
        }
    }
    return value;
}

class InProcWorktreeExecutor final : public ISubworkerExecutor {
public:
    Result<SubworkerResult> run(AgentOS&,
                                AgentId,
                                const std::shared_ptr<Agent>& worker,
                                const WorkerTemplate&,
                                const std::string& task,
                                const std::filesystem::path& worktree_path,
                                const SubworkerRunOptions&) override {
        auto started_at = Clock::now();
        auto worker_run = worker->run(task);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started_at);

        SubworkerResult result;
        result.task = task;
        result.worktree_path = worktree_path;
        result.elapsed = elapsed;

        if (!worker_run) {
            result.status = map_error_to_status(worker_run.error());
            result.error = worker_run.error().message;
            result.summary = worker_run.error().message;
            return result;
        }

        result.status = SubworkerStatus::Succeeded;
        result.output = *worker_run;
        result.summary = result.output;
        return result;
    }
};

} // namespace

SubworkerRuntime::SubworkerRuntime(std::unique_ptr<ISubworkerExecutor> executor)
    : executor_(executor ? std::move(executor)
                         : std::make_unique<InProcWorktreeExecutor>()) {}

Result<SubworkerResult> SubworkerRuntime::run(AgentOS& os,
                                              AgentId supervisor_id,
                                              const WorkerTemplate& tpl,
                                              const std::string& task,
                                              const SubworkerRunOptions& opts) {
    const auto seq = g_subworker_run_seq.fetch_add(1, std::memory_order_relaxed);
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
    const std::string run_id =
        fmt::format("subworker-{}-{}-{}", supervisor_id, now_ns, seq);
    const std::string worker_name = tpl.name.empty() ? tpl.config.name : tpl.name;
    const std::string worktree_name =
        sanitize_worktree_name(fmt::format("{}-{}", worker_name, run_id));
    std::unique_ptr<worktree::WorktreeManager> preferred_mgr;
    worktree::WorktreeManager* worktree_mgr = &os.worktree_mgr();
    if (opts.preferred_worktree_base.has_value()) {
        preferred_mgr = std::make_unique<worktree::WorktreeManager>(
            worktree::WorktreeConfig{
                .repo_root = os.config().repo_root,
                .worktree_base = *opts.preferred_worktree_base,
                .max_concurrent = os.config().max_worktrees});
        worktree_mgr = preferred_mgr.get();
    }

    auto wt_res = worktree_mgr->create(worktree_name);
    if (!wt_res) {
        return make_unexpected(wt_res.error());
    }

    AgentConfig worker_cfg = tpl.config;
    if (worker_cfg.name.empty()) {
        worker_cfg.name = worker_name.empty() ? worktree_name : worker_name;
    }
    worker_cfg.isolation = worktree::IsolationMode::Thread;

    auto worker = os.create_agent<ReActAgent>(worker_cfg);
    worker->work_dir_ = wt_res->path;

    auto result = executor_->run(
        os, supervisor_id, worker, tpl, task, wt_res->path, opts);

    os.destroy_agent(worker->id());

    if (!result) {
        return make_unexpected(result.error());
    }

    result->run_id = run_id;
    result->worker_name = worker_name;
    result->task = task;
    result->worktree_path = wt_res->path;
    if (result->summary.empty()) {
        result->summary = result->status == SubworkerStatus::Succeeded
            ? result->output
            : result->error;
    }

    if (result->status != SubworkerStatus::Succeeded &&
        !should_preserve_failed_worktree(tpl, opts)) {
        (void)worktree_mgr->remove(worktree_name, /*force=*/true);
    }

    return result;
}

} // namespace agentos
