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
namespace agentos {

struct LogEvent {
    LogLevel           level;
    std::string        timestamp;   // ISO-8601 with ms
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

#### 1.2 ConsoleSink

Default sink. Formats as current human-readable style:
```
[2026-03-21 14:00:01.234] [INFO ] logger.hpp:42  Message key1=val1
```
Output to `stderr` via `std::fputs`.

#### 1.3 JsonFileSink

Writes JSON Lines (one JSON object per line) to a configured file path:
```json
{"ts":"2026-03-21T14:00:01.234Z","level":"INFO","file":"logger.hpp","line":42,"msg":"Message","agent_id":"a1","extra":{"key1":"val1"}}
```

Features:
- Atomic open with append mode
- Configurable rotation: by file size (default 50MB) or by day
- On rotation: rename current file to `<name>.<date>.jsonl`, open new file
- `flush()` calls `fflush` on the underlying file

#### 1.4 Logger Refactor

```cpp
class Logger {
public:
    void add_sink(std::unique_ptr<ILogSink> sink);
    void clear_sinks();  // for testing
    // ...existing log/log_kv/flush methods unchanged...
private:
    std::vector<std::unique_ptr<ILogSink>> sinks_;  // replaces custom_sink_
    // worker_loop broadcasts each LogEvent to all sinks
};
```

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
namespace agentos::bus {

struct AuditEntry {
    uint64_t    id;
    TimePoint   timestamp;
    AgentId     from_agent;
    AgentId     to_agent;
    MessageType type;
    std::string topic;
    std::string payload;
    bool        redacted;
};

struct AuditFilter {
    std::optional<AgentId>     agent_id;      // from OR to
    std::optional<MessageType> type;
    std::optional<TimePoint>   after;
    std::optional<TimePoint>   before;
    std::optional<std::string> topic;
    size_t                     limit{100};
    size_t                     offset{0};
};

struct RotationPolicy {
    size_t max_entries{100000};          // 0 = unlimited
    std::chrono::hours max_age{24*30};   // default 30 days
};

class IAuditStore {
public:
    virtual ~IAuditStore() = default;
    virtual void write(const AuditEntry& entry) = 0;
    virtual void write_batch(std::span<const AuditEntry> entries) = 0;
    virtual std::vector<AuditEntry> query(const AuditFilter& filter) = 0;
    virtual size_t count(const AuditFilter& filter) = 0;
    virtual void rotate(const RotationPolicy& policy) = 0;
    virtual void flush() {}
};

} // namespace agentos::bus
```

#### 2.2 SqliteAuditStore

```cpp
// include/agentos/bus/sqlite_audit_store.hpp
namespace agentos::bus {

class SqliteAuditStore : public IAuditStore {
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
private:
    sqlite3* db_;
};

} // namespace agentos::bus
```

Features:
- WAL mode for concurrent read/write
- Batch insert via transaction (write_batch)
- `rotate()` deletes rows by age or count via `DELETE FROM audit_log WHERE ...`

#### 2.3 AgentBus Integration

```cpp
class AgentBus {
public:
    explicit AgentBus(security::SecurityManager* sec = nullptr,
                      std::shared_ptr<IAuditStore> store = nullptr);
    // ...
private:
    void audit_push(const BusMessage& msg) {
        // Convert BusMessage → AuditEntry
        if (store_) store_->write(entry);
        // Keep in-memory trail as before (for monitors + backward compat)
        audit_trail_.push_back(msg);
        while (audit_trail_.size() > 10000) audit_trail_.pop_front();
        for (auto& m : monitors_) m(msg);
    }
    std::shared_ptr<IAuditStore> store_;
};
```

In-memory `audit_trail_` stays for backward compatibility and monitor dispatch. The persistent store runs in parallel.

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

    // Get current value (thread-safe read)
    template<typename T>
    T get(const std::string& key, const T& default_val) const;

    // Programmatic update
    void set(const std::string& key, const nlohmann::json& value);

    // Reload from file
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

    void notify(const std::string& key, const nlohmann::json& value);
    void file_watch_loop(std::stop_token st);  // platform-specific
};

} // namespace agentos
```

#### 3.3 File Watch Implementation

```cpp
// src/core/hot_config.cpp
// Platform abstraction:
//   macOS: kqueue + EVFILT_VNODE (NOTE_WRITE)
//   Linux: inotify (IN_MODIFY)
// Falls back to polling (stat every 2s) if neither available
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

Add `config/reload` and `config/get` methods to `MCPServer`:
```json
{"jsonrpc":"2.0","method":"config/reload","id":1}
{"jsonrpc":"2.0","method":"config/get","params":{"key":"tpm_limit"},"id":2}
```

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
    },
    "jieba": {
      "description": "Chinese tokenization support",
      "dependencies": ["cppjieba"]
    }
  }
}
```

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
        "CMAKE_CXX_STANDARD": "23"
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

### New Test Files (2)
9. `tests/test_audit_store.cpp`
10. `tests/test_hot_config.cpp`

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
- Integration: AgentOS with all P0 features enabled end-to-end
- Thread safety: concurrent config reload + log write + audit write
- All existing 719 tests must continue to pass
