# Subworker Worktree Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Phase 1 Claude Code-like subworkers to `SupervisorAgent` with dynamic worker templates, per-run git worktrees, a same-process executor, and structured subworker results/logging.

**Architecture:** Keep `SupervisorAgent` focused on orchestration and LLM routing while moving subworker lifecycle work into a new `SubworkerRuntime` plus `ISubworkerExecutor` abstraction. Phase 1 uses an in-process worktree executor but keeps the API shaped for a later child-process executor swap.

**Tech Stack:** C++20, existing `AgentOS`/`SupervisorAgent`, git worktree support, GoogleTest, header-first public API with focused `.cpp` runtime implementation.

---

## File Map

### Create

- `include/agentos/subworkers/runtime.hpp`
  Defines `WorkerTemplate`, `SubworkerRunOptions`, `SubworkerStatus`, `SubworkerResult`, `SubworkerRunRecord`, `ISubworkerExecutor`, and `SubworkerRuntime`.
- `src/subworkers/runtime.cpp`
  Implements `SubworkerRuntime` and Phase 1 `InProcWorktreeExecutor`.

### Modify

- `CMakeLists.txt`
  Add new runtime source to the main library.
- `include/agentos/supervisor_agent.hpp`
  Add worker template registration, explicit subworker API, template-aware routing, and structured subworker logs.
- `include/agentos/agentos.hpp`
  Expose new public subworker types if needed by umbrella users.
- `tests/test_supervisor.cpp`
  Add failing and passing tests for template-backed subworkers and explicit runtime behavior.

### Verify / Reference

- `include/agentos/agent.hpp`
  Check agent construction and `run()` behavior for temporary worker instances.
- `include/agentos/worktree/worktree_manager.hpp`
  Reuse worktree lifecycle API rather than introducing a parallel worktree helper.
- `docs/superpowers/specs/2026-03-20-subworker-worktree-design.md`
  Source of truth for boundaries and non-goals.

---

### Task 1: Scaffold Public Subworker Types

**Files:**
- Create: `include/agentos/subworkers/runtime.hpp`
- Modify: `include/agentos/agentos.hpp`
- Test: `tests/test_supervisor.cpp`

- [ ] **Step 1: Write the failing tests for public types and supervisor template registration**

Add tests covering:

```cpp
TEST(SupervisorTest, WorkerTemplateRegistrationDoesNotCrash) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    WorkerTemplate tpl{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    };

    EXPECT_NO_THROW(sup->add_worker_template("researcher", tpl));
}

TEST(SupervisorTest, ExplicitRunUnknownTemplateReturnsError) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    auto res = sup->run_subworker("missing", "do work");
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ErrorCode::NotFound);
}
```

- [ ] **Step 2: Run the focused test target to verify failure**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.WorkerTemplateRegistrationDoesNotCrash:SupervisorTest.ExplicitRunUnknownTemplateReturnsError
```

Expected:
- Build or test fails because `WorkerTemplate` / `run_subworker` / `add_worker_template` do not exist yet.

- [ ] **Step 3: Add minimal type declarations and umbrella exposure**

Implement the minimal compile surface in `include/agentos/subworkers/runtime.hpp`:

```cpp
enum class SubworkerStatus { Succeeded, Failed, Cancelled, TimedOut };

struct WorkerTemplate {
    std::string name;
    std::string description;
    AgentConfig config;
    IsolationMode isolation{IsolationMode::Worktree};
    Duration timeout{std::chrono::seconds(300)};
    bool preserve_worktree_on_failure{true};
    bool allow_parallel{false};
};

struct SubworkerRunOptions {
    std::optional<Duration> timeout;
    std::optional<bool> preserve_worktree_on_failure;
    std::optional<std::filesystem::path> preferred_worktree_base;
    std::string metadata_json{"{}"};
};

