# Runtime Hardening Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the highest-risk runtime lifecycle bug in `HeadlessRunner` and route MCP tool calls through the same validated execution pipeline used by agents.

**Architecture:** Keep the public surface small and avoid broad refactors. Fix `HeadlessRunner` by keeping the agent object alive until its async task is finished and by making timeout handling non-destructive. Fix MCP by injecting a `ToolManager` instead of calling `ToolRegistry` tools directly, so MCP gets argument validation, timeout handling, and future policy hooks through one path.

**Tech Stack:** C++23, CMake, GoogleTest, existing AgentOS runtime modules (`agent`, `headless`, `tools`, `mcp`)

---

## File Map

- Modify: `include/agentos/headless/runner.hpp`
  Responsibility: make headless execution timeout-safe and lifetime-safe.
- Modify: `include/agentos/mcp/mcp_server.hpp`
  Responsibility: route MCP tool calls through `ToolManager`.
- Modify: `tests/test_headless.cpp`
  Responsibility: add regression coverage for timeout behavior and agent cleanup semantics.
- Modify: `tests/test_mcp.cpp`
  Responsibility: add regression coverage proving MCP uses `ToolManager` validation/timeout behavior.

### Task 1: Harden `HeadlessRunner` Timeout and Lifetime Semantics

**Files:**
- Modify: `include/agentos/headless/runner.hpp`
- Test: `tests/test_headless.cpp`

- [ ] **Step 1: Write the failing timeout regression test**

Add a test that runs a deliberately slow agent through `HeadlessRunner` with a short timeout and verifies:
- `run()` returns a timeout error
- execution does not crash
- the slow agent still completes safely after the timeout path

- [ ] **Step 2: Run the targeted headless test to verify it fails**

Run: `ctest --output-on-failure -R '^HeadlessRunnerTest\\.'`
Expected: new timeout regression fails because current timeout handling destroys the agent while work may still still be running.

- [ ] **Step 3: Implement the minimal lifetime-safe fix**

Update `HeadlessRunner::run()` so timeout does not immediately destroy the agent object that owns the running task. Keep the `shared_ptr` alive until the async work is settled, then perform cleanup in a safe order.

- [ ] **Step 4: Run the targeted headless test suite**

Run: `ctest --output-on-failure -R '^HeadlessRunnerTest\\.'`
Expected: all headless tests pass.

- [ ] **Step 5: Refactor only if needed**

If the fix needs helper extraction, keep it local to `runner.hpp` and do not widen the API.

### Task 2: Route MCP Tool Calls Through `ToolManager`

**Files:**
- Modify: `include/agentos/mcp/mcp_server.hpp`
- Test: `tests/test_mcp.cpp`

- [ ] **Step 1: Write the failing MCP regression tests**

Add tests that prove MCP `tools/call`:
- returns an error payload when required parameters are missing
- respects `ToolManager` timeout handling for slow tools

- [ ] **Step 2: Run the targeted MCP tests to verify they fail**

Run: `ctest --output-on-failure -R '^MCPServerTest\\.'`
Expected: new regression tests fail because `MCPServer` currently calls `ToolRegistry` tools directly and bypasses `ToolManager::dispatch()`.

- [ ] **Step 3: Implement the minimal execution-path unification**

Change `MCPServer` to depend on `ToolManager` for `tools/list` and `tools/call`, and construct `kernel::ToolCallRequest` values that flow through the normal dispatch path.

- [ ] **Step 4: Run the targeted MCP test suite**

Run: `ctest --output-on-failure -R '^MCPServerTest\\.'`
Expected: all MCP tests pass.

- [ ] **Step 5: Keep response shape stable**

Preserve existing JSON-RPC and MCP response structure so current tests and clients do not break.

### Task 3: Verify the Runtime-Hardening Slice

**Files:**
- Test: `tests/test_headless.cpp`
- Test: `tests/test_mcp.cpp`

- [ ] **Step 1: Run focused verification**

Run: `ctest --output-on-failure -R '^(HeadlessRunnerTest|MCPServerTest)\\.'`
Expected: all focused regression tests pass.

- [ ] **Step 2: Run broader tool/runtime verification**

Run: `ctest --output-on-failure -R '^(ToolManagerTest|HeadlessRunnerTest|MCPServerTest)\\.'`
Expected: pass, confirming MCP unification did not break tool execution semantics.

- [ ] **Step 3: Inspect working tree**

Run: `git status --short`
Expected: only intended files changed.

- [ ] **Step 4: Commit**

```bash
git add include/agentos/headless/runner.hpp include/agentos/mcp/mcp_server.hpp tests/test_headless.cpp tests/test_mcp.cpp docs/superpowers/plans/2026-03-19-runtime-hardening-phase-1.md
git commit -m "fix: harden headless runtime and unify mcp tool dispatch"
```
