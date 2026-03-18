// ============================================================
// AgentOS :: Memory Consolidator — Implementation
// ============================================================
#include <agentos/memory/consolidator.hpp>
#include <agentos/memory/memory.hpp>

namespace agentos::memory {

// ── Constructor / Destructor ────────────────────────────────

MemoryConsolidator::MemoryConsolidator(MemorySystem& memory, ConsolidatorConfig cfg)
    : memory_(memory), config_(std::move(cfg)) {}

MemoryConsolidator::~MemoryConsolidator() {
    stop();
}

// ── Lifecycle ───────────────────────────────────────────────

void MemoryConsolidator::start() {
    // Avoid double-start: if thread is already joinable, do nothing
    if (background_thread_.joinable()) return;

    background_thread_ = std::jthread([this](std::stop_token st) {
        background_loop(st);
    });
    LOG_INFO("MemoryConsolidator started");
}

void MemoryConsolidator::stop() {
    if (background_thread_.joinable()) {
        background_thread_.request_stop();
        cv_.notify_all();
        background_thread_.join();
        LOG_INFO("MemoryConsolidator stopped");
    }
}

// ── Agent Registration ──────────────────────────────────────

void MemoryConsolidator::register_agent(AgentId agent_id) {
    std::lock_guard lk(mu_);
    registered_agents_.insert(agent_id);
}

void MemoryConsolidator::unregister_agent(AgentId agent_id) {
    std::lock_guard lk(mu_);
    registered_agents_.erase(agent_id);
}

// ── Event-Driven Triggers ───────────────────────────────────

void MemoryConsolidator::on_agent_run_complete(AgentId agent_id) {
    auto result = consolidate_agent(agent_id);
    LOG_INFO(fmt::format("Consolidation on agent run complete (agent={}): "
                         "scanned={} consolidated={} forgotten={} retained={}",
                         agent_id, result.scanned, result.consolidated,
                         result.forgotten, result.retained));
}

void MemoryConsolidator::on_agent_destroyed(AgentId agent_id) {
    auto result = consolidate_agent(agent_id);
    LOG_INFO(fmt::format("Consolidation on agent destroyed (agent={}): "
                         "scanned={} consolidated={} forgotten={} retained={}",
                         agent_id, result.scanned, result.consolidated,
                         result.forgotten, result.retained));
    unregister_agent(agent_id);
}

// ── Manual Trigger ──────────────────────────────────────────

ConsolidationResult MemoryConsolidator::consolidate_now(AgentId agent_id) {
    return consolidate_agent(agent_id);
}

// ── Background Loop ─────────────────────────────────────────

void MemoryConsolidator::background_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, config_.periodic_interval,
                         [&st] { return st.stop_requested(); });
        }

        if (st.stop_requested()) break;

        // Snapshot registered agents under lock
        std::vector<AgentId> agents;
        {
            std::lock_guard lk(mu_);
            agents.assign(registered_agents_.begin(), registered_agents_.end());
        }

        for (auto agent_id : agents) {
            if (st.stop_requested()) break;
            auto result = consolidate_agent(agent_id);
            if (result.consolidated > 0 || result.forgotten > 0) {
                LOG_INFO(fmt::format("Periodic consolidation (agent={}): "
                                     "scanned={} consolidated={} forgotten={} retained={}",
                                     agent_id, result.scanned, result.consolidated,
                                     result.forgotten, result.retained));
            }
        }
    }
}

// ── Core Consolidation Logic ────────────────────────────────

ConsolidationResult MemoryConsolidator::consolidate_agent(AgentId agent_id) {
    ConsolidationResult result;
    auto now_tp = now();
    auto agent_id_str = std::to_string(agent_id);

    // 1. Get all STM entries
    auto all_entries = memory_.short_term().get_all();

    // 2. Filter by agent_id and process
    for (auto& entry : all_entries) {
        if (entry.agent_id != agent_id_str) continue;

        ++result.scanned;

        float strength = compute_memory_strength(
            entry.importance, entry.access_count,
            entry.accessed_at, now_tp, config_.decay_rate);

        if (strength >= config_.consolidate_threshold) {
            // Migrate to LTM
            auto write_result = memory_.long_term().write(entry);
            if (write_result) {
                (void)memory_.short_term().forget(entry.id);
                ++result.consolidated;
            } else {
                // Write failed — retain in STM
                LOG_WARN(fmt::format("Failed to consolidate entry {} to LTM: {}",
                                     entry.id, write_result.error().message));
                ++result.retained;
            }
        } else if (strength < config_.forget_threshold) {
            // Forget weak memories
            (void)memory_.short_term().forget(entry.id);
            ++result.forgotten;
        } else {
            // Retain in STM
            ++result.retained;
        }
    }

    return result;
}

} // namespace agentos::memory
