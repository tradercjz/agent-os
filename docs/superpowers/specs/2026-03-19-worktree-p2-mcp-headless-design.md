# Worktree Phase 2, MCP Adapter & Headless Runner Design

**Date:** 2026-03-19
**Status:** Approved (all decisions per recommendation)

---

## Feature 4: Worktree Phase 2 — Agent-Scoped Isolation

### Approach
Agent-scoped worktrees: each agent with `IsolationMode::Worktree` gets a dedicated worktree on creation, all its tasks run there, worktree cleaned up on destroy.

### Changes

**AgentConfig** — add isolation mode:
```cpp
IsolationMode isolation{IsolationMode::Thread};  // default: no worktree
```

**AgentOS** — owns a `WorktreeManager`, auto-manages worktree lifecycle:
- `create_agent()`: if isolation == Worktree, call `worktree_mgr_->create(agent_name)`
- `destroy_agent()`: if agent had worktree, call `worktree_mgr_->remove()`
- New accessor: `worktree_mgr()` for direct access
- New method on Agent: `work_dir()` returns worktree path (or repo root for Thread mode)

**AgentOS::Config** — add worktree settings:
```cpp
std::filesystem::path repo_root{std::filesystem::current_path()};
std::filesystem::path worktree_base{".agentos/worktrees"};
uint32_t max_worktrees{10};
```

### Testing
- Create agent with Worktree isolation → verify worktree directory exists
- Destroy agent → verify worktree removed
- work_dir() returns correct path per isolation mode
- Capacity limits propagated from WorktreeManager

---

## Feature 5: MCP Protocol Adapter

### Approach
Lightweight JSON-RPC adapter layer wrapping ToolManager. Implements the core MCP message types (initialize, tools/list, tools/call) as a header-only class. No network layer — just message handling. Applications can wire it to HTTP/stdio/WebSocket as needed.

### New Types
```cpp
// include/agentos/mcp/mcp_server.hpp
struct MCPRequest {
    std::string jsonrpc{"2.0"};
    std::string method;
    Json params;
    Json id;  // string or int
};

struct MCPResponse {
    std::string jsonrpc{"2.0"};
    Json result;
    Json error;  // {code, message, data}
    Json id;
};

class MCPServer {
public:
    MCPServer(tools::ToolRegistry& registry, std::string server_name, std::string version);
    MCPResponse handle(const MCPRequest& req);
    MCPResponse handle_json(const std::string& json_str);

private:
    MCPResponse handle_initialize(const MCPRequest&);
    MCPResponse handle_tools_list(const MCPRequest&);
    MCPResponse handle_tools_call(const MCPRequest&);
};
```

### Supported Methods
- `initialize` → returns server info + capabilities
- `tools/list` → returns all registered tools as MCP tool definitions
- `tools/call` → dispatches to ToolManager, returns result

### Testing
- Initialize returns server name/version
- tools/list returns correct tool count
- tools/call dispatches and returns result
- Unknown method returns error

---

## Feature 6: Headless Runner

### Approach
A standalone class that takes a JSON config, creates AgentOS + agent, runs a task, and returns structured output. Designed for CI/webhook integration.

### New Types
```cpp
// include/agentos/headless/runner.hpp
struct RunRequest {
    std::string task;           // user input / task description
    std::string agent_name{"headless-agent"};
    std::string role_prompt;
    std::string config_json;    // optional AgentOS config overrides
    Duration timeout{Duration{60000}};
};

struct RunResult {
    bool success;
    std::string output;
    std::string error;
    uint64_t duration_ms;
    uint64_t tokens_used;
};

class HeadlessRunner {
public:
    explicit HeadlessRunner(std::unique_ptr<kernel::ILLMBackend> backend);
    RunResult run(const RunRequest& req);
    static RunRequest from_json(const Json& j);
};
```

### Testing
- Run with mock backend returns result
- Timeout is respected
- from_json parses correctly
- Error handling for invalid config

---

## File Map

| File | Action | Feature |
|------|--------|---------|
| `include/agentos/agent.hpp` | Modify | Worktree P2: AgentConfig + Agent::work_dir() |
| `include/agentos/worktree/types.hpp` | Already exists | IsolationMode already defined |
| `tests/test_worktree_integration.cpp` | Create | Worktree P2 tests |
| `include/agentos/mcp/mcp_server.hpp` | Create | MCP adapter |
| `tests/test_mcp.cpp` | Create | MCP tests |
| `include/agentos/headless/runner.hpp` | Create | Headless runner |
| `tests/test_headless.cpp` | Create | Headless tests |
| `CMakeLists.txt` | Modify | Add test files |
