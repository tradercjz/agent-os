#pragma once
// ============================================================
// AgentOS :: SupervisorAgent — LLM-driven multi-agent delegation
// Workers registered per supervisor; presented to LLM as tools.
// ============================================================
#include <agentos/agent.hpp>
#include <agentos/core/types.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

// ── Depth guard ───────────────────────────────────────────────────────────────
// inline thread_local in a named namespace — anonymous namespace in a header
// gives each TU its own copy, breaking cross-TU depth tracking.
namespace detail {
inline thread_local size_t tl_supervisor_depth = 0;
} // namespace detail
using detail::tl_supervisor_depth;

struct ScopeDepthGuard {
    ScopeDepthGuard()  noexcept { ++tl_supervisor_depth; }
    ~ScopeDepthGuard() noexcept { --tl_supervisor_depth; }
};

// ── WorkerEntry ──────────────────────────────────────────────────────────────

struct WorkerEntry {
    std::shared_ptr<Agent> agent;
    std::string description;
    size_t max_calls{5};
};

// ── DelegationRecord ─────────────────────────────────────────────────────────

inline constexpr size_t kMaxDelegationResultChars = 4096;

struct DelegationRecord {
    AgentId supervisor_id{0};
    AgentId worker_id{0};
    std::string task;
    std::string result;           // capped at kMaxDelegationResultChars
    bool result_truncated{false};
    std::chrono::milliseconds elapsed{0};
    TimePoint timestamp{Clock::now()};
    bool success{true};
};

// ── SupervisorAgent ──────────────────────────────────────────────────────────

class SupervisorAgent : public AgentBase<SupervisorAgent> {
public:
    using AgentBase<SupervisorAgent>::AgentBase;

    SupervisorAgent& add_worker(std::shared_ptr<Agent> worker,
                                std::string description,
                                size_t max_calls = 5);

    Result<std::string> run(std::string user_input) override;

    std::vector<DelegationRecord> delegation_log() const;

    void set_max_depth(size_t depth) { max_depth_ = depth; }

    // Public for direct testing
    tools::ToolResult dispatch_worker(WorkerEntry& entry,
                                      const std::string& task,
                                      size_t& call_count);

private:
    std::string build_workers_tools_json() const;
    static std::string merge_tools_json(std::string_view global,
                                        std::string_view workers);

    std::unordered_map<std::string, WorkerEntry> workers_;
    std::vector<DelegationRecord> delegation_log_;
    size_t max_depth_{3};
    static constexpr int MAX_STEPS = 10;
    mutable std::mutex mu_;
};

// ── Inline implementations ───────────────────────────────────────────────────

inline SupervisorAgent& SupervisorAgent::add_worker(
        std::shared_ptr<Agent> worker, std::string description, size_t max_calls) {
    std::lock_guard lk(mu_);
    std::string key = worker->config().name;
    workers_[key] = WorkerEntry{std::move(worker), std::move(description), max_calls};
    return *this;
}

inline std::vector<DelegationRecord> SupervisorAgent::delegation_log() const {
    std::lock_guard lk(mu_);
    return delegation_log_;
}

inline std::string SupervisorAgent::build_workers_tools_json() const {
    if (workers_.empty()) return "[]";
    std::string arr = "[";
    bool first = true;
    for (const auto& [name, entry] : workers_) {
        if (!first) arr += ",";
        first = false;
        arr += fmt::format(
            R"({{"type":"function","function":{{"name":"{}","description":"{}","parameters":{{"type":"object","properties":{{"task":{{"type":"string","description":"Task description for this worker"}}}},"required":["task"]}}}}}})",
            name, entry.description);
    }
    arr += "]";
    return arr;
}

inline std::string SupervisorAgent::merge_tools_json(
        std::string_view global, std::string_view workers) {
    auto strip = [](std::string_view s) -> std::string {
        size_t l = s.find('[');
        size_t r = s.rfind(']');
        if (l == std::string_view::npos || r == std::string_view::npos || l >= r)
            return {};
        std::string inner(s.substr(l + 1, r - l - 1));
        size_t start = inner.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return {};
        size_t end = inner.find_last_not_of(" \t\n\r");
        return inner.substr(start, end - start + 1);
    };
    std::string g = strip(global);
    std::string w = strip(workers);
    if (g.empty() && w.empty()) return "[]";
    if (g.empty()) return "[" + w + "]";
    if (w.empty()) return "[" + g + "]";
    return "[" + g + "," + w + "]";
}

