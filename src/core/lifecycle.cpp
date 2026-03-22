#include <agentos/core/lifecycle.hpp>
#include <algorithm>

namespace agentos {

void LifecycleManager::on(LifecycleEvent event, LifecycleCallback cb) {
    std::lock_guard lk(mu_);
    listeners_[static_cast<int>(event)].push_back(std::move(cb));
}

void LifecycleManager::on_any(LifecycleCallback cb) {
    std::lock_guard lk(mu_);
    any_listeners_.push_back(std::move(cb));
}

void LifecycleManager::emit(const LifecycleEventData& data) {
    // Copy callbacks under lock, then invoke outside lock to avoid
    // deadlock if a callback calls back into the manager.
    std::vector<LifecycleCallback> to_call;
    {
        std::lock_guard lk(mu_);
        // Record in history
        history_.push_back(data);
        while (history_.size() > kMaxHistory) {
            history_.pop_front();
        }

        // Gather typed listeners
        auto it = listeners_.find(static_cast<int>(data.event));
        if (it != listeners_.end()) {
            to_call.insert(to_call.end(), it->second.begin(), it->second.end());
        }
        // Gather any-listeners
        to_call.insert(to_call.end(), any_listeners_.begin(), any_listeners_.end());
    }

    for (const auto& cb : to_call) {
        cb(data);
    }
}

std::vector<LifecycleEventData> LifecycleManager::recent(size_t count) const {
    std::lock_guard lk(mu_);
    size_t n = std::min(count, history_.size());
    // Return the last N events (most recent)
    return {history_.end() - static_cast<std::ptrdiff_t>(n), history_.end()};
}

size_t LifecycleManager::listener_count() const {
    std::lock_guard lk(mu_);
    size_t total = any_listeners_.size();
    for (const auto& [_, cbs] : listeners_) {
        total += cbs.size();
    }
    return total;
}

} // namespace agentos
