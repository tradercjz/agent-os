# P0 Infrastructure Improvements Design Spec

**Date:** 2026-03-21
**Batch:** P0 — Infrastructure Foundation
**Scope:** Structured Logging, Audit Log Persistence, Hot Config Reload, vcpkg Package Management

---

## 1. Structured Logging (Dual-Sink)

### Current State

`Logger` (singleton, `include/agentos/core/logger.hpp`) uses a single `LogSink = std::function<void(LogLevel, string_view)>` callback. Output is human-readable text to stderr. Already has async batched worker thread, `LogField` KV support, and `log_kv()` method.

### Design

**Goal:** Console stays human-readable; a parallel JSON Lines file sink captures machine-parseable logs for ELK/Loki ingestion.

#### 1.1 ILogSink Interface

```cpp
// include/agentos/core/log_sink.hpp
#include <span>
namespace agentos {

struct LogEvent {
    LogLevel           level;
    std::string        timestamp;   // UTC ISO-8601 "YYYY-MM-DDTHH:MM:SS.mmmZ"
    std::string        filename;
    int                line;
    std::string        msg;
    std::vector<LogField> fields;   // structured KV pairs
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogEvent& event) = 0;
    virtual void flush() {}
};

} // namespace agentos
```

**Timestamp change:** `prepare_event()` will switch from local time to UTC ISO-8601 (`2026-03-21T14:00:01.234Z`). `ConsoleSink` uses this format directly (consistent with structured logging best practices). This is a visible behavioral change in console output.

#### 1.2 ConsoleSink

Default sink. Formats as current human-readable style:
```
[2026-03-21 14:00:01.234] [INFO ] logger.hpp:42  Message key1=val1
```
Output to `stderr` via `std::fputs`.

#### 1.3 JsonFileSink

Writes JSON Lines (one JSON object per line) to a configured file path:
```json
{"ts":"2026-03-21T14:00:01.234Z","level":"INFO","file":"logger.hpp","line":42,"msg":"Message","extra":{"key1":"val1"}}
```

Note: `agent_id` is not a dedicated `LogEvent` field — it comes from `fields` KV pairs when callers use `LOG_INFO_KV("msg", KV("agent_id", id))`. The JSON sink flattens all `fields` into the `extra` object.

Features:
- Atomic open with append mode
- Configurable rotation: by file size (default 50MB) or by day
- On rotation: write to temp file, check `ofs.good()`, then rename (consistent with project disk-full safety pattern). If rename fails (target exists), append timestamp suffix to avoid collision.
- `flush()` calls `fflush` on the underlying file

#### 1.4 Logger Refactor

```cpp
class Logger {
public:
    void add_sink(std::shared_ptr<ILogSink> sink);
    void clear_sinks();  // for testing
    // ...existing log/log_kv/flush methods unchanged...
private:
    std::vector<std::shared_ptr<ILogSink>> sinks_;  // replaces custom_sink_
    std::shared_mutex sinks_mu_;                     // separate from queue mu_
    // worker_loop broadcasts each LogEvent to all sinks
};
```

**Thread safety for multi-sink:** Use `std::shared_ptr` (not `unique_ptr`) for sinks to enable safe snapshotting. Worker loop takes a shared lock on `sinks_mu_` to snapshot the sink vector, releases the lock, then iterates and calls `sink->write()` outside the lock. `add_sink()`/`clear_sinks()` take an exclusive lock on `sinks_mu_`. This avoids blocking log producers during slow sink I/O (e.g., JsonFileSink rotation), matching the existing `sink_copy` pattern.

- `LogEvent` struct promoted from private inner class to public (in `log_sink.hpp`)
- `prepare_event()` returns `LogEvent`, worker loop calls `sink->write(event)` for each sink
- Remove old `set_sink(LogSink)` (deprecated, keep as adapter for one release)
- `LOG_*` macros unchanged — zero API breakage for callers

#### 1.5 Files

| File | Action |
|------|--------|
| `include/agentos/core/log_sink.hpp` | **New** — `ILogSink`, `LogEvent`, `ConsoleSink`, `JsonFileSink` |
| `include/agentos/core/logger.hpp` | **Modify** — multi-sink vector, deprecate `set_sink` |
| `tests/test_logger.cpp` | **Modify** — add tests for dual-sink, JSON format, rotation |

