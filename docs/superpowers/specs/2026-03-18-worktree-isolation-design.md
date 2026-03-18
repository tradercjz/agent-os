# Worktree Isolation Design Spec

**Date:** 2026-03-18
**Status:** Draft
**Scope:** Git worktree-based agent isolation for cpp-agent-os
**Platform:** POSIX only (Darwin/Linux). Uses `pid_t`, Unix domain sockets, `fork+exec`.

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
- Windows support (all process/socket APIs are POSIX)

---

## 2. Core Types

```cpp
// include/agentos/worktree/types.hpp

#include <sys/types.h>   // pid_t — POSIX only

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

### New Error Codes

```cpp
// Added to ErrorCode enum in core/types.hpp

WorktreeError,          // General worktree operation failure (git CLI error, disk full)
WorktreeCapacityFull,   // max_concurrent worktrees reached
ProcessSpawnFailed,     // fork+exec failure
ProcessDied,            // sub-process exited unexpectedly
HandshakeFailed,        // sub-process handshake timeout or protocol error
```

- `WorktreeCapacityFull` — returned by `WorktreeManager::create()` when at capacity
- `WorktreeError` — returned for git CLI failures, disk errors
- `ProcessSpawnFailed` / `ProcessDied` / `HandshakeFailed` — used by `ProcessManager`

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

    // Recovery: scan worktree_base on startup, reconcile with git worktree list
    Result<void> recover();

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
- `create()` returns `ErrorCode::WorktreeCapacityFull` when `at_capacity()`
- `recover()` called during `AgentOS` init — scans `.claude/worktrees/` directory and runs `git worktree list --porcelain` to reconcile in-memory state with on-disk reality after a crash

---

## 4. Transport Abstraction Layer

Abstract the message transport so `AgentBus` can route identically to in-process and cross-process agents.

**Key design: `AgentBus` routes by looking up a `Transport` per `AgentId`, NOT by wrapping `Channel` in a `Transport`.** The existing `Channel` class stays unchanged. Instead, `AgentBus` maintains a parallel map of `AgentId -> Transport` for remote agents, and routes to either the local `Channel` or the remote `Transport` based on agent type.

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
// Used only on the sub-agent side of same-process worktree agents,
// NOT used to replace Channel internals.
class InProcTransport : public Transport {
public:
    explicit InProcTransport(std::shared_ptr<Channel> channel);
    Result<void> send(const BusMessage& msg) override;   // delegates to channel_->push()
    Result<BusMessage> recv(Duration timeout) override;   // delegates to channel_->recv()
    void close() override;
    bool is_connected() const override;

private:
    std::shared_ptr<Channel> channel_;
    std::atomic<bool> closed_{false};
};
```

**Timeout semantics:** `InProcTransport::recv()` passes `timeout` directly to `Channel::recv()` — single timeout, no nesting.

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

```cpp
// Additions to existing AgentBus class — Channel and existing code unchanged

class AgentBus {
public:
    // ... existing API unchanged ...

    // New: register a remote agent transport (protected by mu_)
    Result<void> register_remote(AgentId id, std::unique_ptr<Transport> transport);

    // New: unregister remote transport
    void unregister_remote(AgentId id);

private:
    // ... existing members unchanged ...

    // New: remote agent transports, protected by mu_ (same mutex as channels_)
    std::unordered_map<AgentId, std::unique_ptr<Transport>> remote_transports_;

    // Modified: send() routing logic
    // if target in channels_ → channel->push() (existing path)
    // if target in remote_transports_ → transport->send() (new path)
    // else → ErrorCode::NotFound
};
```

