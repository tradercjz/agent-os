#include <agentos/tracing.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace agentos::tracing {

namespace fs = std::filesystem;

// ── Helper: milliseconds since epoch for a steady_clock TimePoint ──
// steady_clock has no wall-clock epoch, so we convert relative to a
// reference captured once at process start.
static int64_t to_epoch_ms(TimePoint tp) {
    static const auto steady_ref = Clock::now();
    static const auto system_ref =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    auto offset = std::chrono::duration_cast<std::chrono::milliseconds>(tp - steady_ref).count();
    return system_ref + offset;
}

// ── Trace::to_json ─────────────────────────────────────────────────
std::string Trace::to_json() const {
    Json j;
    j["trace_id"]      = trace_id;
    j["agent_id"]      = agent_id;
    j["goal"]          = goal;
    j["start_time_ms"] = to_epoch_ms(start_time);
    j["end_time_ms"]   = to_epoch_ms(end_time);
    j["duration_ms"]   = duration().count();
    j["total_tokens"]  = total_tokens;
    j["success"]       = success;

    auto& j_spans = j["spans"];
    j_spans = Json::array();
    for (const auto& sp : spans) {
        Json js;
        js["span_id"]        = sp.id;
        js["parent_span_id"] = sp.parent_id;
        js["operation"]      = sp.operation;
        js["start_time_ms"]  = to_epoch_ms(sp.start_time);
        js["duration_ms"]    = sp.duration().count();
        js["tokens_used"]    = sp.tokens_used;
        js["input"]          = sp.input;
        js["output"]         = sp.output;
        js["success"]        = sp.success;
        if (!sp.error.empty()) {
            js["error"] = sp.error;
        }
        Json jt = Json::object();
        for (const auto& [k, v] : sp.tags) {
            jt[k] = v;
        }
        js["tags"] = std::move(jt);
        j_spans.push_back(std::move(js));
    }
    return j.dump(2);
}

// ── Tracer constructor ─────────────────────────────────────────────
Tracer::Tracer(TracerConfig cfg) : config_(std::move(cfg)) {}

// ── Trace lifecycle ────────────────────────────────────────────────
std::string Tracer::begin_trace(AgentId agent_id, const std::string& goal) {
    if (!config_.enabled) return "";

    std::lock_guard lk(mu_);
    auto id = fmt::format("trace_{}_{}", agent_id, trace_counter_++);

    Trace t;
    t.trace_id   = id;
    t.agent_id   = agent_id;
    t.goal       = goal;
    t.start_time = now();

    traces_.push_back(std::move(t));
    trace_index_[id] = traces_.size() - 1;
    evict_if_needed();

    LOG_DEBUG(fmt::format("Tracer: begin trace {}", id));
    return id;
}

void Tracer::end_trace(const std::string& trace_id, bool success) {
    if (!config_.enabled) return;

    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return;

    auto& trace = traces_[it->second];
    trace.end_time = now();
    trace.success  = success;

    // Sum tokens from all spans
    TokenCount total = 0;
    for (const auto& sp : trace.spans) {
        total += sp.tokens_used;
    }
    trace.total_tokens = total;

    LOG_DEBUG(fmt::format("Tracer: end trace {} tokens={} success={}",
                          trace_id, total, success));

    // Auto-export if configured
    if (!config_.export_dir.empty()) {
        auto json_str = trace.to_json();
        // Write outside lock would be ideal, but trace ref would be invalidated.
        // Export dir writes are fast for small JSON, so acceptable here.
        auto dir = fs::path(config_.export_dir);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!ec) {
            auto tmp_path  = dir / (trace_id + ".json.tmp");
            auto final_path = dir / (trace_id + ".json");
            std::ofstream ofs(tmp_path);
            if (ofs) {
                ofs << json_str;
                ofs.flush();
                if (ofs.good()) {
                    ofs.close();
                    fs::rename(tmp_path, final_path, ec);
                } else {
                    ofs.close();
                    fs::remove(tmp_path, ec);
                }
            }
        }
    }
}

// ── Span lifecycle ─────────────────────────────────────────────────
std::string Tracer::begin_span(const std::string& trace_id,
                               const std::string& parent_span_id,
                               const std::string& operation,
                               const std::string& input) {
    if (!config_.enabled) return "";

    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return "";

    auto span_id = fmt::format("span_{}", span_counter_++);

    Span sp;
    sp.id         = span_id;
    sp.parent_id  = parent_span_id;
    sp.operation  = operation;
    sp.start_time = now();
    sp.input      = truncate(input);

    traces_[it->second].spans.push_back(std::move(sp));
    return span_id;
}

