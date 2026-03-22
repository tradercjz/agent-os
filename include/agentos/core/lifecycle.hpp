#pragma once
// ============================================================
// AgentOS :: Core :: Lifecycle Events
// Agent lifecycle event system for observability
// ============================================================
#include <agentos/core/types.hpp>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

enum class LifecycleEvent {
    AgentCreated,
    AgentDestroyed,
    AgentError,
    AgentStarted,    // run() called
    AgentFinished,   // run() returned
};

struct LifecycleEventData {
    LifecycleEvent event{};
    AgentId agent_id{0};
    std::string agent_name;
    std::string detail;      // error message for AgentError, output for AgentFinished
    TimePoint timestamp{now()};
};

using LifecycleCallback = std::function<void(const LifecycleEventData&)>;

class LifecycleManager : private NonCopyable {
public:
    // Register callback for specific event type
    void on(LifecycleEvent event, LifecycleCallback cb);

    // Register callback for ALL events
    void on_any(LifecycleCallback cb);

    // Emit an event (called by AgentOS/Agent internally)
    void emit(const LifecycleEventData& data);

    // Get recent events (for debugging)
    std::vector<LifecycleEventData> recent(size_t count = 20) const;

    size_t listener_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<int, std::vector<LifecycleCallback>> listeners_;
    std::vector<LifecycleCallback> any_listeners_;
    std::deque<LifecycleEventData> history_;
    static constexpr size_t kMaxHistory = 100;
};

} // namespace agentos