**Locking strategy:** `remote_transports_` is protected by the existing `mu_` mutex, same as `channels_`. No new mutexes introduced. The `send()` method holds `mu_` to find the target, then releases before calling `transport->send()` (which may block on socket I/O) to avoid holding the bus lock during I/O.

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
    // Dead-process detection latency = heartbeat_interval * max_missed_heartbeats (default 9s)
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

    // Lookup by agent ID (for destroy_agent flow)
    std::optional<ProcessInfo> find_by_agent(AgentId id) const;

    void start_monitor();
    void stop_monitor();

    using DeathCallback = std::function<void(ProcessInfo)>;
    void on_death(DeathCallback cb);

    // Log forwarding: sub-process stderr is captured and forwarded to orchestrator LOG_*
    using LogCallback = std::function<void(pid_t, std::string_view)>;
    void on_log(LogCallback cb);

private:
    ProcessConfig cfg_;
    mutable std::mutex mu_;
    std::unordered_map<pid_t, ProcessInfo> processes_;
    std::jthread monitor_thread_;
    DeathCallback death_cb_;
    LogCallback log_cb_;

    void monitor_loop(std::stop_token st);
};
```

### Sub-Agent Process Bootstrap & Serialization

```cpp
// AgentConfig JSON serialization — new methods added to AgentConfig

struct AgentConfig {
    // ... existing fields + new isolation fields ...

    nlohmann::json to_json() const;
    static AgentConfig from_json(const nlohmann::json& j);
};

struct SubAgentBootstrap {
    std::string worktree_name;
    std::filesystem::path worktree_path;
    std::filesystem::path socket_path;
    AgentConfig agent_cfg;
    std::string llm_backend_json;    // LLMKernel config serialized via kernel->to_json()

    nlohmann::json to_json() const;
    static SubAgentBootstrap from_json(const nlohmann::json& j);
};

// Sub-process entry point
int subagent_main(SubAgentBootstrap boot);
```

**CLI invocation:** The orchestrator passes the bootstrap config as a JSON file (not CLI args, to avoid shell escaping issues):

```
fork+exec(argv: --subagent --config /tmp/agentos-boot-<pid>.json)
```

The JSON file is written atomically (temp+rename, existing pattern) before fork, and deleted by the sub-process after reading.

### Handshake Protocol

After the sub-process connects to the Unix socket, it must complete a handshake within `startup_timeout`:

```cpp
// 1. Sub-process sends handshake request
BusMessage handshake_req {
    .type = MessageType::Request,
    .from = agent_id,
    .to = 0,                        // 0 = orchestrator
    .topic = "worktree.handshake",
    .payload = json{
        {"agent_id", agent_id},
        {"worktree_name", worktree_name},
        {"protocol_version", 1}
    }.dump()
};

// 2. Orchestrator validates agent_id matches expected, responds
BusMessage handshake_ack {
    .type = MessageType::Response,
    .from = 0,
    .to = agent_id,
    .topic = "worktree.handshake",
    .reply_to = handshake_req.id,
    .payload = json{{"status", "ok"}}.dump()
};

// 3. If handshake fails (wrong agent_id, timeout, protocol mismatch):
//    → Orchestrator closes socket
//    → ProcessManager marks process as Failed
//    → Returns ErrorCode::HandshakeFailed
```

### Startup Sequence

```
Orchestrator                          SubAgent Process
    |                                       |
    +-- write /tmp/agentos-boot-<>.json     |
    +-- fork+exec(--subagent --config <f>)  |
    +-- listen(socket_path)                 |
    |                                       |
    |       +-------------------------------+
    |       | 1. read + delete config file  |
    |       | 2. chdir(worktree_path)       |
    |       | 3. init slim AgentOS          |
    |       |    (Kernel + ToolManager      |
    |       |     + ContextManager)         |
    |       | 4. connect(socket_path)       |
    |       +--- Handshake(agent_id) ------>|
    |       |         validate + ACK        |
    |<------+                               |
    |       | 5. enter agent.run() loop     |
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
- **Config via temp file** — avoids CLI arg length limits and shell escaping issues
- **Log forwarding** — sub-process stderr is captured by orchestrator for debugging

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

    // New: JSON serialization for cross-process bootstrap
    nlohmann::json to_json() const;
    static AgentConfig from_json(const nlohmann::json& j);
};
```

### AgentOS Extensions

**Key: existing `create_agent()` signature is preserved.** A new separate method `create_isolated_agent()` handles worktree creation. The existing `create_agent<AgentT>(cfg, args...)` returning `shared_ptr<AgentT>` is unchanged — no breaking API change.

```cpp
class AgentOS {
public:
    // ... existing API unchanged, including:
    // template<AgentConcept AgentT = ReActAgent, typename... Args>
    // std::shared_ptr<AgentT> create_agent(AgentConfig cfg, Args&&... args);

