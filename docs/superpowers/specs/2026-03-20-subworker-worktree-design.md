# Worktree Subworker Design Spec

**Date:** 2026-03-20
**Status:** Draft
**Scope:** Claude Code-like subworkers backed by git worktrees
**Primary Goal:** User-experience-first subworker delegation for `SupervisorAgent`

---

## 1. Overview

Add a Claude Code-like subworker model on top of the existing `SupervisorAgent`.
The primary user experience is:

- A supervisor can expose subworkers to the LLM as callable tools
- The LLM can delegate a sub-task to a named subworker
- Each subworker run gets its own git worktree
- The subworker result flows back into the supervisor's context as a tool observation

The design is intentionally phased:

- **Phase 1** delivers the user-facing behavior with a same-process executor
- **Phase 2** swaps the executor to a child-process model without changing the
  top-level API

This keeps the first implementation small enough to ship while preserving a
path to true Claude Code-style isolation later.

---

## 2. Goals

- Extend `SupervisorAgent` from static in-memory workers to dynamic subworkers
- Make subworkers available through both:
  - LLM-driven automatic delegation inside `run()`
  - explicit API calls for tests and advanced orchestration
- Allocate a dedicated git worktree per subworker run
- Return structured subworker results instead of plain strings
- Keep the runtime boundary abstract so Phase 2 can use child processes

## 3. Non-Goals

- No true cross-process execution in Phase 1
- No long-lived resident worker pool
- No full job scheduler for many concurrent subworkers
- No streaming stdout/stderr transport in Phase 1
- No UI layer or TUI for subworker visualization

---

## 4. Existing Baseline

Current repository support:

- `SupervisorAgent` already exists in
  `include/agentos/supervisor_agent.hpp`
- Existing delegation uses `add_worker()` with pre-instantiated `Agent`
  objects
- Workers are exposed to the LLM as tool definitions
- `SupervisorAgent::run()` intercepts worker-named tool calls and directly
  invokes `worker->run(task)`
- There is already worktree design material in
  `docs/superpowers/specs/2026-03-18-worktree-isolation-design.md`

Gap versus desired subworker behavior:

- no dynamic worker creation
- no per-run worktree allocation
- no structured subworker lifecycle
- no executor abstraction
- no explicit subworker API

---

## 5. User Experience

Target interaction model:

1. Application creates a `SupervisorAgent`
2. Application registers one or more subworker templates
3. Supervisor `run()` exposes those templates to the LLM as callable tools
4. LLM chooses a subworker tool and passes a task string
5. Runtime creates a dedicated worktree and runs the subworker
6. Result is appended back to supervisor context
7. Supervisor continues reasoning and either delegates again or returns a final answer

Explicit API is also supported:

```cpp
auto result = supervisor->run_subworker("researcher", "Investigate build failures");
```

This explicit path is not the primary UX, but it is required for:

- unit tests
- deterministic integration tests
- advanced host applications

---

## 6. Core Architecture

### 6.1 Main Components

#### `SupervisorAgent`

Responsibilities:

- register worker templates
- expose worker templates as LLM-callable tools
- route tool calls to either:
  - legacy in-memory workers
  - subworker runtime
- append subworker results back into context

#### `SubworkerRuntime`

Responsibilities:

- validate requested worker template
- allocate a worktree
- create and execute a subworker run
- capture structured result
- record structured run log
- clean up or preserve worktree based on policy

#### `ISubworkerExecutor`

Responsibilities:

- run a single subworker task inside a given worktree
- hide whether execution is same-process or child-process

Implementations:

- **Phase 1:** `InProcWorktreeExecutor`
- **Phase 2:** `ProcessWorktreeExecutor`

#### `WorktreeManager`

Responsibilities:

- create/remove/list git worktrees
- remain focused on worktree lifecycle only
- not know anything about supervisors or subworker semantics

### 6.2 Key Boundary Rule

`SupervisorAgent` must not know whether a subworker is executed in-process or
out-of-process.

That detail belongs exclusively to `SubworkerRuntime` plus the active executor.

This is the main architectural rule that keeps Phase 1 from blocking Phase 2.

---

## 7. Core Types

### `WorkerTemplate`

Represents a dynamically creatable subworker definition.

```cpp
struct WorkerTemplate {
    std::string name;                 // LLM-visible tool name
    std::string description;          // LLM-visible description
    AgentConfig config;               // Base agent config for spawned worker
    IsolationMode isolation{IsolationMode::Worktree};
    Duration timeout{std::chrono::seconds(300)};
    bool preserve_worktree_on_failure{true};
    bool allow_parallel{false};
};
```