---

## 2. Audit Log Persistence

### Current State

`AgentBus` (`include/agentos/bus/agent_bus.hpp`) stores audit trail in `std::deque<BusMessage> audit_trail_` with 10k cap. All in-memory, lost on restart.

### Design

**Goal:** Pluggable `IAuditStore` interface with SQLite default implementation. AgentBus delegates persistence to the store.

#### 2.1 IAuditStore Interface

```cpp
// include/agentos/bus/audit_store.hpp
#include <span>
#include <chrono>
namespace agentos::bus {

// Use system_clock (wall clock) for audit timestamps — NOT steady_clock.
// steady_clock is monotonic but meaningless across restarts and cannot be
// serialized as a human-readable timestamp.
using WallTimePoint = std::chrono::system_clock::time_point;

struct AuditEntry {
    uint64_t      id;
    WallTimePoint timestamp;
    AgentId       from_agent;
    AgentId       to_agent;
    MessageType   type;
    std::string   topic;
    std::string   payload;
    bool          redacted;
};

struct AuditFilter {
    std::optional<AgentId>       agent_id;      // from OR to
    std::optional<MessageType>   type;
    std::optional<WallTimePoint> after;
    std::optional<WallTimePoint> before;
    std::optional<std::string>   topic;
    size_t                       limit{100};
    size_t                       offset{0};
};

struct RotationPolicy {
    size_t max_entries{100000};          // 0 = unlimited
    std::chrono::hours max_age{24*30};   // default 30 days
};

class IAuditStore {
public:
    virtual ~IAuditStore() = default;
    virtual Result<void> write(const AuditEntry& entry) = 0;
    virtual Result<void> write_batch(std::span<const AuditEntry> entries) = 0;
    virtual std::vector<AuditEntry> query(const AuditFilter& filter) = 0;
    virtual size_t count(const AuditFilter& filter) = 0;
    virtual Result<void> rotate(const RotationPolicy& policy) = 0;
    virtual void flush() {}
};

} // namespace agentos::bus
```

**Key design decisions:**
- `WallTimePoint` (system_clock) instead of `TimePoint` (steady_clock) — critical for persistence across restarts and human-readable serialization.
- `write`/`write_batch`/`rotate` return `Result<void>` — SQLite operations can fail (disk full, corruption). Follows project `[[nodiscard]]` Expected convention.
- `#include <span>` explicitly included for `write_batch` parameter.

#### 2.2 SqliteAuditStore

```cpp
// include/agentos/bus/sqlite_audit_store.hpp
namespace agentos::bus {

// RAII wrapper for sqlite3 handle (project convention: CurlGuard, PipeGuard)
struct SqliteGuard {
    sqlite3* db = nullptr;
    SqliteGuard() = default;
    explicit SqliteGuard(const std::string& path) {
        sqlite3_open(path.c_str(), &db);
    }
    ~SqliteGuard() { if (db) sqlite3_close(db); }
    SqliteGuard(const SqliteGuard&) = delete;
    SqliteGuard& operator=(const SqliteGuard&) = delete;
};

class SqliteAuditStore : public IAuditStore, private NonCopyable {
public:
    explicit SqliteAuditStore(const std::string& db_path);
    // Implements all IAuditStore methods
    // Table schema:
    //   CREATE TABLE IF NOT EXISTS audit_log (
    //     id INTEGER PRIMARY KEY,
    //     timestamp TEXT NOT NULL,
    //     from_agent INTEGER,
    //     to_agent INTEGER,
    //     type TEXT NOT NULL,
    //     topic TEXT,
    //     payload TEXT,
    //     redacted INTEGER DEFAULT 0
    //   );
    //   CREATE INDEX idx_audit_ts ON audit_log(timestamp);
    //   CREATE INDEX idx_audit_agent ON audit_log(from_agent, to_agent);
    //   CREATE INDEX idx_audit_type_ts ON audit_log(type, timestamp);
private:
    SqliteGuard db_;
};

} // namespace agentos::bus
```

Features:
- WAL mode for concurrent read/write
- Batch insert via transaction (write_batch)
- `rotate()` deletes rows by age or count via `DELETE FROM audit_log WHERE ...`
- Rotation is caller-driven: AgentOS wires a periodic timer (or on `write()` every N entries) to call `store->rotate(policy)`. Not auto-triggered inside the store.
- RAII `SqliteGuard` for handle lifecycle (project convention)
- `NonCopyable` to prevent accidental copy of raw DB handle
- Additional index on `(type, timestamp)` for common "show all Events in last hour" queries

