#pragma once
// ============================================================
// AgentOS :: Prometheus Metrics Formatter
// Formats subsystem metrics in Prometheus exposition format
// ============================================================
#include <cstdint>
#include <string>

namespace agentos {

class AgentOS; // forward declare

class PrometheusFormatter {
public:
    explicit PrometheusFormatter(const AgentOS& os) : os_(os) {}
    std::string format() const;

private:
    const AgentOS& os_;

    static void write_counter(std::string& out, const std::string& name,
                              const std::string& help, uint64_t value);
    static void write_gauge(std::string& out, const std::string& name,
                            const std::string& help, int64_t value);
    static void write_gauge_labeled(std::string& out, const std::string& name,
                                    const std::string& help,
                                    const std::string& label_key,
                                    const std::string& label_val, int64_t value,
                                    bool write_header);
};

} // namespace agentos
