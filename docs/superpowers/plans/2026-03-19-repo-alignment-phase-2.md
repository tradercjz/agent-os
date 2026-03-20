# Repo Alignment Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align the repository's versioning, build/docs claims, and public examples with the current codebase so the published surface matches reality.

**Architecture:** Keep this slice narrow and factual. Use one test to pin the runtime version string to the public macros, then update the hard-coded version macros to match the CMake project version, and finally clean README/example text so compiler level, dependencies, and API samples reflect the actual implementation.

**Tech Stack:** C++23, CMake, GoogleTest, Markdown docs, existing AgentOS examples

---

## File Map

- Modify: `include/agentos/agentos.hpp`
  Responsibility: public SDK version constants and top-level usage comments.
- Modify: `tests/test_agentos.cpp`
  Responsibility: catch version string drift in the public SDK macros.
- Modify: `README.md`
  Responsibility: fix inaccurate claims about language level, dependencies, and sample API usage.
- Modify: `examples/mcp_demo.cpp`
  Responsibility: keep the MCP example aligned with the current constructor/API shape.

### Task 1: Pin Public Version Metadata

**Files:**
- Modify: `include/agentos/agentos.hpp`
- Test: `tests/test_agentos.cpp`

- [ ] **Step 1: Write the failing version drift test**

Add a test asserting `AGENTOS_VERSION_STRING` matches `version().to_string()`.

- [ ] **Step 2: Run the targeted version test to verify it fails**

Run: `ctest --output-on-failure -R '^AgentOSCoreTest\\.'`
Expected: fail because the hard-coded public version string is out of sync with the project versioning direction.

- [ ] **Step 3: Implement the minimal version fix**

Update the public version macros in `include/agentos/agentos.hpp` to one coherent version and keep the usage comment/examples consistent with the current defaults.

- [ ] **Step 4: Run the targeted version test suite**

Run: `ctest --output-on-failure -R '^AgentOSCoreTest\\.'`
Expected: pass.

### Task 2: Align README Claims With the Actual Build

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update language-level and dependency claims**

Correct the README so it no longer claims `C++20` or zero external dependencies when the build currently requires `C++23`, `libcurl`, `sqlite3`, and optional `DuckDB` / `cppjieba`.

- [ ] **Step 2: Fix sample construction code**

Update the OpenAI backend sample and any stale API snippets to match current constructors and builder usage.

- [ ] **Step 3: Refresh project layout and design highlights**

Adjust the overview text so it reflects the current module set and build reality without over-claiming portability.

### Task 3: Keep Examples and Public Surface Consistent

**Files:**
- Modify: `examples/mcp_demo.cpp`
- Modify: `include/agentos/agentos.hpp`

- [ ] **Step 1: Check example/API wording against current code**

Ensure comments and quickstart snippets reflect `ToolManager`-based MCP construction and current default model names.

- [ ] **Step 2: Rebuild affected targets**

Run: `cmake --build build --target test_agentos mcp_demo -j4`
Expected: build succeeds.

### Task 4: Verify the Repo-Alignment Slice

**Files:**
- Test: `tests/test_agentos.cpp`

- [ ] **Step 1: Run focused verification**

Run: `ctest --output-on-failure -R '^(AgentOSCoreTest|MCPServerTest)\\.'`
Expected: pass.

- [ ] **Step 2: Inspect working tree**

Run: `git status --short`
Expected: only intended files changed.

- [ ] **Step 3: Commit**

```bash
git add include/agentos/agentos.hpp tests/test_agentos.cpp README.md examples/mcp_demo.cpp docs/superpowers/plans/2026-03-19-repo-alignment-phase-2.md
git commit -m "docs: align public versioning and repository claims"
```