#### 2.3 AgentBus Integration

```cpp
class AgentBus {
public:
    explicit AgentBus(security::SecurityManager* sec = nullptr,
                      std::shared_ptr<IAuditStore> store = nullptr);
    // ...
private:
    void audit_push(const BusMessage& msg) {
        // 1. In-memory trail + monitors (under mu_, fast)
        audit_trail_.push_back(msg);
        while (audit_trail_.size() > 10000) audit_trail_.pop_front();
        for (auto& m : monitors_) m(msg);

        // 2. Persistent store write OUTSIDE mu_ lock to avoid blocking
        //    send()/publish() during slow disk I/O.
        //    Build AuditEntry under lock, then write after releasing.
        if (store_) {
            auto entry = to_audit_entry(msg);  // pure conversion, no I/O
            // mu_ already released by caller pattern (see below)
            (void)store_->write(entry);
        }
    }

    // Callers (send/publish) restructured:
    //   {lock_guard lk(mu_); ...; audit_push_mem(msg); }  // in-memory only
    //   audit_push_store(msg);  // persistent store, outside lock
    std::shared_ptr<IAuditStore> store_;
};
```

**Performance note:** `store_->write()` (SQLite disk I/O) runs outside `mu_` to avoid blocking all bus traffic. The in-memory trail and monitor dispatch remain under the lock for consistency. `send()`/`publish()` are restructured to split the lock scope: in-memory operations under lock, store write after lock release.

#### 2.4 Files

| File | Action |
|------|--------|
| `include/agentos/bus/audit_store.hpp` | **New** — `IAuditStore`, `AuditEntry`, `AuditFilter`, `RotationPolicy` |
| `include/agentos/bus/sqlite_audit_store.hpp` | **New** — SQLite implementation |
| `src/bus/sqlite_audit_store.cpp` | **New** — SQLite implementation |
| `include/agentos/bus/agent_bus.hpp` | **Modify** — accept `IAuditStore`, dual write |
| `tests/test_audit_store.cpp` | **New** — store CRUD, rotation, query tests |
| `tests/test_bus.cpp` | **Modify** — test bus with persistent store |

---

## 3. Hot Config Reload

### Current State

Configuration is set at build time via `AgentOSBuilder` and immutable at runtime. Changing TPM limits, log level, or security thresholds requires restart.

### Design

**Goal:** Runtime-safe parameter reload via file watch + API, limited to parameters that don't require component reconstruction.

#### 3.1 Hot-Reloadable Parameters

| Parameter | Type | Module |
|-----------|------|--------|
| `tpm_limit` | `size_t` | Kernel (TokenBucketRateLimiter) |
| `log_level` | `LogLevel` | Logger |
| `injection_detection_enabled` | `bool` | Security (InjectionDetector) |
| `injection_threshold` | `double` | Security |
| `audit_rotation_max_entries` | `size_t` | Bus (IAuditStore) |
| `audit_rotation_max_age_hours` | `size_t` | Bus |
| `context_default_token_limit` | `size_t` | Context |

Parameters NOT hot-reloadable (require restart): thread pool size, LLM backend, data directory, RBAC role definitions.

#### 3.2 HotConfig Class

```cpp
// include/agentos/core/hot_config.hpp
namespace agentos {

using ConfigChangeCallback = std::function<void(const std::string& key, const nlohmann::json& value)>;

class HotConfig : private NonCopyable {
public:
    explicit HotConfig(const std::string& config_path = "");

    // Get current value (thread-safe shared read)
    template<typename T>
    T get(const std::string& key, const T& default_val) const;

    // Programmatic update (validated, then applied)
    Result<void> set(const std::string& key, const nlohmann::json& value);

    // Reload from file (all-or-nothing: validates entire file before applying)
    Result<void> reload();

    // Register change observer
    void on_change(const std::string& key, ConfigChangeCallback cb);

    // Start file watcher (kqueue/inotify)
    void start_watching();
    void stop_watching();

private:
    nlohmann::json config_;
    mutable std::shared_mutex mu_;
    std::string config_path_;
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> observers_;
    std::jthread watcher_thread_;

    // Validate a single key-value pair (type check, range check)
    Result<void> validate(const std::string& key, const nlohmann::json& value) const;

    // Collect changes under lock, notify OUTSIDE lock to prevent deadlock.
    // Pattern: snapshot changed keys + callbacks, release mu_, then invoke.
    void apply_and_notify(const nlohmann::json& new_config);

    void file_watch_loop(std::stop_token st);  // platform-specific
};

} // namespace agentos
```