    // New: worktree-isolated agent creation
    // Returns AgentId (not shared_ptr) because the agent may be in another process
    Result<AgentId> create_isolated_agent(AgentConfig cfg);

    // New: run an isolated agent and wait for result
    Result<std::string> run_isolated(AgentId id, const std::string& input);

    // New: accessors
    WorktreeManager& worktree_mgr();
    ProcessManager& process_mgr();

private:
    std::unique_ptr<worktree::WorktreeManager> worktree_mgr_;
    std::unique_ptr<worktree::ProcessManager> process_mgr_;

    Result<AgentId> create_isolated_agent_impl(AgentConfig cfg) {
        // 1. Allocate agent ID first (needed for RBAC check)
        auto id = next_agent_id_++;

        // 2. Grant temporary role for RBAC check
        if (security_) {
            security_->grant(id, cfg.security_role);

            // 3. RBAC check using AgentId (matches existing RBAC::check signature)
            auto perm_result = security_->check(id, Permission::WorktreeCreate);
            if (!perm_result) return perm_result.error();
        }

        // 4. Create worktree
        auto name = cfg.worktree_name.value_or(generate_worktree_name(id, cfg));
        auto wt_result = worktree_mgr_->create(name, cfg.base_branch);
        if (!wt_result) return wt_result.error();
        auto& wt = *wt_result;

        // 5. Same-process or independent process
        if (needs_process_isolation(cfg)) {
            if (security_) {
                auto proc_perm = security_->check(id, Permission::WorktreeProcess);
                if (!proc_perm) return proc_perm.error();
            }
            auto proc_result = process_mgr_->spawn(name, wt.path, id, cfg);
            if (!proc_result) return proc_result.error();
            // Sub-process connects via socket; AgentBus registers remote transport
        } else {
            // Same-process worktree: create agent with working_dir bound to worktree
            cfg.working_dir = wt.path;
            create_agent(std::move(cfg));  // existing method
        }

        return id;
    }

    std::string generate_worktree_name(AgentId id, const AgentConfig& cfg) {
        return fmt::format("agent-{}-{:04x}", cfg.name, id & 0xFFFF);
    }
};
```

### Scheduler Extensions

```cpp
struct AgentTaskDescriptor {
    // ... existing fields ...
    std::optional<std::filesystem::path> working_dir;
    IsolationMode isolation{IsolationMode::Thread};
};
```

### SupervisorAgent Integration

**Key: `WorkerEntry` struct is unchanged.** Instead, a new `WorkerTemplate` is added for worktree-isolated workers that are created on-demand (not pre-instantiated).

```cpp
// New struct for on-demand worker creation
struct WorkerTemplate {
    AgentConfig config;
    std::string description;
    size_t max_calls{5};
};

class SupervisorAgent : public AgentBase<SupervisorAgent> {
public:
    // Existing: add pre-instantiated worker (unchanged)
    SupervisorAgent& add_worker(std::shared_ptr<Agent> worker,
                                std::string description,
                                size_t max_calls = 5);

    // New: add worker template for worktree-isolated on-demand creation
    SupervisorAgent& add_worker_template(std::string name,
                                          AgentConfig config,
                                          std::string description,
                                          size_t max_calls = 5);

private:
    std::unordered_map<std::string, WorkerEntry> workers_;           // existing
    std::unordered_map<std::string, WorkerTemplate> worker_templates_; // new

