#pragma once
// ============================================================
// AgentOS :: Memory Consolidator
// Ebbinghaus forgetting curve + STM-to-LTM migration
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace agentos::memory {

class MemorySystem; // forward declare

struct ConsolidatorConfig {
    Duration periodic_interval{120'000};  // 120s background scan
    size_t stm_count_threshold{100};      // Trigger consolidation when STM exceeds this
    float consolidate_threshold{0.5f};    // Migrate to LTM if strength >= this
    float forget_threshold{0.1f};         // Delete from STM if strength < this
    float decay_rate{0.3f};              // Lambda per hour (Ebbinghaus)
};

struct ConsolidationResult {
    size_t scanned{0};
    size_t consolidated{0};
    size_t forgotten{0};
    size_t retained{0};
};

// Compute memory strength using Ebbinghaus forgetting curve
// S(t) = S0 * e^(-lambda * t_hours)
// S0 = min(1.0, importance * (1 + 0.1 * access_count))
inline float compute_memory_strength(float importance, uint32_t access_count,
                                      TimePoint last_access, TimePoint now_tp,
                                      float decay_rate) {
    double hours = std::chrono::duration<double, std::ratio<3600>>(now_tp - last_access).count();
    if (hours < 0.0) hours = 0.0;
    double base = static_cast<double>(importance) * (1.0 + 0.1 * access_count);
    base = std::min(base, 1.0);
    return static_cast<float>(base * std::exp(-decay_rate * hours));
}

class MemoryConsolidator : private NonCopyable {
public:
    explicit MemoryConsolidator(MemorySystem& memory, ConsolidatorConfig cfg = {});
    ~MemoryConsolidator();

    void start();
    void stop();

    // Event-driven triggers
    void on_agent_run_complete(AgentId agent_id);
    void on_agent_destroyed(AgentId agent_id);

    // Register/unregister agents for periodic consolidation
    void register_agent(AgentId agent_id);
    void unregister_agent(AgentId agent_id);

    // Manual trigger (for testing)
    ConsolidationResult consolidate_now(AgentId agent_id);

    // Config access
    const ConsolidatorConfig& config() const noexcept { return config_; }

private:
    void background_loop(std::stop_token st);
    ConsolidationResult consolidate_agent(AgentId agent_id);

    MemorySystem& memory_;
    ConsolidatorConfig config_;
    std::jthread background_thread_;
    std::mutex mu_;
    std::condition_variable_any cv_;  // For interruptible wait with stop_token
    std::unordered_set<AgentId> registered_agents_;
};

} // namespace agentos::memory