struct SubworkerResult { /* fields from spec */ };
struct SubworkerRunRecord { /* fields from spec */ };
```

Wire them into `include/agentos/agentos.hpp` if the umbrella header should expose them.

- [ ] **Step 4: Add minimal `SupervisorAgent` declarations**

In `include/agentos/supervisor_agent.hpp`, add declarations and stub behavior:

```cpp
SupervisorAgent& add_worker_template(std::string name, WorkerTemplate tpl);
Result<SubworkerResult> run_subworker(const std::string& name,
                                      const std::string& task,
                                      const SubworkerRunOptions& opts = {});
std::vector<SubworkerRunRecord> subworker_log() const;
```

For now:
- `add_worker_template` stores the template
- `run_subworker` returns `NotFound` if template does not exist
- `subworker_log` returns a snapshot copy

- [ ] **Step 5: Re-run the focused tests to verify pass**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.WorkerTemplateRegistrationDoesNotCrash:SupervisorTest.ExplicitRunUnknownTemplateReturnsError
```

Expected:
- Both tests pass

- [ ] **Step 6: Commit**

```bash
git add include/agentos/subworkers/runtime.hpp include/agentos/supervisor_agent.hpp include/agentos/agentos.hpp tests/test_supervisor.cpp
git commit -m "feat(subworker): scaffold public runtime types"
```

---

### Task 2: Add `SubworkerRuntime` and Explicit In-Process Execution

**Files:**
- Create: `src/subworkers/runtime.cpp`
- Modify: `CMakeLists.txt`
- Modify: `include/agentos/subworkers/runtime.hpp`
- Modify: `include/agentos/supervisor_agent.hpp`
- Test: `tests/test_supervisor.cpp`

- [ ] **Step 1: Write the failing tests for explicit subworker execution**

Add tests:

```cpp
TEST(SupervisorTest, ExplicitRunSubworkerCreatesWorktree) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    WorkerTemplate tpl{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    };
    sup->add_worker_template("researcher", tpl);

    auto res = sup->run_subworker("researcher", "Inspect the repository");
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->worker_name, "researcher");
    EXPECT_FALSE(res->worktree_path.empty());
    EXPECT_TRUE(std::filesystem::exists(res->worktree_path));
}

TEST(SupervisorTest, ExplicitRunSubworkerRecordsStructuredLog) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    WorkerTemplate tpl{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    };
    sup->add_worker_template("researcher", tpl);

    auto res = sup->run_subworker("researcher", "Inspect the repository");
    ASSERT_TRUE(res.has_value());

    auto log = sup->subworker_log();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].worker_name, "researcher");
    EXPECT_EQ(log[0].task, "Inspect the repository");
}
```

- [ ] **Step 2: Run the focused tests to verify failure**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.ExplicitRunSubworkerCreatesWorktree:SupervisorTest.ExplicitRunSubworkerRecordsStructuredLog
```

Expected:
- Tests fail because `run_subworker` is not implemented.

- [ ] **Step 3: Implement `SubworkerRuntime` interface and in-process executor**

In `include/agentos/subworkers/runtime.hpp`, declare:

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
    explicit SubworkerRuntime(std::shared_ptr<ISubworkerExecutor> executor = {});
    Result<SubworkerResult> run(
        AgentOS& os,
        AgentId supervisor_id,
        const WorkerTemplate& tpl,
        const std::string& task,
        const SubworkerRunOptions& opts = {});
};
```

In `src/subworkers/runtime.cpp`, implement:
- default `InProcWorktreeExecutor`
- `SubworkerRuntime::run(...)`
- worktree allocation via existing `WorktreeManager`
- temporary agent creation from `tpl.config`
- synchronous `run(task)`
- structured `SubworkerResult`

- [ ] **Step 4: Wire `SupervisorAgent::run_subworker` through `SubworkerRuntime`**

Implementation shape:

```cpp
auto it = worker_templates_.find(name);
if (it == worker_templates_.end())
    return make_error(ErrorCode::NotFound, "subworker template not found");

auto result = runtime_.run(*this->os_, this->id_, it->second, task, opts);
if (result) {
    std::lock_guard lk(mu_);
    subworker_log_.push_back(record_from(*result));
}
return result;
```

