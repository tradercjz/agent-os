# P2 Observability Design Spec

**Date:** 2026-03-21
**Batch:** P2 — Observability
**Scope:** Enhanced Tracing (ScopedSpan + TraceContext + OTLP export), Prometheus Metrics Formatting

---

## 1. Enhanced Tracing

### Current State

`tracing::Tracer` exists with `Span`, `Trace`, `begin_span()`, `end_span()`, JSON export. Missing: RAII span management, thread-local context propagation, OTLP-compatible export.

### Design

#### 1.1 ScopedSpan RAII

```cpp
class ScopedSpan {
public:
    ScopedSpan(Tracer& tracer, const std::string& trace_id,
               const std::string& parent_span_id, const std::string& operation,
               const std::string& input = "");
    ~ScopedSpan();  // calls end_span
    void set_output(const std::string& output);
    void set_tokens(TokenCount tokens);
    void set_error(const std::string& error);
    const std::string& span_id() const;
private:
    Tracer& tracer_;
    std::string trace_id_;
    std::string span_id_;
    std::string output_;
    TokenCount tokens_{0};
    bool success_{true};
    std::string error_;
};
```

#### 1.2 TraceContext (Thread-Local)

```cpp
class TraceContext {
public:
    static void set_current(const std::string& trace_id, const std::string& span_id);
    static std::string current_trace_id();
    static std::string current_span_id();
    static void clear();
private:
    static thread_local std::string trace_id_;
    static thread_local std::string span_id_;
};
```

`ScopedSpan` constructor sets `TraceContext`, destructor restores parent. This lets nested calls auto-propagate trace/span IDs.

#### 1.3 OTLP JSON Export

Add to `Tracer`:
```cpp
    std::string export_otlp_json(const std::string& trace_id) const;
```

Formats trace as OTLP JSON (compatible with OpenTelemetry Collector HTTP endpoint). Structure follows `ExportTraceServiceRequest` schema.

#### 1.4 Files

| File | Action |
|------|--------|
| `include/agentos/tracing.hpp` | **Modify** — add ScopedSpan, TraceContext, export_otlp_json |
| `src/tracing/tracer.cpp` | **Modify** — implement ScopedSpan, TraceContext, OTLP export |
| `tests/test_tracing.cpp` | **Modify** — add ScopedSpan, TraceContext, OTLP tests |

---

## 2. Prometheus Metrics Formatting

### Current State

`KernelMetrics` (5 atomic counters) and `SchedulerMetrics` (7 atomic counters) exist but have no export format.

### Design

```cpp
// include/agentos/core/prometheus.hpp
namespace agentos {

class PrometheusFormatter {
public:
    explicit PrometheusFormatter(AgentOS& os);
    std::string format() const;
};

} // namespace agentos
```

`format()` returns Prometheus text exposition format:
```
# HELP agentos_kernel_requests_total Total LLM inference requests
# TYPE agentos_kernel_requests_total counter
agentos_kernel_requests_total 42
...
```

Metrics collected from: `kernel().metrics()`, `scheduler().metrics()`, `scheduler().active_task_count()`, `memory().working().size()`, `memory().short_term().size()`, `memory().long_term().size()`, `bus().total_dropped_messages()`.

#### Integration

- `AgentOS::metrics_prometheus()` → delegates to `PrometheusFormatter`
- MCP method `metrics/prometheus` → returns formatted string

#### Files

| File | Action |
|------|--------|
| `include/agentos/core/prometheus.hpp` | **New** — PrometheusFormatter |
| `include/agentos/agent.hpp` | **Modify** — add metrics_prometheus() |
| `include/agentos/mcp/mcp_server.hpp` | **Modify** — add metrics/prometheus method |
| `tests/test_prometheus.cpp` | **New** — format output tests |