Notes:

- `AgentConfig` reuse keeps worker construction aligned with the rest of the runtime
- `IsolationMode` is future-facing; Phase 1 only supports worktree-backed execution
- `allow_parallel` is advisory for future scheduling, not fully implemented in Phase 1

### `SubworkerRunOptions`

Per-invocation overrides.

```cpp
struct SubworkerRunOptions {
    std::optional<Duration> timeout;
    std::optional<bool> preserve_worktree_on_failure;
    std::optional<std::filesystem::path> preferred_worktree_base;
    std::string metadata_json{"{}"};
};
```

### `SubworkerResult`

Structured output from a single subworker run.

```cpp
enum class SubworkerStatus {
    Succeeded,
    Failed,
    Cancelled,
    TimedOut
};

struct SubworkerResult {
    std::string run_id;
    std::string worker_name;
    std::string task;
    SubworkerStatus status{SubworkerStatus::Failed};
    std::string summary;
    std::string output;
    std::string error;
    std::filesystem::path worktree_path;
    std::chrono::milliseconds elapsed{0};
};
```

### `SubworkerRunRecord`

Persistent record kept by the supervisor/runtime.

```cpp
struct SubworkerRunRecord {
    AgentId supervisor_id{0};
    std::string run_id;
    std::string worker_name;
    std::string task;
    SubworkerStatus status{SubworkerStatus::Failed};
    std::filesystem::path worktree_path;
    std::chrono::milliseconds elapsed{0};
    TimePoint started_at{Clock::now()};
    TimePoint finished_at{Clock::now()};
    bool output_truncated{false};
    std::string summary;
    std::string output;
    std::string error;
};
```

This is conceptually the replacement for the current plain `DelegationRecord`
for template-backed workers.

---

## 8. API Surface

### `SupervisorAgent` Additions

```cpp
class SupervisorAgent : public AgentBase<SupervisorAgent> {
public:
    // Existing API remains:
    SupervisorAgent& add_worker(std::shared_ptr<Agent> worker,
                                std::string description,
                                size_t max_calls = 5);

    // New API:
    SupervisorAgent& add_worker_template(std::string name, WorkerTemplate tpl);

    Result<SubworkerResult> run_subworker(
        const std::string& name,
        const std::string& task,
        const SubworkerRunOptions& opts = {});

    std::vector<SubworkerRunRecord> subworker_log() const;
};
```

### Internal Runtime API

```cpp
class ISubworkerExecutor {
public:
    virtual ~ISubworkerExecutor() = default;

    virtual Result<SubworkerResult> run(
        AgentOS& os,
        AgentId supervisor_id,
        const WorkerTemplate& tpl,
        const std::string& task,
        const std::filesystem::path& worktree_path,
        const SubworkerRunOptions& opts) = 0;
};

class SubworkerRuntime {
public:
    Result<SubworkerResult> run(
        AgentOS& os,
        AgentId supervisor_id,
        const WorkerTemplate& tpl,
        const std::string& task,
        const SubworkerRunOptions& opts = {});
};
```

---

## 9. LLM Tool Exposure

Worker templates are exposed to the LLM similarly to the current in-memory
workers.

Each template becomes a function tool:

```json
{
  "type": "function",
  "function": {
    "name": "researcher",
    "description": "Researches implementation details in an isolated worktree",
    "parameters": {
      "type": "object",
      "properties": {
        "task": {
          "type": "string",
          "description": "Sub-task for this subworker"
        }
      },
      "required": ["task"]
    }
  }
}
```

Tool routing priority inside `SupervisorAgent::run()`:

1. template-backed subworker names
2. legacy in-memory worker names
3. normal `ToolManager` tools

This avoids ambiguity and keeps the user-facing subworker path dominant.

---

## 10. Runtime Flow

### 10.1 Explicit API Flow

```text
supervisor->run_subworker("researcher", "Investigate bug")
  -> lookup WorkerTemplate
  -> SubworkerRuntime::run(...)
  -> WorktreeManager::create(...)
  -> executor->run(...)
  -> collect SubworkerResult
  -> record SubworkerRunRecord
  -> return Result<SubworkerResult>
```

### 10.2 Automatic LLM Delegation Flow

```text
SupervisorAgent::run(user_input)
  -> build tools_json with:
       global tools + legacy workers + worker templates
  -> infer
  -> LLM returns tool call { name: "researcher", args: { task: "..." } }
  -> route to run_subworker(...)
  -> runtime creates worktree and executes worker
  -> result converted into tool observation
  -> append observation into context
  -> continue ReAct loop
```

