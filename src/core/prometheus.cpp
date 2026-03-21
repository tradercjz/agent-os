#include <agentos/core/prometheus.hpp>
#include <agentos/agent.hpp>

namespace agentos {

void PrometheusFormatter::write_counter(std::string& out, const std::string& name,
                                        const std::string& help, uint64_t value) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " counter\n";
    out += name + " " + std::to_string(value) + "\n";
}

void PrometheusFormatter::write_gauge(std::string& out, const std::string& name,
                                      const std::string& help, int64_t value) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " gauge\n";
    out += name + " " + std::to_string(value) + "\n";
}

void PrometheusFormatter::write_gauge_labeled(std::string& out, const std::string& name,
                                              const std::string& help,
                                              const std::string& label_key,
                                              const std::string& label_val, int64_t value,
                                              bool write_header) {
    if (write_header) {
        out += "# HELP " + name + " " + help + "\n";
        out += "# TYPE " + name + " gauge\n";
    }
    out += name + "{" + label_key + "=\"" + label_val + "\"} " + std::to_string(value) + "\n";
}

std::string PrometheusFormatter::format() const {
    std::string out;
    out.reserve(2048);

    // Kernel metrics
    const auto& km = os_.kernel().metrics();
    write_counter(out, "agentos_kernel_requests_total", "Total LLM inference requests",
                  km.total_requests.load(std::memory_order_relaxed));
    write_counter(out, "agentos_kernel_tokens_total", "Total tokens consumed",
                  km.total_tokens.load(std::memory_order_relaxed));
    write_counter(out, "agentos_kernel_errors_total", "Total LLM errors",
                  km.errors.load(std::memory_order_relaxed));
    write_counter(out, "agentos_kernel_retries_total", "Total LLM retries",
                  km.retries.load(std::memory_order_relaxed));
    write_counter(out, "agentos_kernel_rate_limit_hits_total", "Total rate limit hits",
                  km.rate_limit_hits.load(std::memory_order_relaxed));

    // Scheduler metrics
    const auto& sm = os_.scheduler().metrics();
    write_counter(out, "agentos_scheduler_tasks_submitted_total", "Total tasks submitted",
                  sm.tasks_submitted.load(std::memory_order_relaxed));
    write_counter(out, "agentos_scheduler_tasks_completed_total", "Total tasks completed",
                  sm.tasks_completed.load(std::memory_order_relaxed));
    write_counter(out, "agentos_scheduler_tasks_failed_total", "Total tasks failed",
                  sm.tasks_failed.load(std::memory_order_relaxed));
    write_gauge(out, "agentos_scheduler_active_tasks", "Currently active tasks",
                static_cast<int64_t>(os_.scheduler().active_task_count()));

    // Memory metrics (per tier)
    write_gauge_labeled(out, "agentos_memory_entries", "Memory entries by tier",
                       "tier", "working",
                       static_cast<int64_t>(os_.memory().working().size()),
                       true);
    write_gauge_labeled(out, "agentos_memory_entries", "Memory entries by tier",
                       "tier", "short_term",
                       static_cast<int64_t>(os_.memory().short_term().size()),
                       false);
    write_gauge_labeled(out, "agentos_memory_entries", "Memory entries by tier",
                       "tier", "long_term",
                       static_cast<int64_t>(os_.memory().long_term().size()),
                       false);

    // Bus metrics
    write_counter(out, "agentos_bus_dropped_total", "Total bus messages dropped",
                  os_.bus().total_dropped_messages());

    return out;
}

} // namespace agentos