**Deadlock prevention:** `apply_and_notify()` acquires exclusive `mu_` to diff old vs new config and snapshot the affected callbacks, then releases `mu_` before invoking any callbacks. This prevents deadlock when observer callbacks call `get()` (which takes shared `mu_`).

**Validation:** `reload()` parses the file, validates ALL keys against known schemas (type + range), and only applies if all pass (all-or-nothing). Returns `Result<void>` with error details on failure. `set()` also validates before applying.

**Atomicity:** Multi-key updates from file reload are applied atomically — config JSON is swapped in one step under exclusive lock, then all observers are notified sequentially outside the lock. Observers always see a consistent snapshot.

#### 3.3 File Watch Implementation

```cpp
// src/core/hot_config.cpp
// Platform abstraction:
//   macOS: kqueue + EVFILT_VNODE (NOTE_WRITE)
//   Linux: inotify (IN_MODIFY)
// Falls back to polling (stat every 2s) if neither available
//
// Debounce: after receiving a file change event, wait 200ms before reload.
// Editors often write files in multiple steps (truncate + write, or
// write temp + rename), so reading immediately may catch a truncated file.
// The debounce window coalesces rapid events into a single reload.
```

#### 3.4 Integration with AgentOS

```cpp
// In AgentOSBuilder:
auto os = AgentOSBuilder()
    .openai(...)
    .config_file("agentos.json")  // enables hot reload
    .build();

// AgentOS holds HotConfig, wires observers to subsystems:
// hot_config_.on_change("tpm_limit", [this](auto&, auto& v) {
//     kernel_.rate_limiter().set_rate(v.get<size_t>());
// });
```

#### 3.5 MCP Integration

Add `config/reload`, `config/get`, and `config/set` methods to `MCPServer`:
```json
{"jsonrpc":"2.0","method":"config/reload","id":1}
{"jsonrpc":"2.0","method":"config/get","params":{"key":"tpm_limit"},"id":2}
{"jsonrpc":"2.0","method":"config/set","params":{"key":"tpm_limit","value":200000},"id":3}
```

`config/set` exposes `HotConfig::set()` via MCP. Validation runs server-side; invalid values return a JSON-RPC error response.

#### 3.6 Config File Format

```json
{
  "tpm_limit": 100000,
  "log_level": "info",
  "injection_detection_enabled": true,
  "injection_threshold": 0.7,
  "audit_rotation_max_entries": 100000,
  "audit_rotation_max_age_hours": 720,
  "context_default_token_limit": 8192
}
```

#### 3.7 Files

