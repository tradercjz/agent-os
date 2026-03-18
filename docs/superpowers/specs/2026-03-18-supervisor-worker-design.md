# Supervisor-Worker Multi-Agent Delegation

**Date:** 2026-03-18
**Status:** Approved
**Module:** `agentos` (top-level header)

---

## Overview

Add a `SupervisorAgent` class that enables LLM-driven dynamic task delegation to
Worker Agents. Workers are registered on the Supervisor and presented to its LLM
as tools alongside the normal global tool registry. The Supervisor's own `run()`
loop intercepts tool calls whose names match registered workers, calls
`worker->run(task)` directly, and feeds the result back as an observation.

No changes to `Agent`, `AgentBase`, `ToolManager`, or `AgentBus`.

---

## Architecture

### LLM Request Construction (bypasses `think()`)

`AgentBase::think()` builds `tools_json` exclusively from `os_->tools()`. Since
workers are per-supervisor (not global), `SupervisorAgent::run()` does **not**
call `this->think()`. Instead it builds the `LLMRequest` directly:

```cpp
// Inside run() loop:
kernel::LLMRequest req;
req.agent_id = this->id_;
req.priority = this->config_.priority;
auto& win = this->os_->ctx().get_window(this->id_, this->config_.context_limit);
req.messages.assign(win.messages().begin(), win.messages().end());

// Merge: global tool registry + registered workers
std::string global_tools = this->os_->tools().tools_json(this->config_.allowed_tools);
std::string worker_tools = build_workers_tools_json();
req.tools_json = merge_tools_json(global_tools, worker_tools); // concatenate JSON arrays
```

`merge_tools_json(a, b)` is a local helper that concatenates two JSON arrays,
handling empty cases (`"[]"`, `""`). It does not require nlohmann::json; a simple
string splice is sufficient.

The assistant message and tool_result observations are appended directly to
`os_->ctx()` after each inference step, exactly as `AgentBase::think()` would.
Non-worker tool calls go through `this->act(call)` unchanged.

### Depth Tracking via `thread_local`

Delegation depth is tracked with a `thread_local` counter and a stack RAII guard:

```cpp
// File-scope in supervisor_agent.hpp (anonymous namespace):
namespace { thread_local size_t tl_supervisor_depth = 0; }

struct ScopeDepthGuard {
    ScopeDepthGuard()  noexcept { ++tl_supervisor_depth; }
    ~ScopeDepthGuard() noexcept { --tl_supervisor_depth; }
};
```

At entry to `run()`: if `tl_supervisor_depth >= max_depth_`, return
`Err(ErrorCode::PermissionDenied, "delegation depth limit reached")` immediately
(before constructing a `ScopeDepthGuard`). Otherwise construct a guard, then
proceed. When `worker->run(task)` is called, if that worker is itself a
`SupervisorAgent`, its `run()` will increment `tl_supervisor_depth` again before
making its own check. Thread-safe: `thread_local` is per-call-stack; `run_async()`
creates a new thread with its own counter starting at 0.

### Concurrency Contract

**Concurrent `run()` calls on the same `SupervisorAgent` instance are not
supported** (analogous to calling `std::vector::push_back` from two threads).
`run_async()` submits to a single `std::future`; callers must not call `run()`
concurrently on the same instance. This matches the contract of `ReActAgent` and
`PlanningAgent`. `call_count` reset and loop state are per-`run()` invocation;
since only one `run()` executes at a time per instance, no extra locking is needed
for `call_count` beyond what `mu_` already provides for `delegation_log_` writes.

---

## Data Structures

```cpp
struct WorkerEntry {
    std::shared_ptr<Agent> agent;
    std::string description;    // shown to supervisor LLM as tool description
    size_t max_calls{5};        // per-run call cap
};

// Max chars stored from a worker's output (project OOM-hardening convention)
inline constexpr size_t kMaxDelegationResultChars = 4096;

struct DelegationRecord {
    AgentId supervisor_id;
    AgentId worker_id;
    std::string task;
    std::string result;              // worker output, capped at kMaxDelegationResultChars
    bool result_truncated{false};
    std::chrono::milliseconds elapsed;
    TimePoint timestamp;             // Clock::now() — agentos::TimePoint (steady_clock)
    bool success{true};
};
```

Required includes in `supervisor_agent.hpp`:
```cpp
#include <agentos/agent.hpp>        // Agent, AgentBase, AgentOS, etc.
#include <agentos/core/types.hpp>   // AgentId, TimePoint, Duration, Result
```

---

## Public API

```cpp
class SupervisorAgent : public AgentBase<SupervisorAgent> {
public:
    using AgentBase<SupervisorAgent>::AgentBase;

    // Register a worker. Fluent, pointer-based:
    //   auto sup = os->create_agent<SupervisorAgent>(cfg);
    //   sup->add_worker(researcher, "Researches web content");
    SupervisorAgent& add_worker(std::shared_ptr<Agent> worker,
                                std::string description,
                                size_t max_calls = 5);

    // Required override. Returns Result<std::string>.
    Result<std::string> run(std::string user_input) override;

    // Returns a snapshot copy of delegation history. Thread-safe: takes mu_.
    std::vector<DelegationRecord> delegation_log() const;

    // Max nesting depth (default: 3). Enforced via thread_local.
    void set_max_depth(size_t depth);

private:
    // Build OpenAI-style JSON array for all registered workers.
    std::string build_workers_tools_json() const;

    // Merge two JSON arrays (handles empty/null cases).
    static std::string merge_tools_json(std::string_view global, std::string_view workers);

    // Dispatch one worker call. Handles injection check, max_calls cap, timing.
    tools::ToolResult dispatch_worker(WorkerEntry& entry, const std::string& task,
                                      size_t& call_count);

    std::unordered_map<std::string, WorkerEntry> workers_;   // keyed by agent config().name
    std::vector<DelegationRecord> delegation_log_;
    size_t max_depth_{3};
    static constexpr int MAX_STEPS = 10;
    mutable std::mutex mu_;   // guards: delegation_log_ writes; delegation_log() reads
};
```

