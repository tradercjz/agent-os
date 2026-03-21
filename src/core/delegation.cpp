// ============================================================
// AgentOS :: Core :: Delegation — Implementation
// ============================================================
#include <agentos/agent.hpp>
#include <agentos/core/delegation.hpp>
#include <future>

namespace agentos {

Result<DelegationResult> DelegationManager::delegate(const DelegationRequest& req) {
    auto agent = os_.find_agent(req.to);
    if (!agent) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("delegation: target agent {} not found", req.to));
    }

    DelegationResult result;
    auto start = now();

    // Prepare input: if context is provided, prepend it to the task
    std::string input = req.context.empty()
        ? req.task
        : fmt::format("[Context: {}]\n{}", req.context, req.task);

    auto run_result = agent->run(input);

    result.elapsed = std::chrono::duration_cast<Duration>(now() - start);

    if (run_result.has_value()) {
        result.success = true;
        result.output = std::move(*run_result);
    } else {
        result.success = false;
        result.error = run_result.error().message;
    }

    // Track tokens from the kernel metrics (approximate)
    result.tokens_used = 0;

    return result;
}

std::future<Result<DelegationResult>> DelegationManager::delegate_async(DelegationRequest req) {
    return std::async(std::launch::async, [this, r = std::move(req)]() {
        return delegate(r);
    });
}

std::vector<Result<DelegationResult>> DelegationManager::fan_out(
    AgentId from,
    const std::vector<AgentId>& targets,
    const std::string& task,
    Duration timeout) {

    std::vector<Result<DelegationResult>> results;
    results.reserve(targets.size());

    for (const auto& target_id : targets) {
        DelegationRequest req;
        req.from = from;
        req.to = target_id;
        req.task = task;
        req.timeout = timeout;

        results.push_back(delegate(req));
    }

    return results;
}

} // namespace agentos