void Tracer::end_span(const std::string& trace_id,
                      const std::string& span_id,
                      const std::string& output,
                      TokenCount tokens,
                      bool success,
                      const std::string& error) {
    if (!config_.enabled) return;

    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return;

    auto& spans = traces_[it->second].spans;
    for (auto& sp : spans) {
        if (sp.id == span_id) {
            sp.end_time   = now();
            sp.output     = truncate(output);
            sp.tokens_used = tokens;
            sp.success    = success;
            sp.error      = error;
            return;
        }
    }
}

// ── Query ──────────────────────────────────────────────────────────
std::optional<Trace> Tracer::get_trace(const std::string& trace_id) const {
    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return std::nullopt;
    return traces_[it->second];
}

std::vector<Trace> Tracer::recent_traces(size_t count) const {
    std::lock_guard lk(mu_);
    size_t n = std::min(count, traces_.size());
    // Return the last N traces (most recent are at the back)
    return {traces_.end() - static_cast<std::ptrdiff_t>(n), traces_.end()};
}

size_t Tracer::trace_count() const {
    std::lock_guard lk(mu_);
    return traces_.size();
}

// ── Export ──────────────────────────────────────────────────────────
std::string Tracer::export_json(const std::string& trace_id) const {
    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return "{}";
    return traces_[it->second].to_json();
}

// ── Private helpers ────────────────────────────────────────────────
std::string Tracer::truncate(const std::string& s) const {
    if (s.size() <= config_.max_input_length) return s;
    return s.substr(0, config_.max_input_length) + "...[truncated]";
}

void Tracer::evict_if_needed() {
    // Caller must hold mu_
    while (traces_.size() > config_.max_traces) {
        auto& oldest_id = traces_.front().trace_id;
        trace_index_.erase(oldest_id);
        traces_.pop_front();
        // Rebuild index — deque indices shifted by 1
        trace_index_.clear();
        for (size_t i = 0; i < traces_.size(); ++i) {
            trace_index_[traces_[i].trace_id] = i;
        }
    }
}

// ── TraceContext thread_local definitions ──────────────────────
thread_local std::string TraceContext::trace_id_;
thread_local std::string TraceContext::span_id_;

// ── OTLP JSON export ──────────────────────────────────────────
std::string Tracer::export_otlp_json(const std::string& trace_id) const {
    std::lock_guard lk(mu_);
    auto it = trace_index_.find(trace_id);
    if (it == trace_index_.end()) return "{}";
    const auto& trace = traces_[it->second];

    using Json = nlohmann::json;
    Json scope_spans = Json::array();

    for (const auto& span : trace.spans) {
        Json j;
        j["traceId"] = trace.trace_id;
        j["spanId"] = span.id;
        if (!span.parent_id.empty()) j["parentSpanId"] = span.parent_id;
        j["name"] = span.operation;

        auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            span.start_time.time_since_epoch()).count();
        auto end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            span.end_time.time_since_epoch()).count();
        j["startTimeUnixNano"] = std::to_string(start_ns);
        j["endTimeUnixNano"] = std::to_string(end_ns);

        Json attrs = Json::array();
        for (const auto& [k, v] : span.tags) {
            Json attr;
            attr["key"] = k;
            attr["value"]["stringValue"] = v;
            attrs.push_back(attr);
        }
        if (span.tokens_used > 0) {
            Json attr;
            attr["key"] = "tokens_used";
            attr["value"]["intValue"] = std::to_string(span.tokens_used);
            attrs.push_back(attr);
        }
        if (!span.error.empty()) {
            Json attr;
            attr["key"] = "error.message";
            attr["value"]["stringValue"] = span.error;
            attrs.push_back(attr);
        }
        j["attributes"] = attrs;

        Json status;
        status["code"] = span.success ? 1 : 2;
        j["status"] = status;

        scope_spans.push_back(j);
    }

    Json result;
    result["resourceSpans"] = Json::array({
        {{"scopeSpans", Json::array({
            {{"spans", scope_spans}}
        })}}
    });

    return result.dump(2);
}

} // namespace agentos::tracing
