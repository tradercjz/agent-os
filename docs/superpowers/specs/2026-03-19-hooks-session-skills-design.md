# Hooks, Session Persistence & Skill Registry Design

**Date:** 2026-03-19
**Status:** Approved

## Overview

Three features extending AgentOS, each building on existing infrastructure:

1. **Hooks System** — Tool-level lifecycle interception via extended Middleware
2. **Session Persistence** — Serialize/resume full agent state (context + config + middleware names)
3. **Skill Registry** — Auto-activate domain knowledge modules by keyword matching

---

## Feature 1: Hooks System

### Approach

Extend existing `HookContext` and `Middleware` (in `agent.hpp`) rather than introducing a separate hooks layer. Backward compatible — existing middleware continues to work unchanged.

### Changes to HookContext

```cpp
struct HookContext {
    AgentId agent_id;
    std::string operation;      // existing: "think", "act", "remember", "recall"
                                // new: "pre_tool_use", "post_tool_use", "stop"
    std::string input;          // for pre/post_tool_use: tool name
    Json args;                  // NEW: for pre/post_tool_use: tool call arguments
    bool cancelled{false};
    std::string cancel_reason;

    // NEW: post-hook result injection (only for post_tool_use)
    tools::ToolResult* result{nullptr};  // mutable pointer to tool result
};
```

### Execution Flow

```
act() call:
  1. before("act")              ← existing, coarse-grained
  2. before("pre_tool_use")     ← NEW, tool name in input, args in args
     → if cancelled: skip tool execution, return cancel_reason as error
  3. dispatch tool → get ToolResult
  4. after("post_tool_use")     ← NEW, result is mutable via result pointer
  5. after("act")               ← existing, coarse-grained

run() return:
  1. before("stop")             ← NEW, can set cancelled=true to force continue
     → if cancelled: agent continues ReAct loop instead of returning
```

### Post-Hook Result Mutation

Post-tool hooks receive a non-null `result` pointer. They may:
- Append to `result->output` (e.g., inject lint warnings)
- Overwrite `result->output`
- Set `result->success = false` to mark as failed
- Leave unchanged (read-only observation)

### Backward Compatibility

- Existing `before("act")` / `after("act")` hooks fire at the same points
- `args` field defaults to empty JSON object
- `result` pointer defaults to nullptr (safe for existing hooks)

---

## Feature 2: Session Persistence

### Approach

Build on existing `ContextSnapshot` (already has binary serialization with OOM hardening). Add `SessionState` struct that bundles context + agent config + middleware names.

### New Types

```cpp
// In context/context.hpp
struct SessionState {
    AgentId agent_id;
    SessionId session_id;
    AgentConfig config;
    std::vector<std::string> middleware_names;
    ContextSnapshot context;
    TimePoint saved_at;
    std::string metadata_json;

    std::vector<uint8_t> serialize_binary() const;
    static std::optional<SessionState> deserialize_binary(std::span<const uint8_t> data);
};
```

### New Methods on ContextManager

```cpp
Result<fs::path> save_session(AgentId id, const AgentConfig& cfg,
                              const std::vector<Middleware>& middleware);
Result<SessionState> load_session(AgentId id, SessionId session_id);
Result<std::vector<SessionId>> list_sessions(AgentId id);
```

### Serialization Format

Binary format extending ContextSnapshot:
```
[magic: 4B "SESS"] [version: 2B] [agent_id: 8B] [session_id: len+data]
[config_json: len+data] [middleware_names_count: 4B] [middleware_names: len+data each]
[context_snapshot: existing binary format] [metadata_json: len+data]
[saved_at: 8B] [crc32: 4B]
```

OOM hardening: same bounds as ContextSnapshot (50MB total, 100K messages, 10MB strings).

### Resume Flow

1. Application calls `load_session()` → gets `SessionState`
2. Application creates agent with `SessionState.config`
3. Application re-registers middleware by name (using its own registry)
4. Application calls `restore()` with the context data
5. Agent resumes with full conversation history

Note: Middleware functions (lambdas) cannot be serialized. Only names are saved. The application is responsible for re-registering middleware by name on resume.

---

## Feature 3: Skill Registry

### Approach

New `SkillRegistry` class with keyword-based matching. Skills are bundles of tools + prompt context that auto-activate when a task description matches.

### New Types

```cpp
// include/agentos/skills/skill_registry.hpp
struct SkillDef {
    std::string name;
    std::string description;
    std::vector<std::string> keywords;
    std::vector<tools::ToolSchema> tools;
    std::vector<tools::ToolFn> tool_fns;      // corresponding implementations
    std::string prompt_injection;              // extra system prompt when active
};

class SkillRegistry {
public:
    void register_skill(SkillDef skill);
    void remove_skill(const std::string& name);
    std::vector<const SkillDef*> match(const std::string& task_description) const;
    Result<void> activate(const std::string& skill_name, AgentOS& os, AgentId agent);
    Result<void> deactivate(const std::string& skill_name, AgentOS& os, AgentId agent);
    std::vector<std::string> active_skills(AgentId agent) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, SkillDef> skills_;
    std::unordered_map<AgentId, std::vector<std::string>> active_;
};
```

### Matching Algorithm

Case-insensitive substring scan:
1. For each registered skill, count how many of its keywords appear in the task description
2. Skills with zero hits are excluded
3. Results sorted by hit count descending
4. Caller decides activation threshold (e.g., >= 1 hit)

### Activation Flow

1. `match(task_description)` → returns matching skills sorted by relevance
2. `activate(skill_name, os, agent_id)`:
   - Registers skill's tools with the agent's ToolManager
   - Injects `prompt_injection` into agent's context as a system message
   - Records activation in `active_` map
3. `deactivate(skill_name, os, agent_id)`:
   - Unregisters skill's tools
   - Records deactivation

---

## File Map

| File | Action | Feature |
|------|--------|---------|
| `include/agentos/agent.hpp` | Modify | Hooks: extend HookContext, modify act()/run() |
| `tests/test_hooks.cpp` | Create | Hooks: test pre/post tool use, stop, result mutation |
| `include/agentos/context/context.hpp` | Modify | Session: add SessionState |
| `src/context/context.cpp` | Modify | Session: serialize/deserialize SessionState |
| `tests/test_session.cpp` | Create | Session: test save/load/list/resume |
| `include/agentos/skills/skill_registry.hpp` | Create | Skills: SkillDef + SkillRegistry |
| `tests/test_skill_registry.cpp` | Create | Skills: test register/match/activate |
| `CMakeLists.txt` | Modify | Add new test files |

## Testing Strategy

- Hooks: Mock agent with middleware, verify pre_tool_use cancellation blocks tool, post_tool_use mutates result, stop hook forces continuation
- Session: Round-trip serialize/deserialize, verify all fields restored, OOM bounds
- Skills: Register skills with keywords, verify matching, activate/deactivate lifecycle