### 10.3 Tool Observation Format

Tool observation should remain compact and LLM-friendly.

Recommended observation body:

```text
[subworker:researcher]
status: succeeded
summary: Investigated the failing path and identified the parser bug.
worktree: /path/to/worktree
output:
<subworker output here>
```

This preserves:

- worker identity
- execution status
- worktree traceability
- full textual output for continued reasoning

---

## 11. Phase Plan

## Phase 1: In-Process Worktree Subworkers

Deliver:

- `WorkerTemplate`
- `SubworkerRuntime`
- `ISubworkerExecutor`
- `InProcWorktreeExecutor`
- `SupervisorAgent::add_worker_template`
- `SupervisorAgent::run_subworker`
- template-backed tool exposure in `SupervisorAgent::run()`
- structured subworker log

Execution model:

- runtime creates a dedicated worktree
- executor creates a temporary agent instance in-process
- temporary agent runs using the provided template config
- result is returned synchronously

Phase 1 explicitly does **not** include:

- child process spawn
- IPC
- heartbeats
- crash recovery for a separate worker process
- streaming logs
- reusable warm worker pool

## Phase 2: Child-Process Subworkers

Replace only the executor implementation:

- `ProcessWorktreeExecutor`
- process bootstrap config
- independent worker process
- structured handoff and result return
- heartbeat and crash handling

The top-level `SupervisorAgent` and `SubworkerRuntime` APIs stay unchanged.

---

## 12. Error Handling

Required failures:

- unknown worker template
- worktree creation failure
- executor failure
- task timeout
- cancellation
- result serialization/truncation failure

Recommended error behavior:

- explicit API returns `Err(...)`
- automatic LLM path converts failures into a tool observation with
  `status: failed`
- failed runs still create a `SubworkerRunRecord`
- failed runs preserve worktree by default for debugging

Potential new error codes:

- `SubworkerNotFound`
- `SubworkerExecutionFailed`
- `SubworkerTimeout`

If adding new error codes is too invasive in Phase 1, map to existing:

- `ErrorCode::NotFound`
- `ErrorCode::RuntimeError`
- `ErrorCode::Timeout`

---

## 13. Testing Strategy

### Phase 1 Required Tests

1. `SupervisorTemplateRegistration`
- register worker template without crash
- tool JSON contains the template

2. `ExplicitRunSubworkerCreatesWorktree`
- `run_subworker(...)` succeeds
- returned result contains non-empty `worktree_path`
- log contains one record

3. `LLMDelegatesToTemplateWorker`
- mock LLM emits a template worker tool call
- supervisor routes to subworker runtime
- final run succeeds

4. `SubworkerUsesDedicatedWorktree`
- two runs create distinct worktree paths

5. `UnknownTemplateReturnsError`
- explicit API rejects missing template

6. `SubworkerFailurePropagates`
- executor failure returns failed result
- log records failure

7. `TemplateAndLegacyWorkersCoexist`
- `add_worker()` and `add_worker_template()` can both be registered
- routing remains deterministic

### Verification Rules

- tests must not depend on global fixed temp directories
- worktree paths must be unique per test
- tests should verify structured result fields, not just string output

---

## 14. File Impact

Recommended minimal file set for Phase 1:

- `include/agentos/supervisor_agent.hpp`
  - add template registration and runtime routing
- `include/agentos/subworkers/runtime.hpp`
  - declare runtime, executor interface, result types
- `src/subworkers/runtime.cpp`
  - runtime and in-process executor implementation
- `include/agentos/agentos.hpp`
  - umbrella exposure if public API requires it
- `tests/test_supervisor.cpp`
  - add template-backed subworker tests

Optional if better separation is needed:

- `include/agentos/subworkers/types.hpp`
- `include/agentos/subworkers/executor.hpp`

For Phase 1, avoid over-splitting unless the header becomes unwieldy.

---

## 15. Recommended MVP

The first shippable increment should include only:

- one template registration path
- one synchronous explicit API
- one automatic LLM delegation path
- per-run worktree allocation
- structured result/logging

Anything beyond that should be deferred until the first end-to-end experience is
actually usable.

That MVP is enough to answer the real product question:

> Can a supervisor delegate a coding task to a Claude Code-like subworker that
> runs in its own worktree and returns a usable result back into the main loop?

If the answer is yes, the architecture is validated and Phase 2 becomes a
runtime substitution problem, not a redesign.