    // dispatch_worker checks both maps:
    // - If found in workers_ → existing in-process delegation
    // - If found in worker_templates_ → create_isolated_agent, run, destroy
};
```

### Builder API

```cpp
auto os = AgentOSBuilder()
    .openai("sk-...", "gpt-4o")
    .threads(4)
    .repo_root("/path/to/project")          // new
    .max_worktrees(10)                      // new
    .build();

// Option 1: fluent builder for worktree agent
auto id_result = os->create_isolated_agent(
    AgentConfig{
        .name = "refactor-bot",
        .role_prompt = "Refactor the database layer",
        .isolation = IsolationMode::Worktree,
        .base_branch = "main"
    });

// Option 2: via SupervisorAgent template
supervisor->add_worker_template("refactor-bot",
    AgentConfig{
        .name = "refactor-bot",
        .role_prompt = "Refactor the database layer",
        .isolation = IsolationMode::Worktree
    },
    "Refactors code in isolated worktree");
```

---

## 7. Security, Cleanup & Error Handling

### RBAC Extensions

```cpp
enum class Permission : uint32_t {
    // ... existing bits 0-9 ...
    // Worktree permissions — contiguous with existing bits
    WorktreeCreate   = 1 << 10,   // 0x00000400 — create worktree (same-process)
    WorktreeProcess  = 1 << 11,   // 0x00000800 — create independent process worktree
    WorktreeForce    = 1 << 12,   // 0x00001000 — force-delete worktree with changes
    // Admin
    Admin            = 0xFFFFFFFF,
};
```

Updated role presets:

```cpp
static Role make_standard() {
    return {"standard",
            Permission::ToolReadOnly | Permission::ToolWrite |
            Permission::MemoryRead  | Permission::MemoryWrite |
            Permission::AgentCreate | Permission::AgentObserve |
            Permission::WorktreeCreate};  // can create same-process worktrees
}
static Role make_privileged() {
    return {"privileged",
            Permission::ToolReadOnly | Permission::ToolWrite |
            Permission::ToolDangerous |
            Permission::MemoryRead   | Permission::MemoryWrite |
            Permission::MemoryDelete |
            Permission::AgentCreate  | Permission::AgentKill |
            Permission::AgentObserve |
            Permission::WorktreeCreate | Permission::WorktreeProcess};  // can spawn processes
}
```

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
void AgentOS::destroy_agent(AgentId id) {
    // 1. Existing cleanup: context, bus channel, agent object (current code)

    // 2. If cross-process agent → shutdown sub-process
    if (auto proc = process_mgr_->find_by_agent(id)) {
        process_mgr_->shutdown(proc->pid);
        bus_->unregister_remote(id);
    }

    // 3. If worktree bound → execute auto-cleanup
    if (auto wt = worktree_mgr_->find_by_agent(id)) {
        if (wt->auto_cleanup) {
            worktree_mgr_->auto_cleanup(wt->name);
        }
    }
}
```

### Error Scenarios

| Scenario | Behavior |
|----------|----------|
| Sub-process crash | Preserve worktree (state=Failed), publish `agent.died` on bus, forward last stderr |
| Worktree creation failure (disk full, git error) | Return `WorktreeError`, caller decides fallback |
| Capacity exceeded | Return `WorktreeCapacityFull`, caller can queue or fallback to Thread mode |
| Socket disconnect | Mark agent unreachable, treat as process death |
| Handshake timeout | Kill process, return `HandshakeFailed` |
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
    worktree_manager.hpp         # WorktreeManager (incl. recover())
    process_manager.hpp          # ProcessManager (incl. find_by_agent, log forwarding)
  bus/
    transport.hpp                # Transport interface
    inproc_transport.hpp         # InProcTransport
    unix_socket_transport.hpp    # UnixSocketTransport

src/
  worktree/
    worktree_manager.cpp         # git command execution, state machine, recovery
    process_manager.cpp          # fork+exec, waitpid, heartbeat monitor, stderr capture
  bus/
    inproc_transport.cpp
    unix_socket_transport.cpp    # socket creation, frame I/O
  main_subagent.cpp              # sub-process entry point (subagent_main)

