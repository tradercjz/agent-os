# Supervisor-Worker Multi-Agent Delegation

**Date:** 2026-03-18
**Status:** Approved
**Module:** `agentos::supervisor`

---

## Overview

Add a `SupervisorAgent` class that enables LLM-driven dynamic task delegation to
Worker Agents. Workers are registered as tools on the Supervisor; the existing
ReAct loop dispatches to them without any changes to `AgentBase`, `ToolManager`,
or `AgentBus`.

---

## Architecture

### Core Idea

Each Worker Agent is wrapped as a `ToolSchema + handler` and injected into the
Supervisor's `tools_json`. The Supervisor's LLM decides at each step which worker
to call, what task to pass, and when to return the final answer — identical to the
standard ReAct think→act→observe cycle.

```
SupervisorAgent (extends AgentBase)
  ├── worker_registry_: map<string, WorkerEntry>
  │     WorkerEntry = { shared_ptr<AgentBase>, description, max_calls }
  └── tool dispatch intercept:
        tool_name in worker_registry_ → worker->run(args["task"]) → ToolResult
```

### Data Structures

```cpp
struct WorkerEntry {
    std::shared_ptr<AgentBase> agent;
    std::string description;   // shown to supervisor LLM as tool description
    size_t max_calls{5};       // per-session call cap, prevents tight loops
};

struct DelegationRecord {
    AgentId supervisor_id;
    AgentId worker_id;
    std::string task;
    std::string result;
    std::chrono::milliseconds elapsed;
    TimePoint timestamp;
};
```

### Public API

```cpp
class SupervisorAgent : public AgentBase {
public:
    explicit SupervisorAgent(AgentOS& os, AgentConfig cfg = {});

    // Register a worker. Automatically creates a ToolSchema entry.
    SupervisorAgent& add_worker(std::shared_ptr<AgentBase> worker,
                                std::string description);

    // Run the supervisor with the given high-level task.
    std::string run(const std::string& task);

    // Inspect delegation history (useful for tests and tracing).
    const std::vector<DelegationRecord>& delegation_log() const;

    // Max nesting depth (default: 3). Prevents A→B→A loops.
    void set_max_depth(size_t depth);
};
```

---

## Key Flows

### Happy Path

```
supervisor->run("Analyse market and write a report")
  LLM think  → "Need market data first, call researcher"
  tool_call  → {name:"researcher", args:{task:"Analyse AI market trends"}}
  dispatch   → researcher->run("Analyse AI market trends") → "AI market +20%..."
  observe    → ToolResult{content:"AI market +20%..."}
  LLM think  → "Have data, call writer"
  tool_call  → {name:"writer", args:{task:"Write report based on: AI market +20%..."}}
  dispatch   → writer->run(...) → "Report: ..."
  observe    → LLM decides task complete
  return     → final synthesised answer
```

### Depth Guard

Each `SupervisorAgent` carries a thread-local delegation depth counter. If a
worker is itself a `SupervisorAgent`, the counter increments on entry and
decrements on exit. When `depth >= max_depth`, the call returns a
`ToolResult::fail("delegation depth limit reached")` instead of recursing.

---

## Protection Mechanisms

| Concern | Mechanism |
|---------|-----------|
| Infinite delegation loop (A→B→A) | `delegation_depth_` counter + `max_depth` cap (default 3) |
| Worker runaway calls | `max_calls` per WorkerEntry; returns error once exceeded |
| Timeout | Worker `run()` runs inside a `std::async` with same `tool_timeout_ms` as other tools |
| Permission bypass | Worker's `security_role` RBAC is still enforced; delegation does not grant elevated permissions |
| Injection via task string | Existing `InjectionDetector` scans task args before worker dispatch |

---

## Files Changed

| File | Action |
|------|--------|
| `include/agentos/supervisor_agent.hpp` | **New** — full implementation (header-only) |
| `include/agentos/agentos.hpp` | **Amend** — add `#include` for supervisor header |
| `tests/test_supervisor.cpp` | **New** — unit tests (≥12 test cases) |
| `CMakeLists.txt` | **Amend** — add test file |

**No changes** to: `agent.hpp`, `tool_manager.hpp`, `agent_bus.hpp`,
`scheduler.hpp`, `security.hpp`, or any existing `.cpp` file.

---

## Test Plan

1. `SingleWorker_TaskDelegated` — supervisor with one worker, task flows through correctly
2. `MultiWorker_LLMRoutes` — two workers, LLM picks the right one per task
3. `WorkerResultInContext` — worker result appears in supervisor's next LLM call
4. `MaxCallsExceeded` — calling the same worker >max_calls returns error
5. `DepthLimitPreventsRecursion` — nested supervisors stop at max_depth
6. `WorkerNotFound_NoTool` — calling an unregistered tool name falls through to normal tool dispatch
7. `EmptyWorkerList_NoDelegation` — supervisor with no workers behaves like normal agent
8. `DelegationLogRecorded` — delegation_log() captures correct supervisor/worker IDs and elapsed time
9. `InjectionInTaskBlocked` — task string with injection pattern is blocked before worker dispatch
10. `WorkerFailure_PropagatedToSupervisor` — worker returning failure is observed by supervisor LLM
11. `AsyncRun_FutureReturnsResult` — `run_async()` works on supervisor
12. `Integration_ThreeAgentPipeline` — researcher → analyst → writer pipeline produces coherent output
