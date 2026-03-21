#pragma once
// ============================================================
// AgentOS :: Core :: Per-Agent Rate Limiter
// Each agent gets its own TPM bucket for independent throttling
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace agentos {

struct AgentQuota {
    uint32_t tpm{0};  // 0 = use global limit
};

// Per-agent rate limiting: each agent gets its own TPM bucket
class AgentRateLimiter : private NonCopyable {
public:
    // Set quota for a specific agent
    void set_quota(AgentId id, AgentQuota quota);

    // Remove quota for an agent (falls back to global)
    void remove_quota(AgentId id);

    // Get rate limiter for an agent (nullptr if no custom quota)
    kernel::TokenBucketRateLimiter* get_limiter(AgentId id);

    // Check if agent has a custom quota
    bool has_quota(AgentId id) const;

    // Get all quotas
    std::unordered_map<AgentId, AgentQuota> quotas() const;

    size_t agent_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<AgentId, AgentQuota> quotas_;
    std::unordered_map<AgentId, std::unique_ptr<kernel::TokenBucketRateLimiter>> limiters_;
};

} // namespace agentos