| File | Action |
|------|--------|
| `include/agentos/core/hot_config.hpp` | **New** — HotConfig class |
| `src/core/hot_config.cpp` | **New** — file watch + reload logic |
| `include/agentos/mcp/mcp_server.hpp` | **Modify** — add config/* methods |
| `include/agentos/agentos.hpp` | **Modify** — wire HotConfig to builder + subsystems |
| `tests/test_hot_config.cpp` | **New** — reload, observers, thread safety |

---

## 4. vcpkg Package Management

### Current State

Dependencies managed via mixed `FetchContent` (nlohmann_json, googletest, hnswlib, cppjieba) and `find_package` / `pkg_check_modules` (curl, sqlite3, duckdb). No package manifest.

### Design

**Goal:** Add vcpkg manifest for reproducible builds. Keep FetchContent as fallback for users who don't use vcpkg.

#### 4.1 vcpkg.json

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "agentos",
  "version": "0.1.0",
  "description": "C++23 LLM Agent Operating System",
  "builtin-baseline": "<pin to a recent vcpkg commit hash at implementation time>",
  "dependencies": [
    "nlohmann-json",
    "curl",
    "sqlite3",
    "gtest",
    "hnswlib"
  ],
  "features": {
    "duckdb": {
      "description": "DuckDB columnar storage backend",
      "dependencies": ["duckdb"]
    }
  }
}
```

**Registry availability notes:**
- `nlohmann-json`, `curl`, `sqlite3`, `gtest`, `duckdb` are all available in the official vcpkg registry.
- `hnswlib` is available in vcpkg as `hnswlib`.
- `cppjieba` is NOT in the official vcpkg registry — it stays as FetchContent-only (no vcpkg feature). The `jieba` feature is removed from vcpkg.json.
- `builtin-baseline` must be pinned to a specific vcpkg commit hash for reproducible version resolution.

#### 4.2 CMakePresets.json

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "vcpkg",
      "displayName": "vcpkg Build",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_CXX_STANDARD": "23"
      }
    },
    {
      "name": "vcpkg-release",
      "inherits": "vcpkg",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    },
    {
      "name": "default",
      "displayName": "FetchContent Build (no vcpkg)",
      "cacheVariables": {
        "CMAKE_CXX_STANDARD": "23",
        "CMAKE_BUILD_TYPE": "RelWithDebInfo"
      }
    }
  ]
}
```

#### 4.3 CMakeLists.txt Changes

Wrap FetchContent blocks in `if(NOT TARGET ...)` guards:

```cmake
# ---- nlohmann/json ----
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
```

Same pattern for googletest, hnswlib, cppjieba.

**sqlite3 handling:** vcpkg provides `unofficial-sqlite3` via `find_package(unofficial-sqlite3 CONFIG)`, while non-vcpkg builds use `pkg_check_modules(SQLITE3 REQUIRED sqlite3)`. Use a try-find_package-first pattern:

```cmake
# ---- SQLite3 ----
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
endif()
# Link: use unofficial::sqlite3::sqlite3 if available, else ${SQLITE3_LIBRARIES}
```

#### 4.4 Files

| File | Action |
|------|--------|
| `vcpkg.json` | **New** — package manifest |
| `CMakePresets.json` | **New** — vcpkg + default presets |
| `CMakeLists.txt` | **Modify** — add `if(NOT TARGET)` guards around FetchContent |

---

## Summary of All New/Modified Files

### New Files (8)
1. `include/agentos/core/log_sink.hpp`
2. `include/agentos/bus/audit_store.hpp`
3. `include/agentos/bus/sqlite_audit_store.hpp`
4. `src/bus/sqlite_audit_store.cpp`
5. `include/agentos/core/hot_config.hpp`
6. `src/core/hot_config.cpp`
7. `vcpkg.json`
8. `CMakePresets.json`

### New Test Files (3)
9. `tests/test_audit_store.cpp`
10. `tests/test_hot_config.cpp`
11. `tests/test_log_sink.cpp` — JsonFileSink rotation edge cases, concurrent sink writes

### Modified Files (7)
11. `include/agentos/core/logger.hpp` — multi-sink
12. `include/agentos/bus/agent_bus.hpp` — IAuditStore injection
13. `include/agentos/mcp/mcp_server.hpp` — config/* methods
14. `include/agentos/agentos.hpp` — HotConfig + config_file builder
15. `CMakeLists.txt` — vcpkg guards + new sources
16. `tests/test_logger.cpp` — dual-sink tests
17. `tests/test_bus.cpp` — persistent store tests

### Backward Compatibility
- All `LOG_*` macros unchanged
- `AgentBus` constructor gains optional `IAuditStore` param (default nullptr = current behavior)
- `AgentOSBuilder` gains optional `.config_file()` (omit = no hot reload)
- FetchContent still works without vcpkg
- `set_sink()` deprecated but functional via adapter

### Testing Strategy
- Unit tests per new component (ILogSink, IAuditStore, HotConfig)
- `test_log_sink.cpp`: JsonFileSink rotation (size-based, day-based), rename failure recovery, concurrent multi-sink writes
- `test_audit_store.cpp`: CRUD, batch write, query with filters, rotation by age/count, WAL concurrency
- `test_hot_config.cpp`: reload validation (reject invalid values), observer notification order, deadlock-free concurrent get+set, file watch debounce
- Integration: AgentOS with all P0 features enabled end-to-end
- Thread safety: concurrent config reload + log write + audit write
- All existing 719 tests must continue to pass