- [ ] **Step 5: Re-run the focused tests to verify pass**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.ExplicitRunSubworkerCreatesWorktree:SupervisorTest.ExplicitRunSubworkerRecordsStructuredLog
```

Expected:
- Both tests pass

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt include/agentos/subworkers/runtime.hpp src/subworkers/runtime.cpp include/agentos/supervisor_agent.hpp tests/test_supervisor.cpp
git commit -m "feat(subworker): add explicit in-proc worktree runtime"
```

---

### Task 3: Route Template Workers Through `SupervisorAgent::run()`

**Files:**
- Modify: `include/agentos/supervisor_agent.hpp`
- Test: `tests/test_supervisor.cpp`

- [ ] **Step 1: Write the failing tests for automatic LLM delegation**

Add tests:

```cpp
TEST(SupervisorTest, LLMDelegatesToTemplateWorker) {
    auto os = make_os();
    auto mock = dynamic_cast<MockLLMBackend*>(&os->kernel().backend());
    ASSERT_NE(mock, nullptr);

    auto sup = os->create_agent<SupervisorAgent>(
        AgentConfig{.name = "sup", .role_prompt = "Delegate tasks."});

    WorkerTemplate tpl{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    };
    sup->add_worker_template("researcher", tpl);

    mock->register_rule("use researcher",
                        R"({"tool_calls":[{"id":"t1","name":"researcher","arguments":"{\"task\":\"inspect code\"}"}]})");

    auto res = sup->run("use researcher");
    ASSERT_TRUE(res.has_value());

    auto log = sup->subworker_log();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].worker_name, "researcher");
}

TEST(SupervisorTest, TemplateAndLegacyWorkersCoexist) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});
    auto legacy = os->create_agent<ReActAgent>(AgentConfig{.name = "writer"});

    sup->add_worker(legacy, "Writes summaries");
    sup->add_worker_template("researcher", WorkerTemplate{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    });

    EXPECT_NO_THROW(sup->build_tools_json_for_tests());
}
```

If a helper is needed for tests, add a narrow test-only-accessible helper rather than opening large internals.

- [ ] **Step 2: Run the focused tests to verify failure**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.LLMDelegatesToTemplateWorker:SupervisorTest.TemplateAndLegacyWorkersCoexist
```

Expected:
- Tests fail because template workers are not merged into `run()` routing yet.

- [ ] **Step 3: Extend tool JSON building and routing order**

Update `SupervisorAgent` internals so `run()` merges:
- global tools
- legacy worker tools
- template worker tools

Routing precedence:
1. template workers
2. legacy workers
3. regular tools

Add an internal helper similar to:

```cpp
std::string build_template_tools_json() const;
```

- [ ] **Step 4: Append structured subworker results back into context**

When a template-backed worker is called:
- run `run_subworker(...)`
- convert `SubworkerResult` into tool observation text
- append as `Role::Tool`

Observation format:

```text
[subworker:researcher]
status: succeeded
summary: ...
worktree: ...
output:
...
```

- [ ] **Step 5: Re-run the focused tests to verify pass**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.LLMDelegatesToTemplateWorker:SupervisorTest.TemplateAndLegacyWorkersCoexist
```

Expected:
- Both tests pass

- [ ] **Step 6: Commit**

```bash
git add include/agentos/supervisor_agent.hpp tests/test_supervisor.cpp
git commit -m "feat(subworker): route template workers through supervisor run loop"
```

---

### Task 4: Failure Semantics and Worktree Policies

**Files:**
- Modify: `include/agentos/subworkers/runtime.hpp`
- Modify: `src/subworkers/runtime.cpp`
- Modify: `include/agentos/supervisor_agent.hpp`
- Test: `tests/test_supervisor.cpp`

- [ ] **Step 1: Write the failing tests for failure handling**

Add tests:

```cpp
TEST(SupervisorTest, SubworkerFailurePropagates) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    WorkerTemplate tpl{
        .name = "bad_worker",
        .description = "Fails intentionally",
        .config = AgentConfig{.name = "bad_worker", .role_prompt = ""}
    };
    sup->add_worker_template("bad_worker", tpl);

    auto res = sup->run_subworker("bad_worker", "force failure");
    ASSERT_FALSE(res.has_value() || res->status == SubworkerStatus::Succeeded);
}

TEST(SupervisorTest, DistinctSubworkerRunsUseDistinctWorktrees) {
    auto os = make_os();
    auto sup = os->create_agent<SupervisorAgent>(AgentConfig{.name = "sup"});

    WorkerTemplate tpl{
        .name = "researcher",
        .description = "Researches implementation details",
        .config = AgentConfig{.name = "researcher_worker", .role_prompt = "Research carefully."}
    };
    sup->add_worker_template("researcher", tpl);

    auto r1 = sup->run_subworker("researcher", "task one");
    auto r2 = sup->run_subworker("researcher", "task two");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(r1->worktree_path, r2->worktree_path);
}
```

- [ ] **Step 2: Run the focused tests to verify failure**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.SubworkerFailurePropagates:SupervisorTest.DistinctSubworkerRunsUseDistinctWorktrees
```

Expected:
- Tests fail because failure semantics and worktree uniqueness are incomplete.

- [ ] **Step 3: Implement result/status normalization**

In runtime/executor:
- map execution outcomes into `SubworkerStatus`
- return `Failed` on runtime errors
- populate `error` consistently
- keep failed worktrees by default

- [ ] **Step 4: Guarantee unique per-run worktree naming**

Use a run-scoped identifier:

```cpp
auto run_id = fmt::format("sw_{}_{}", supervisor_id, next_nonce());
```

Worktree name should include:
- supervisor id
- worker template name
- unique nonce

- [ ] **Step 5: Re-run the focused tests to verify pass**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.SubworkerFailurePropagates:SupervisorTest.DistinctSubworkerRunsUseDistinctWorktrees
```

Expected:
- Both tests pass

- [ ] **Step 6: Commit**

```bash
git add include/agentos/subworkers/runtime.hpp src/subworkers/runtime.cpp include/agentos/supervisor_agent.hpp tests/test_supervisor.cpp
git commit -m "feat(subworker): harden result and worktree lifecycle"
```

---

### Task 5: Full Regression Sweep and API Cleanup

**Files:**
- Modify: `tests/test_supervisor.cpp`
- Modify: any touched Phase 1 files as needed

- [ ] **Step 1: Add final regression coverage for the Phase 1 API surface**

Ensure `tests/test_supervisor.cpp` includes:
- template registration
- explicit success path
- explicit missing-template error
- automatic LLM delegation
- coexistence with legacy workers
- failure propagation
- distinct worktree allocation

- [ ] **Step 2: Run the focused supervisor suite**

Run:

```bash
cmake --build build --target test_agentos -j4
./build/test_agentos --gtest_filter=SupervisorTest.*
```

Expected:
- All `SupervisorTest.*` tests pass

- [ ] **Step 3: Run adjacent regression suites affected by orchestration changes**

Run:

```bash
./build/test_agentos --gtest_filter=AgentOSTest.*:ReflectiveAgentOSTest.*:IntegrationTest.*:TracerIntegrationTest.*
```

Expected:
- All pass

- [ ] **Step 4: Run worktree-adjacent tests if available in this build**

Run:

```bash
./build/test_agentos --gtest_filter=Worktree*
```

Expected:
- If tests are registered in this build, they pass
- If none are registered, record that fact in the final summary

- [ ] **Step 5: Commit**

```bash
git add include/agentos/supervisor_agent.hpp include/agentos/subworkers/runtime.hpp src/subworkers/runtime.cpp include/agentos/agentos.hpp tests/test_supervisor.cpp CMakeLists.txt
git commit -m "feat(subworker): deliver phase 1 worktree subworkers"
```

---

## Notes for Execution

- Use TDD exactly: write the failing test first, run it, then implement.
- Do not introduce child-process logic in this phase.
- Keep `SupervisorAgent` orchestration-focused; push execution into runtime.
- Prefer small helper functions over growing `supervisor_agent.hpp` into another monolith.
- Do not broaden scope into parallel scheduling, cancellation, or streaming logs yet.