tests/
  test_worktree_manager.cpp      # CRUD, capacity, auto cleanup, recovery
  test_process_manager.cpp       # spawn/shutdown, heartbeat, crash callback, handshake
  test_transport.cpp             # InProc + UnixSocket round-trip
  test_worktree_integration.cpp  # end-to-end lifecycle
```

### Modified Files

| File | Change | Scope |
|------|--------|-------|
| `include/agentos/agent.hpp` | `AgentConfig` + isolation fields + `to_json()`/`from_json()`; `AgentOS` + `create_isolated_agent()`, `worktree_mgr_`, `process_mgr_` | Medium |
| `include/agentos/agentos.hpp` | Builder: `.repo_root()`, `.max_worktrees()` | Small |
| `include/agentos/bus/agent_bus.hpp` | Add `remote_transports_` map, `register_remote()`, `unregister_remote()`; modify `send()` routing | Small |
| `include/agentos/security/security.hpp` | New permission bits (10-12), update `make_standard()`/`make_privileged()` | Small |
| `include/agentos/supervisor_agent.hpp` | Add `WorkerTemplate`, `add_worker_template()`, `worker_templates_` map | Small |
| `include/agentos/scheduler/scheduler.hpp` | `AgentTaskDescriptor` + `working_dir`/`isolation` | Minimal |
| `include/agentos/core/types.hpp` | New error codes: `WorktreeError`, `WorktreeCapacityFull`, `ProcessSpawnFailed`, `ProcessDied`, `HandshakeFailed` | Minimal |

### External Dependencies

None added. Uses:
- Git CLI via `popen`/`exec` (existing `PipeGuard` RAII)
- POSIX Unix domain sockets (`sys/socket.h`, `sys/un.h`)
- POSIX process management (`unistd.h`, `sys/wait.h`)
- nlohmann::json for BusMessage + AgentConfig serialization (existing)
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

---

## Appendix: Review Issue Resolution

Issues identified by spec review and how they were addressed:

1. **`create_agent` signature mismatch** — Resolved: new `create_isolated_agent()` method instead of overloading existing `create_agent()`. No breaking change.
2. **RBAC `check()` uses wrong API** — Resolved: allocate `AgentId` first, grant role, then call `check(AgentId, Permission)` matching existing RBAC signature.
3. **Permission bit collision** — Resolved: use bits 10-12 (contiguous with existing 0-9), not 12-14. Updated `make_standard()`/`make_privileged()` shown explicitly.
4. **AgentBus dual-mutex deadlock** — Resolved: `remote_transports_` protected by existing `mu_`. Send releases lock before I/O. No new mutexes.
5. **SupervisorAgent `WorkerEntry` incompatibility** — Resolved: new `WorkerTemplate` struct and `add_worker_template()` method. Existing `WorkerEntry` unchanged.
6. **`TRY`/`TRY_DECL` macros undefined** — Resolved: pseudocode rewritten using explicit `if (!result)` error handling matching existing codebase pattern.
7. **`pid_t` POSIX-only** — Resolved: platform constraint explicitly stated in spec header and Section 2.
8. **`SubAgentBootstrap` serialization unspecified** — Resolved: `to_json()`/`from_json()` added to `AgentConfig` and `SubAgentBootstrap`. Config passed via temp JSON file.
9. **Double-timeout issue** — Resolved: `Channel` not wrapped in `Transport`. Separate routing maps in `AgentBus`. `InProcTransport::recv()` delegates directly to `Channel::recv()` with single timeout.
10. **Error codes incomplete** — Resolved: Section 2 now enumerates all 5 new error codes with usage context.
11. **Handshake protocol underspecified** — Resolved: Section 5 now specifies full handshake message format, validation, and failure handling.
12. **Crash recovery** — Resolved: `WorktreeManager::recover()` scans disk on startup.
13. **Sub-agent log forwarding** — Resolved: `ProcessManager::on_log()` callback for stderr capture.
14. **Heartbeat detection latency** — Resolved: documented as `heartbeat_interval * max_missed_heartbeats` in `ProcessConfig`.
