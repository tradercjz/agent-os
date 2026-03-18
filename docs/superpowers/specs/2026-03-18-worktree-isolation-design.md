# Worktree Isolation Design Spec

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Git worktree-based agent isolation for cpp-agent-os

---

## 1. Overview

Implement a Git worktree-based isolation mechanism that allows agents to work in independent copies of the repository. This enables parallel development workflows where multiple agents edit code, compile, and commit without interfering with each other — similar to Claude Code's worktree feature.

### Goals

- **Library-level `WorktreeManager`** — CRUD for git worktrees, automatic lifecycle management
- **Runtime isolation** — agents can execute file/git/build tools in isolated worktree directories
- **Mixed process model** — default same-process (cwd-bound), optionally escalate to independent process
- **Transparent IPC** — `AgentBus` routes messages identically regardless of in-process or cross-process agent
- **Configurable limits** — max concurrent worktrees, RBAC-gated process isolation

### Non-Goals

- Distributed (cross-machine) agent execution (future extension via `TcpTransport`)
- Automatic merge conflict resolution
- Shared memory or context between sub-agents

---

## 2. Core Types

```cpp
// include/agentos/worktree/types.hpp

enum class IsolationMode { Thread, Worktree };
enum class WorktreeState { Creating, Active, Merging, Removing, Failed };

struct WorktreeInfo {
    std::string name;              // e.g. "feature-auth"
    std::string branch;            // auto-created branch name
    std::filesystem::path path;    // .claude/worktrees/<name>/
    WorktreeState state;
    AgentId owner_agent;
    std::optional<pid_t> pid;      // set when running as independent process
    TimePoint created_at;
    bool has_changes;
};
```

---

## 3. WorktreeManager

Manages git worktree lifecycle with configurable capacity and automatic cleanup.

```cpp
// include/agentos/worktree/worktree_manager.hpp

struct WorktreeConfig {
    std::filesystem::path repo_root;
    std::filesystem::path worktree_base;       // default: repo_root/.claude/worktrees/
    uint32_t max_concurrent{10};
    bool auto_cleanup{true};
    Duration cleanup_timeout{60s};
};

class WorktreeManager {
public:
    explicit WorktreeManager(WorktreeConfig cfg);

    // CRUD
    Result<WorktreeInfo> create(std::string name, std::optional<std::string> base_branch = {});
    Result<void> remove(const std::string& name, bool force = false);
    Result<std::vector<WorktreeInfo>> list() const;
    Result<WorktreeInfo> get(const std::string& name) const;

    // Lifecycle
    Result<bool> has_changes(const std::string& name) const;
    Result<void> auto_cleanup(const std::string& name);
    Result<void> cleanup_all();

    // Capacity
    bool at_capacity() const;
    uint32_t active_count() const;

private:
    WorktreeConfig cfg_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, WorktreeInfo> worktrees_;

    Result<void> exec_git(std::vector<std::string> args) const;
    Result<std::string> exec_git_output(std::vector<std::string> args) const;
};
```

### Key Behaviors

- `create("feature-auth")` executes `git worktree add .claude/worktrees/feature-auth -b agent/feature-auth`
- `remove()` checks `has_changes()` first; refuses if changes exist and `force=false`
- `auto_cleanup()` removes worktree if no changes, preserves and logs branch name if changes exist
- `create()` returns `ErrorCode::ResourceExhausted` when `at_capacity()`

---

## 4. Transport Abstraction Layer

Abstract the message transport so `AgentBus` can route identically to in-process and cross-process agents.

### Transport Interface

```cpp
// include/agentos/bus/transport.hpp

class Transport {
public:
    virtual ~Transport() = default;
    virtual Result<void> send(const BusMessage& msg) = 0;
    virtual Result<BusMessage> recv(Duration timeout) = 0;
    virtual void close() = 0;
    virtual bool is_connected() const = 0;
};
```

### InProcTransport

```cpp
// include/agentos/bus/inproc_transport.hpp

// Wraps existing Channel memory queue as a Transport.
class InProcTransport : public Transport {
public:
    explicit InProcTransport(std::shared_ptr<Channel> channel);
    Result<void> send(const BusMessage& msg) override;
    Result<BusMessage> recv(Duration timeout) override;
    void close() override;
    bool is_connected() const override;

private:
    std::shared_ptr<Channel> channel_;
    std::atomic<bool> closed_{false};
};
```

