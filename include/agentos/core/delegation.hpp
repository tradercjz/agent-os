#pragma once
// ============================================================
// AgentOS :: Core :: Delegation
// Agent-to-agent task delegation with result forwarding
// ============================================================
#include <agentos/core/types.hpp>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace agentos {

class AgentOS;
class Agent;

// Delegation request — one agent asks another to perform a subtask
struct DelegationRequest {
    AgentId from{0};
    AgentId to{0};
    std::string task;           // natural language task description
    std::string context;        // additional context/data
    Duration timeout{Duration{30000}};  // 30s default
};

struct DelegationResult {
    bool success{false};
    std::string output;
    std::string error;
    TokenCount tokens_used{0};
    Duration elapsed{Duration{0}};
};

// Manages agent-to-agent delegation
class DelegationManager : private NonCopyable {
public:
    explicit DelegationManager(AgentOS& os) : os_(os) {}

    // Synchronous delegation: caller blocks until delegate finishes
    [[nodiscard]] Result<DelegationResult> delegate(const DelegationRequest& req);

    // Async delegation: returns future
    std::future<Result<DelegationResult>> delegate_async(DelegationRequest req);

    // Broadcast task to multiple agents, collect results
    std::vector<Result<DelegationResult>> fan_out(
        AgentId from,
        const std::vector<AgentId>& targets,
        const std::string& task,
        Duration timeout = Duration{30000});

private:
    AgentOS& os_;
};

} // namespace agentos
