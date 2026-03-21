// ============================================================
// AgentOS :: Core :: Per-Agent Rate Limiter — Implementation
// ============================================================
#include <agentos/core/agent_rate_limiter.hpp>

namespace agentos {

void AgentRateLimiter::set_quota(AgentId id, AgentQuota quota) {
    std::lock_guard lk(mu_);
    quotas_[id] = quota;
    if (quota.tpm > 0) {
        limiters_[id] = std::make_unique<kernel::TokenBucketRateLimiter>(quota.tpm);
    } else {
        limiters_.erase(id);
    }
}

void AgentRateLimiter::remove_quota(AgentId id) {
    std::lock_guard lk(mu_);
    quotas_.erase(id);
    limiters_.erase(id);
}

kernel::TokenBucketRateLimiter* AgentRateLimiter::get_limiter(AgentId id) {
    std::lock_guard lk(mu_);
    auto it = limiters_.find(id);
    return it != limiters_.end() ? it->second.get() : nullptr;
}

bool AgentRateLimiter::has_quota(AgentId id) const {
    std::lock_guard lk(mu_);
    return quotas_.contains(id);
}

std::unordered_map<AgentId, AgentQuota> AgentRateLimiter::quotas() const {
    std::lock_guard lk(mu_);
    return quotas_;
}

size_t AgentRateLimiter::agent_count() const {
    std::lock_guard lk(mu_);
    return quotas_.size();
}

} // namespace agentos