### UnixSocketTransport

```cpp
// include/agentos/bus/unix_socket_transport.hpp

struct SocketConfig {
    std::filesystem::path socket_path;        // .claude/worktrees/<name>/agent.sock
    Duration connect_timeout{5s};
    Duration read_timeout{30s};
    size_t max_message_size{4 * 1024 * 1024}; // 4MB
};

class UnixSocketTransport : public Transport {
public:
    static Result<std::unique_ptr<UnixSocketTransport>> listen(SocketConfig cfg);
    static Result<std::unique_ptr<UnixSocketTransport>> connect(SocketConfig cfg);

    Result<void> send(const BusMessage& msg) override;
    Result<BusMessage> recv(Duration timeout) override;
    void close() override;
    bool is_connected() const override;

private:
    explicit UnixSocketTransport(int fd, SocketConfig cfg);
    int fd_{-1};
    SocketConfig cfg_;
    std::atomic<bool> closed_{false};
    mutable std::mutex write_mu_;

    // Frame protocol: [uint32_t big-endian length][JSON payload]
    Result<void> write_frame(std::string_view data);
    Result<std::string> read_frame();
};
```

### AgentBus Changes

Minimal change: `Channel` internally holds a `Transport`. New method `register_remote(AgentId, unique_ptr<Transport>)` for cross-process agents. Routing logic unchanged — looks up target `AgentId`, calls `transport->send()`.

---

## 5. ProcessManager

Manages sub-agent process lifecycle with health monitoring.

```cpp
// include/agentos/worktree/process_manager.hpp

struct ProcessConfig {
    std::filesystem::path executable;         // default: self
    Duration startup_timeout{10s};
    Duration shutdown_timeout{5s};
    Duration heartbeat_interval{3s};
    uint32_t max_missed_heartbeats{3};
};

enum class ProcessState { Starting, Running, Stopping, Exited, Failed };

struct ProcessInfo {
    pid_t pid;
    std::string worktree_name;
    AgentId agent_id;
    ProcessState state;
    TimePoint started_at;
    int exit_code;
    std::filesystem::path socket_path;
};

class ProcessManager {
public:
    explicit ProcessManager(ProcessConfig cfg);
    ~ProcessManager();  // calls shutdown_all()

    Result<ProcessInfo> spawn(const std::string& worktree_name,
                              const std::filesystem::path& worktree_path,
                              AgentId agent_id,
                              const AgentConfig& agent_cfg);
    Result<void> shutdown(pid_t pid);       // SIGTERM -> wait -> SIGKILL
    Result<void> shutdown_all();
    Result<ProcessInfo> get(pid_t pid) const;
    std::vector<ProcessInfo> list() const;

    void start_monitor();
    void stop_monitor();

    using DeathCallback = std::function<void(ProcessInfo)>;
    void on_death(DeathCallback cb);

private:
    ProcessConfig cfg_;
    mutable std::mutex mu_;
    std::unordered_map<pid_t, ProcessInfo> processes_;
    std::jthread monitor_thread_;
    DeathCallback death_cb_;

    void monitor_loop(std::stop_token st);
};
```

### Sub-Agent Process Bootstrap

```cpp
struct SubAgentBootstrap {
    std::string worktree_name;
    std::filesystem::path worktree_path;
    std::filesystem::path socket_path;
    AgentConfig agent_cfg;
    std::string llm_backend_json;
};

// Sub-process entry point
int subagent_main(SubAgentBootstrap boot);
```

### Startup Sequence

```
Orchestrator                          SubAgent Process
    |                                       |
    +-- fork+exec(--worktree <name>         |
    |          --socket <path>              |
    |          --agent-config <json>)       |
    |                                       |
    |       +-------------------------------+
    |       | 1. chdir(worktree_path)       |
    |       | 2. init slim AgentOS          |
    |       |    (Kernel + ToolManager      |
    |       |     + ContextManager)         |
    |       | 3. connect(socket_path)       |
    |       +--- Handshake(agent_id) ------>|
    |<------+                               |
    |       | 4. enter agent.run() loop     |
    |       +-------------------------------+
    |                                       |
    +-- BusMessage (task dispatch) -------->|
    |<-- BusMessage (result) ---------------|
    |                                       |
    +-- BusMessage (heartbeat ping) ------->|
    |<-- BusMessage (heartbeat pong) -------|
    |                                       |
    +-- BusMessage (shutdown) ------------->|
    |       | cleanup + exit(0)             |
    |<------+                               |
```