---

## Injection Check

```cpp
// Inside dispatch_worker():
if (this->os_->security() != nullptr) {
    auto dr = this->os_->security()->detector().scan(task);
    if (dr.is_injection) {
        return tools::ToolResult{.success = false,
                                 .output  = {},
                                 .error   = "injection detected in delegation task"};
    }
}
// Proceed to worker->run(task)
```

`detector().scan()` returns `DetectionResult` (a struct). The check is on the
`.is_injection` bool field. When `os_->security()` is `nullptr`
(`enable_security = false`), the scan is skipped entirely.

---

## ToolResult Wrapping

`dispatch_worker` wraps `Result<std::string>` from `worker->run()`:

```cpp
auto t0 = Clock::now();
auto res = entry.agent->run(task);
auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);

std::string output_str;
bool success = res.has_value();
if (success) {
    output_str = *res;   // dereference Result<std::string>
    if (output_str.size() > kMaxDelegationResultChars) {
        output_str.resize(kMaxDelegationResultChars);
        // DelegationRecord.result_truncated = true
    }
}
// Return:
// success: ToolResult{.success=true,  .output=output_str, .error={}}
// failure: ToolResult{.success=false, .output={},         .error=res.error().message}
```

---

## Key Flow

```
supervisor->run("Analyse market and write a report")
  depth check  → tl_supervisor_depth(0) < max_depth_(3) → ok, ScopeDepthGuard{}
  build req   → LLMRequest{messages=ctx, tools_json=merge(global, workers)}
  infer       → LLMResponse{tool_calls=[{name:"researcher", args:{task:"..."}}]}
  intercept   → workers_.contains("researcher") → true
  call_count  → 0 < max_calls(5) → ok; call_count++
  inject chk  → security != null → scan(task) → is_injection=false → ok
  dispatch    → researcher->run("Analyse AI market trends")
                [tl_supervisor_depth now 1 if researcher is also a SupervisorAgent]
              ← Ok{"AI market +20%..."}
  append ctx  → ToolResult observation to context window
  record      → DelegationRecord{..., success=true} under mu_
  LLM next   → decides task complete → return Ok("Report: ...")
  ~Guard      → tl_supervisor_depth back to 0
```

---

## Protection Summary

| Concern | Mechanism |
|---------|-----------|
| Infinite delegation loop | `thread_local tl_supervisor_depth` + `ScopeDepthGuard`; error when `>= max_depth_` |
| Worker call runaway | Per-entry `call_count` vs `max_calls`; failed `ToolResult` once exceeded |
| Worker execution time | Bounded by worker's own `MAX_STEPS = 10` ReAct loop; no extra async layer |
| Permission bypass | Worker RBAC enforced inside `worker->run()` |
| Injection in task args | `detector().scan(task).is_injection` checked with null-guard on `security()` |
| Thread safety | `delegation_log_` writes + `delegation_log()` reads both under `mu_`; `call_count` per-run only (single concurrent run per instance) |
| OOM | `DelegationRecord::result` capped at 4096 chars; `result_truncated` flag set |

---

## Files Changed

| File | Action |
|------|--------|
| `include/agentos/supervisor_agent.hpp` | **New** — full header-only implementation |
| `include/agentos/agentos.hpp` | **Amend** — add `#include <agentos/supervisor_agent.hpp>` |
| `tests/test_supervisor.cpp` | **New** — 12 tests via `MockLLMBackend` |
| `CMakeLists.txt` | **Amend** — add `test_supervisor.cpp` to test targets |

**No changes** to: `agent.hpp`, `tool_manager.hpp`, `agent_bus.hpp`,
`scheduler.hpp`, `security.hpp`, or any `.cpp` file.

---

## Test Plan (12 tests, `enable_security=true` unless noted)

| # | Name | What it verifies |
|---|------|-----------------|
| 1 | `SingleWorker_TaskDelegated` | One worker; task routed, correct result returned |
| 2 | `MultiWorker_LLMRoutes` | Two workers; mock LLM picks correct one per task |
| 3 | `WorkerResultInContext` | Worker output appears in supervisor's next LLM observation |
| 4 | `MaxCallsExceeded` | Same worker called > `max_calls` → failed ToolResult, worker not invoked again |
| 5 | `DepthLimitPreventsRecursion` | Nested supervisors: depth >= max_depth → Err returned cleanly |
| 6 | `NonWorkerToolPassthrough` | Unknown tool name falls through to `AgentBase::act()` |
| 7 | `EmptyWorkerList_NormalAgent` | Supervisor with no workers runs like a plain `ReActAgent` |
| 8 | `DelegationLogRecorded` | `delegation_log()` snapshot has correct IDs, task, result, elapsed, success |
| 9 | `InjectionInTaskBlocked` | Injection in task string → failed ToolResult; `worker->run()` not called (security enabled) |
| 10 | `WorkerFailure_ObservedBySupervisor` | Worker returning `Err` → `ToolResult{success=false}` in supervisor context |
| 11 | `AsyncRun_FutureReturnsResult` | `run_async()` completes and returns `Ok(result)` |
| 12 | `Integration_ThreeAgentPipeline` | researcher → analyst → writer; final output contains all three contributions |