inline tools::ToolResult SupervisorAgent::dispatch_worker(
        WorkerEntry& entry, const std::string& task, size_t& call_count) {
    // Injection check — record the attempt even if blocked
    if (this->os_->security() != nullptr) {
        auto dr = this->os_->security()->detector().scan(task);
        if (dr.is_injection) {
            DelegationRecord rec;
            rec.supervisor_id = this->id_;
            rec.worker_id     = entry.agent->id();
            rec.task          = task;
            rec.result        = {};
            rec.success       = false;
            rec.elapsed       = std::chrono::milliseconds{0};
            rec.timestamp     = Clock::now();
            { std::lock_guard lk(mu_); delegation_log_.push_back(std::move(rec)); }
            return tools::ToolResult{.success = false,
                                     .output  = {},
                                     .error   = "injection detected in delegation task"};
        }
    }

    // Call cap
    if (call_count >= entry.max_calls) {
        return tools::ToolResult{.success = false,
                                 .output  = {},
                                 .error   = fmt::format(
                                     "worker '{}' call cap ({}) exceeded",
                                     entry.agent->config().name, entry.max_calls)};
    }
    ++call_count;

    // Delegate
    auto t0      = Clock::now();
    auto res     = entry.agent->run(task);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);

    DelegationRecord rec;
    rec.supervisor_id = this->id_;
    rec.worker_id     = entry.agent->id();
    rec.task          = task;
    rec.elapsed       = elapsed;
    rec.timestamp     = Clock::now();

    if (res.has_value()) {
        std::string out = *res;
        if (out.size() > kMaxDelegationResultChars) {
            out.resize(kMaxDelegationResultChars);
            rec.result_truncated = true;
        }
        rec.result  = out;
        rec.success = true;
        { std::lock_guard lk(mu_); delegation_log_.push_back(std::move(rec)); }
        return tools::ToolResult{.success = true, .output = std::move(out)};
    } else {
        rec.result  = {};
        rec.success = false;
        std::string err = res.error().message;
        { std::lock_guard lk(mu_); delegation_log_.push_back(std::move(rec)); }
        return tools::ToolResult{.success = false, .output = {}, .error = std::move(err)};
    }
}

inline Result<std::string> SupervisorAgent::run(std::string user_input) {
    if (!this->os_)
        return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");

    // Depth guard
    if (tl_supervisor_depth >= max_depth_)
        return make_error(ErrorCode::PermissionDenied, "delegation depth limit reached");
    ScopeDepthGuard depth_guard;

    // Per-run call counts (single run() per instance — no concurrent run() needed)
    std::unordered_map<std::string, size_t> call_counts;

    // Append user message to context
    this->os_->ctx().append(this->id_, kernel::Message::user(user_input));

    std::string last_content;

    for (int step = 0; step < MAX_STEPS; ++step) {
        // Build LLMRequest directly (bypasses think() to merge worker tools)
        kernel::LLMRequest req;
        req.agent_id = this->id_;
        req.priority = this->config_.priority;

        auto& win = this->os_->ctx().get_window(this->id_, this->config_.context_limit);
        req.messages.assign(win.messages().begin(), win.messages().end());

        // Merge global tools + worker tools
        std::string global_tj = this->os_->tools().tools_json(this->config_.allowed_tools);
        std::string worker_tj = build_workers_tools_json();
        std::string merged    = merge_tools_json(global_tj, worker_tj);
        if (!merged.empty() && merged != "[]") req.tools_json = merged;

        auto infer_result = this->os_->kernel().infer(req);
        if (!infer_result)
            return make_error(infer_result.error().code, infer_result.error().message);

        auto& resp   = *infer_result;
        last_content = resp.content;

        // Append assistant message to context
        {
            auto m = kernel::Message::assistant(resp.content);
            if (resp.wants_tool_call()) m.tool_calls = resp.tool_calls;
            this->os_->ctx().append(this->id_, std::move(m));
        }

        if (!resp.wants_tool_call()) break;

        // Dispatch each tool call
        for (const auto& call : resp.tool_calls) {
            tools::ToolResult tool_res;

            auto it = workers_.find(call.name);
            if (it != workers_.end()) {
                // Worker intercept: parse task from args JSON
                std::string task_str = call.args_json;
                try {
                    auto j = nlohmann::json::parse(call.args_json);
                    if (j.contains("task") && j["task"].is_string())
                        task_str = j["task"].get<std::string>();
                } catch (...) {}

                tool_res = dispatch_worker(it->second, task_str, call_counts[call.name]);
            } else {
                // Normal tool dispatch via act()
                auto act_res = this->act(call);
                if (act_res) {
                    tool_res = *act_res;
                } else {
                    tool_res = tools::ToolResult{.success = false,
                                                 .output  = {},
                                                 .error   = act_res.error().message};
                }
            }

            // Append tool result as observation
            std::string obs = tool_res.success ? tool_res.output
                                               : ("[error] " + tool_res.error);
            kernel::Message tool_msg;
            tool_msg.role         = kernel::Role::Tool;
            tool_msg.content      = std::move(obs);
            tool_msg.name         = call.name;
            tool_msg.tool_call_id = call.id;
            this->os_->ctx().append(this->id_, std::move(tool_msg));
        }
    }

    return last_content;
}

} // namespace agentos