Sub-process initializes only: `LLMKernel`, `ToolManager`, `ContextManager`, `UnixSocketTransport`. Does NOT load: `Scheduler`, `MemorySystem`, `SecurityManager`, full `AgentBus`.

### Design Decisions

- **fork+exec over fork** — clean address space, avoids post-fork mutex issues
- **Heartbeat via BusMessage** — reuses existing `MessageType::Heartbeat`
- **RAII** — `ProcessManager` destructor calls `shutdown_all()`, no orphan processes

---

## 6. AgentOS & Scheduler Integration

### AgentConfig Extensions

```cpp
struct AgentConfig {
    // ... existing fields ...
    IsolationMode isolation{IsolationMode::Thread};
    std::optional<std::string> worktree_name;
    std::optional<std::string> base_branch;
    bool auto_cleanup{true};
    std::optional<std::filesystem::path> working_dir;  // set by worktree creation
};
```

### AgentOS Extensions

```cpp
class AgentOS {
public:
    WorktreeManager& worktree_mgr();
    ProcessManager& process_mgr();

    template<typename AgentT = ReActAgent>
    Result<AgentId> create_agent(AgentConfig cfg) {
        if (cfg.isolation == IsolationMode::Worktree) {
            return create_worktree_agent<AgentT>(std::move(cfg));
        }
        return create_inproc_agent<AgentT>(std::move(cfg));
    }

private:
    std::unique_ptr<worktree::WorktreeManager> worktree_mgr_;
    std::unique_ptr<worktree::ProcessManager> process_mgr_;

    template<typename AgentT>
    Result<AgentId> create_worktree_agent(AgentConfig cfg) {
        // 1. RBAC check
        TRY(security_->check(cfg.role, Permission::WorktreeCreate));
        // 2. Create worktree
        auto name = cfg.worktree_name.value_or(generate_worktree_name(cfg));
        TRY_DECL(wt, worktree_mgr_->create(name, cfg.base_branch));
        // 3. Allocate agent ID
        auto id = next_agent_id();
        // 4. Same-process or independent process
        if (needs_process_isolation(cfg)) {
            TRY(security_->check(cfg.role, Permission::WorktreeProcess));
            TRY_DECL(proc, process_mgr_->spawn(name, wt.path, id, cfg));
        } else {
            cfg.working_dir = wt.path;
            create_inproc_agent<AgentT>(std::move(cfg));
        }
        return id;
    }
};
```

### Scheduler Extensions

```cpp
struct AgentTaskDescriptor {
    // ... existing fields ...
    std::optional<std::filesystem::path> working_dir;
    IsolationMode isolation;
};
```

### SupervisorAgent Integration

```cpp
Result<std::string> SupervisorAgent::delegate(
    const std::string& worker_name, const std::string& task)
{
    auto& entry = workers_.at(worker_name);
    if (entry.config.isolation == IsolationMode::Worktree) {
        TRY_DECL(id, os_->create_agent(entry.config));
        auto result = os_->run_agent(id, task);
        os_->destroy_agent(id);  // triggers auto_cleanup
        return result;
    }
    return delegate_inproc(worker_name, task);
}
```

### Builder API

```cpp
auto os = AgentOSBuilder()
    .openai("sk-...", "gpt-4o")
    .threads(4)
    .repo_root("/path/to/project")
    .max_worktrees(10)
    .build();

auto agent = os->agent("refactor-bot")
    .prompt("Refactor the database layer")
    .tools({"read_file", "write_file", "git_commit", "cmake_build"})
    .isolation(IsolationMode::Worktree)
    .base_branch("main")
    .create();
```

---

## 7. Security, Cleanup & Error Handling

### RBAC Extensions

```cpp
enum class Permission : uint32_t {
    // ... existing ...
    WorktreeCreate   = 0x00001000,
    WorktreeProcess  = 0x00002000,
    WorktreeForce    = 0x00004000,
};
```

- `make_standard()` includes `WorktreeCreate` only
- `make_privileged()` includes `WorktreeCreate + WorktreeProcess`

