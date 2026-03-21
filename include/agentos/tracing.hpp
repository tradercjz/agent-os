#pragma once
// ============================================================
// AgentOS :: Tracing Module
// Distributed-tracing-style spans for agent execution
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/core/logger.hpp>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::tracing {

struct Span {
    std::string id;
    std::string parent_id;      // "" for root span
    std::string operation;      // "think", "act", "tool:http_fetch", "plan", "replan", "synthesize"
    TimePoint start_time;
    TimePoint end_time;
    TokenCount tokens_used{0};
    std::string input;          // Truncated
    std::string output;         // Truncated
    bool success{true};
    std::string error;
    std::unordered_map<std::string, std::string> tags;

    Duration duration() const { return std::chrono::duration_cast<Duration>(end_time - start_time); }
};

struct Trace {
    std::string trace_id;
    AgentId agent_id;
    std::string goal;
    TimePoint start_time;
    TimePoint end_time;
    std::vector<Span> spans;
    TokenCount total_tokens{0};
    bool success{true};

    Duration duration() const { return std::chrono::duration_cast<Duration>(end_time - start_time); }
    std::string to_json() const; // Implemented in tracer.cpp
};

struct TracerConfig {
    size_t max_traces{100};
    size_t max_input_length{500};
    std::string export_dir{};     // Non-empty = auto-export JSON on trace end
    bool enabled{true};
};

class Tracer : private NonCopyable {
public:
    explicit Tracer(TracerConfig cfg = {});

    // Trace lifecycle
    std::string begin_trace(AgentId agent_id, const std::string& goal);
    void end_trace(const std::string& trace_id, bool success = true);

    // Span lifecycle
    std::string begin_span(const std::string& trace_id, const std::string& parent_span_id,
                           const std::string& operation, const std::string& input = "");
    void end_span(const std::string& trace_id, const std::string& span_id,
                  const std::string& output = "", TokenCount tokens = 0,
                  bool success = true, const std::string& error = "");

    // Query
    std::optional<Trace> get_trace(const std::string& trace_id) const;
    std::vector<Trace> recent_traces(size_t count = 10) const;
    size_t trace_count() const;

    // Export
    std::string export_json(const std::string& trace_id) const;
    std::string export_otlp_json(const std::string& trace_id) const;

    bool enabled() const noexcept { return config_.enabled; }
    const TracerConfig& config() const noexcept { return config_; }

private:
    std::string truncate(const std::string& s) const;
    void evict_if_needed();

    TracerConfig config_;
    mutable std::mutex mu_;
    std::deque<Trace> traces_;                                    // Ordered by time
    std::unordered_map<std::string, size_t> trace_index_;         // trace_id -> index in deque
    uint64_t span_counter_{0};
    uint64_t trace_counter_{0};
};

// ── RAII span — auto-ends on destruction ───────────────────────
class ScopedSpan {
public:
    ScopedSpan(Tracer& tracer, const std::string& trace_id,
               const std::string& parent_span_id, const std::string& operation,
               const std::string& input = "")
        : tracer_(tracer), trace_id_(trace_id) {
        span_id_ = tracer_.begin_span(trace_id, parent_span_id, operation, input);
    }

    ~ScopedSpan() {
        tracer_.end_span(trace_id_, span_id_, output_, tokens_, success_, error_);
    }

    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

    void set_output(const std::string& output) { output_ = output; }
    void set_tokens(TokenCount tokens) { tokens_ = tokens; }
    void set_error(const std::string& error) { success_ = false; error_ = error; }
    const std::string& span_id() const { return span_id_; }

private:
    Tracer& tracer_;
    std::string trace_id_;
    std::string span_id_;
    std::string output_;
    TokenCount tokens_{0};
    bool success_{true};
    std::string error_;
};

// ── Thread-local trace context for automatic span propagation ──
class TraceContext {
public:
    static void set_current(const std::string& trace_id, const std::string& span_id) {
        trace_id_ = trace_id;
        span_id_ = span_id;
    }
    static const std::string& current_trace_id() { return trace_id_; }
    static const std::string& current_span_id() { return span_id_; }
    static void clear() { trace_id_.clear(); span_id_.clear(); }

private:
    static thread_local std::string trace_id_;
    static thread_local std::string span_id_;
};

} // namespace agentos::tracing