### Worktree State Machine

```
create() --> Creating --> Active --> [agent completes]
                                        |
                                   has_changes?
                                   /          \
                                 yes           no
                                  |             |
                               Merging      Removing
                                  |             |
                            [merge done]    [git remove]
                                  |             |
                               Removing      (gone)
                                  |
                               (gone)

Any stage failure --> Failed (preserve scene, log error)
```

### Agent Destruction Cleanup

```cpp
Result<void> AgentOS::destroy_agent(AgentId id) {
    // 1. Existing cleanup: context, bus channel, agent object
    // 2. If cross-process: shutdown sub-process (SIGTERM -> wait -> SIGKILL)
    // 3. If worktree bound: auto_cleanup (remove if no changes, preserve if changes)
}
```

### Error Scenarios

| Scenario | Behavior |
|----------|----------|
| Sub-process crash | Preserve worktree (state=Failed), publish `agent.died` on bus |
| Worktree creation failure (disk full, git error) | Return Error, caller decides fallback |
| Socket disconnect | Mark agent unreachable, treat as process death |
| Orchestrator shutdown | drain scheduler -> shutdown_all processes -> cleanup_all worktrees |

### Graceful Shutdown Order

```
1. scheduler_->drain()            -- wait for in-flight tasks
2. process_mgr_->shutdown_all()   -- SIGTERM all sub-processes
3. worktree_mgr_->cleanup_all()   -- remove no-change worktrees
4. bus_->shutdown()                -- close all transports
5. kernel_->shutdown()             -- close LLM connections
```

---

## 8. File Structure

### New Files

```
include/agentos/
  worktree/
    types.hpp                    # IsolationMode, WorktreeState, WorktreeInfo
    worktree_manager.hpp         # WorktreeManager
    process_manager.hpp          # ProcessManager
  bus/
    transport.hpp                # Transport interface
    inproc_transport.hpp         # InProcTransport
    unix_socket_transport.hpp    # UnixSocketTransport

src/
  worktree/
    worktree_manager.cpp         # git command execution, state machine
    process_manager.cpp          # fork+exec, waitpid, heartbeat monitor
  bus/
    inproc_transport.cpp
    unix_socket_transport.cpp    # socket creation, frame I/O
  main_subagent.cpp              # sub-process entry point

tests/
  test_worktree_manager.cpp      # CRUD, capacity, auto cleanup
  test_process_manager.cpp       # spawn/shutdown, heartbeat, crash callback
  test_transport.cpp             # InProc + UnixSocket round-trip
  test_worktree_integration.cpp  # end-to-end lifecycle
```

### Modified Files

| File | Change | Scope |
|------|--------|-------|
| `include/agentos/agent.hpp` | `AgentConfig` fields, `AgentOS` members + `create_worktree_agent()` | Medium |
| `include/agentos/agentos.hpp` | Builder: `.repo_root()`, `.max_worktrees()`, `.isolation()` | Small |
| `include/agentos/bus/agent_bus.hpp` | Channel holds Transport, add `register_remote()` | Small |
| `include/agentos/security/security.hpp` | New permission bits | Small |
| `include/agentos/supervisor_agent.hpp` | `delegate()` worktree branch | Small |
| `include/agentos/scheduler/scheduler.hpp` | `AgentTaskDescriptor` + `working_dir`/`isolation` | Minimal |
| `include/agentos/core/types.hpp` | `ErrorCode::WorktreeError` | Minimal |

### External Dependencies

None added. Uses:
- Git CLI via `popen`/`exec` (existing `PipeGuard` RAII)
- POSIX Unix domain sockets (`sys/socket.h`, `sys/un.h`)
- nlohmann::json for BusMessage serialization (existing)
- fmt for log formatting (existing)

---

## 9. Module Dependency Graph

```
                    AgentOS (facade)
                   /    |    \
                  /     |     \
    WorktreeManager  ProcessManager  AgentBus
         |               |          /       \
         |               |   InProcTransport  UnixSocketTransport
         |               |         |                |
         v               v         v                v
      core/types     <unistd.h>  Channel       <sys/socket.h>
      (git CLI)      (fork/exec) (existing)    <sys/un.h>

    Dependency direction: top -> bottom, no cycles
```
