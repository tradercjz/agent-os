# P0 Infrastructure Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add structured logging (dual-sink), audit log persistence (SQLite), hot config reload (file watch + API), and vcpkg package management to the AgentOS framework.

**Architecture:** Four independent infrastructure improvements that share no code dependencies between them. Each can be built and tested in isolation. The build system change (vcpkg) is done last since it changes CMakeLists.txt which all other tasks also touch.

**Tech Stack:** C++23, SQLite3, nlohmann/json, kqueue (macOS) / inotify (Linux), vcpkg, CMake 3.25+

**Spec:** `docs/superpowers/specs/2026-03-21-p0-infrastructure-design.md`

---

## Task 1: ILogSink Interface + ConsoleSink

**Files:**
- Create: `include/agentos/core/log_common.hpp` (extracted from logger.hpp)
- Create: `include/agentos/core/log_sink.hpp`
- Test: `tests/test_log_sink.cpp`

- [ ] **Step 1: Write failing test for ConsoleSink**

Create `tests/test_log_sink.cpp`:

```cpp
#include <agentos/core/log_sink.hpp>
#include <gtest/gtest.h>
#include <sstream>

using namespace agentos;

TEST(ConsoleSinkTest, WritesHumanReadableFormat) {
    std::string captured;
    ConsoleSink sink([&](std::string_view s) { captured = std::string(s); });

    LogEvent event{
        .level = LogLevel::Info,
        .timestamp = "2026-03-21T14:00:01.234Z",
        .filename = "test.cpp",
        .line = 42,
        .msg = "hello world",
        .fields = {}
    };
    sink.write(event);

    EXPECT_NE(captured.find("[INFO ]"), std::string::npos);
    EXPECT_NE(captured.find("test.cpp:42"), std::string::npos);
    EXPECT_NE(captured.find("hello world"), std::string::npos);
}

TEST(ConsoleSinkTest, WritesKVFields) {
    std::string captured;
    ConsoleSink sink([&](std::string_view s) { captured = std::string(s); });

    LogEvent event{
        .level = LogLevel::Warn,
        .timestamp = "2026-03-21T14:00:01.234Z",
        .filename = "test.cpp",
        .line = 10,
        .msg = "task started",
        .fields = {{"id", "123"}, {"user", "alice"}}
    };
    sink.write(event);

    EXPECT_NE(captured.find("id=123"), std::string::npos);
    EXPECT_NE(captured.find("user=alice"), std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake .. && make test_agentos 2>&1 | tail -5`
Expected: Compilation error — `log_sink.hpp` not found

- [ ] **Step 3a: Extract log_common.hpp from logger.hpp**

Create `include/agentos/core/log_common.hpp` — extract `LogLevel`, `log_level_str`, and `LogField` from `logger.hpp`. Then update `logger.hpp` to `#include <agentos/core/log_common.hpp>` and remove the duplicated definitions. This eliminates future circular dependency issues.

- [ ] **Step 3b: Create log_sink.hpp with LogEvent, ILogSink, ConsoleSink**

Create `include/agentos/core/log_sink.hpp`:

```cpp
#pragma once
#include <agentos/core/log_common.hpp>
#include <cstdio>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

// ── Public LogEvent (promoted from Logger::LogEvent) ──
struct LogEvent {
    LogLevel           level;
    std::string        timestamp;   // UTC ISO-8601 "YYYY-MM-DDTHH:MM:SS.mmmZ"
    std::string        filename;
    int                line;
    std::string        msg;
    std::vector<LogField> fields;
};

// ── Sink interface ──
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogEvent& event) = 0;
    virtual void flush() {}
};

// ── ConsoleSink: human-readable stderr output ──
class ConsoleSink : public ILogSink {
public:
    // Default: write to stderr
    ConsoleSink() = default;

    // Testable: write to custom output function
    explicit ConsoleSink(std::function<void(std::string_view)> output)
        : output_(std::move(output)) {}

    void write(const LogEvent& event) override {
        std::string formatted;
        if (event.fields.empty()) {
            formatted = fmt::format("[{}] [{}] {}:{}  {}\n",
                event.timestamp, log_level_str(event.level),
                event.filename, event.line, event.msg);
        } else {
            std::string kv_part;
            for (const auto& f : event.fields) {
                kv_part += fmt::format(" {}={}", f.key, f.value);
            }
            formatted = fmt::format("[{}] [{}] {}:{}  {}{}\n",
                event.timestamp, log_level_str(event.level),
                event.filename, event.line, event.msg, kv_part);
        }
        if (output_) {
            output_(formatted);
        } else {
            std::fputs(formatted.c_str(), stderr);
        }
    }

private:
    std::function<void(std::string_view)> output_;
};

} // namespace agentos
```

- [ ] **Step 4: Add test_log_sink.cpp to CMakeLists.txt**

In `CMakeLists.txt`, add to `TEST_SOURCES`:
```cmake
  tests/test_log_sink.cpp
```

- [ ] **Step 5: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='ConsoleSink*'`
Expected: 2 tests PASS

- [ ] **Step 6: Commit**

```bash
git add include/agentos/core/log_sink.hpp tests/test_log_sink.cpp CMakeLists.txt
git commit -m "feat(logging): add ILogSink interface and ConsoleSink"
```

---

## Task 2: JsonFileSink

**Files:**
- Modify: `include/agentos/core/log_sink.hpp`
- Modify: `tests/test_log_sink.cpp`

- [ ] **Step 1: Write failing tests for JsonFileSink**

Append to `tests/test_log_sink.cpp`:

```cpp
#include <filesystem>
#include <fstream>

TEST(JsonFileSinkTest, WritesJsonLines) {
    auto tmp = std::filesystem::temp_directory_path() / "test_json_sink.jsonl";
    std::filesystem::remove(tmp);

    {
        JsonFileSink sink(tmp.string());
        LogEvent event{
            .level = LogLevel::Info,
            .timestamp = "2026-03-21T14:00:01.234Z",
            .filename = "test.cpp",
            .line = 42,
            .msg = "hello",
            .fields = {{"key1", "val1"}}
        };
        sink.write(event);
        sink.flush();
    }

    std::ifstream ifs(tmp);
    std::string line;
    ASSERT_TRUE(std::getline(ifs, line));
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["level"], "INFO");
    EXPECT_EQ(j["msg"], "hello");
    EXPECT_EQ(j["file"], "test.cpp");
    EXPECT_EQ(j["line"], 42);
    EXPECT_EQ(j["extra"]["key1"], "val1");

    std::filesystem::remove(tmp);
}

TEST(JsonFileSinkTest, RotatesOnSizeLimit) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "test_rotation";
    std::filesystem::create_directories(tmp_dir);
    auto logfile = tmp_dir / "app.jsonl";

    {
        // Tiny rotation size to trigger rotation quickly
        JsonFileSink sink(logfile.string(), JsonFileSink::RotationConfig{
            .max_bytes = 100  // very small
        });

        LogEvent event{
            .level = LogLevel::Info,
            .timestamp = "2026-03-21T14:00:01.234Z",
            .filename = "test.cpp",
            .line = 1,
            .msg = "a]message that is long enough to exceed 100 bytes when serialized as JSON",
            .fields = {}
        };
        sink.write(event);
        sink.write(event);  // second write should trigger rotation
        sink.flush();
    }

    // Rotated file should exist
    bool found_rotated = false;
    for (auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
        if (entry.path().string().find("app.") != std::string::npos &&
            entry.path() != logfile) {
            found_rotated = true;
        }
    }
    EXPECT_TRUE(found_rotated);

    std::filesystem::remove_all(tmp_dir);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake .. && make test_agentos 2>&1 | tail -5`
Expected: Compilation error — `JsonFileSink` not defined

- [ ] **Step 3: Implement JsonFileSink**

Add to `include/agentos/core/log_sink.hpp`, after `ConsoleSink`:

```cpp
// ── JsonFileSink: JSON Lines file output with rotation ──
class JsonFileSink : public ILogSink {
public:
    struct RotationConfig {
        size_t max_bytes{50 * 1024 * 1024};  // 50MB default
    };

    explicit JsonFileSink(std::string path,
                          RotationConfig rot = {})
        : path_(std::move(path)), rot_(rot), bytes_written_(0) {
        ofs_.open(path_, std::ios::app);
    }

    void write(const LogEvent& event) override {
        nlohmann::json j;
        j["ts"] = event.timestamp;
        j["level"] = log_level_str(event.level);
        j["file"] = event.filename;
        j["line"] = event.line;
        j["msg"] = event.msg;

        if (!event.fields.empty()) {
            nlohmann::json extra;
            for (const auto& f : event.fields) {
                extra[f.key] = f.value;
            }
            j["extra"] = extra;
        }

        std::string line = j.dump() + "\n";
        bytes_written_ += line.size();

        ofs_ << line;

        if (rot_.max_bytes > 0 && bytes_written_ >= rot_.max_bytes) {
            rotate();
        }
    }

    void flush() override {
        if (ofs_.is_open()) ofs_.flush();
    }

private:
    void rotate() {
        ofs_.close();

        // Rename to path_.<timestamp>.jsonl
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        char time_buf[20];
        std::strftime(time_buf, sizeof(time_buf), "%Y%m%d%H%M%S",
                      std::gmtime(&time_t_now));

        std::string rotated = path_ + "." + time_buf + ".jsonl";

        // If target exists, append ms suffix
        if (std::filesystem::exists(rotated)) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;
            rotated = path_ + "." + time_buf + "." +
                      std::to_string(ms.count()) + ".jsonl";
        }

        std::error_code ec;
        std::filesystem::rename(path_, rotated, ec);
        if (ec) {
            // Fallback: just reopen (data stays in old file)
            LOG_WARN(fmt::format("JsonFileSink: rotation rename failed: {}", ec.message()));
        }

        ofs_.open(path_, std::ios::app);
        bytes_written_ = 0;
    }

    std::string path_;
    RotationConfig rot_;
    std::ofstream ofs_;
    size_t bytes_written_;
};
```

Add these includes at the top of `log_sink.hpp`:
```cpp
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
```

- [ ] **Step 4: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='JsonFileSink*'`
Expected: 2 tests PASS

- [ ] **Step 5: Commit**

```bash
git add include/agentos/core/log_sink.hpp tests/test_log_sink.cpp
git commit -m "feat(logging): add JsonFileSink with size-based rotation"
```

---

## Task 3: Logger Multi-Sink Refactor

**Files:**
- Modify: `include/agentos/core/logger.hpp`
- Modify: `tests/test_log_sink.cpp`

- [ ] **Step 1: Write failing test for multi-sink Logger**

Append to `tests/test_log_sink.cpp`:

```cpp
TEST(LoggerMultiSinkTest, BroadcastsToAllSinks) {
    auto& logger = Logger::instance();
    logger.set_level(LogLevel::Info);

    std::string console_out;
    std::string json_out;

    auto console = std::make_shared<ConsoleSink>(
        [&](std::string_view s) { console_out = std::string(s); });

    auto tmp = std::filesystem::temp_directory_path() / "multi_sink_test.jsonl";
    std::filesystem::remove(tmp);
    auto json = std::make_shared<JsonFileSink>(tmp.string());

    logger.clear_sinks();
    logger.add_sink(console);
    logger.add_sink(json);

    LOG_INFO("multi-sink test");
    logger.flush();

    // Console sink received
    EXPECT_NE(console_out.find("multi-sink test"), std::string::npos);

    // JSON file sink received
    json->flush();
    std::ifstream ifs(tmp);
    std::string line;
    ASSERT_TRUE(std::getline(ifs, line));
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["msg"], "multi-sink test");

    // Cleanup
    logger.clear_sinks();
    logger.set_sink(nullptr);  // restore default stderr
    logger.set_level(LogLevel::Warn);
    std::filesystem::remove(tmp);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: `add_sink` / `clear_sinks` not defined in Logger

- [ ] **Step 3: Refactor Logger to support multi-sink**

Modify `include/agentos/core/logger.hpp`:

**a)** Add include at top:
```cpp
#include <shared_mutex>
```

**b)** Add forward declaration after `LogSink` typedef:
```cpp
class ILogSink;  // defined in log_sink.hpp
```

**c)** Add public methods to `Logger` class:
```cpp
    // ── Multi-sink API ──
    void add_sink(std::shared_ptr<ILogSink> sink) {
        std::unique_lock lk(sinks_mu_);
        sinks_.push_back(std::move(sink));
    }

    void clear_sinks() {
        std::unique_lock lk(sinks_mu_);
        sinks_.clear();
    }
```

**d)** Add private members:
```cpp
    std::vector<std::shared_ptr<ILogSink>> sinks_;
    std::shared_mutex sinks_mu_;
```

**e)** Modify `process_event()` to broadcast to sinks. After the existing `sink_copy` dispatch block, add:

```cpp
    // Broadcast to registered ILogSink instances (using public LogEvent directly)
    {
        std::vector<std::shared_ptr<ILogSink>> sinks_snapshot;
        {
            std::shared_lock lk(sinks_mu_);
            sinks_snapshot = sinks_;
        }
        for (auto& s : sinks_snapshot) {
            s->write(event);  // LogEvent is the public type — no duplication
        }
    }
```

**f)** Resolve the circular include between `logger.hpp` and `log_sink.hpp`.

The cleanest approach: extract `LogLevel`, `log_level_str`, and `LogField` into a shared header `include/agentos/core/log_common.hpp`. Both `logger.hpp` and `log_sink.hpp` include `log_common.hpp` — no circular dependency.

Create `include/agentos/core/log_common.hpp`:
```cpp
#pragma once
#include <string>
#include <vector>

namespace agentos {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

inline const char *log_level_str(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    default:              return "?????";
    }
}

struct LogField {
    std::string key;
    std::string value;
};

} // namespace agentos
```

Then in `logger.hpp`:
- Replace the `LogLevel` enum, `log_level_str`, and `LogField` definitions with `#include <agentos/core/log_common.hpp>`
- Remove the private `LogEvent` inner struct — use the public `LogEvent` from `log_sink.hpp` instead
- Add `#include <agentos/core/log_sink.hpp>` at the top (after `log_common.hpp`)
- `process_event()` now uses the public `LogEvent` directly — no duplication

In `log_sink.hpp`:
- Replace `#include <agentos/core/logger.hpp>` with `#include <agentos/core/log_common.hpp>`
- No circular dependency

Include chain: `log_common.hpp` ← `log_sink.hpp` ← `logger.hpp`

**g)** Modify `prepare_event()` to use UTC via `gmtime_r`:

Replace:
```cpp
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_t_now));
    std::string ts = ::agentos::fmt::format("{}.{:03d}", time_buf, static_cast<int>(ms.count()));
```

With:
```cpp
    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &time_t_now);
#else
    gmtime_r(&time_t_now, &utc_tm);
#endif
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S",
                  &utc_tm);
    std::string ts = ::agentos::fmt::format("{}.{:03d}Z", time_buf, static_cast<int>(ms.count()));
```

- [ ] **Step 4: Build and run ALL logger/sink tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*Logger*:*Sink*'`
Expected: All tests PASS (existing logger tests + new sink tests)

- [ ] **Step 5: Commit**

```bash
git add include/agentos/core/logger.hpp include/agentos/core/log_sink.hpp tests/test_log_sink.cpp
git commit -m "feat(logging): refactor Logger to multi-sink with UTC timestamps"
```

---

## Task 4: IAuditStore Interface + AuditEntry Types

**Files:**
- Create: `include/agentos/bus/audit_store.hpp`
- Modify: `tests/test_audit_store.cpp` (new file)

- [ ] **Step 1: Write failing test for AuditEntry and AuditFilter**

Create `tests/test_audit_store.cpp`:

```cpp
#include <agentos/bus/audit_store.hpp>
#include <gtest/gtest.h>

using namespace agentos::bus;

TEST(AuditEntryTest, ConstructsWithWallTimePoint) {
    AuditEntry entry{
        .id = 1,
        .timestamp = std::chrono::system_clock::now(),
        .from_agent = 10,
        .to_agent = 20,
        .type = MessageType::Request,
        .topic = "greet",
        .payload = R"({"msg":"hello"})",
        .redacted = false
    };
    EXPECT_EQ(entry.id, 1u);
    EXPECT_EQ(entry.from_agent, 10u);
    EXPECT_FALSE(entry.redacted);
}

TEST(AuditFilterTest, DefaultLimitIs100) {
    AuditFilter f;
    EXPECT_EQ(f.limit, 100u);
    EXPECT_EQ(f.offset, 0u);
    EXPECT_FALSE(f.agent_id.has_value());
}

TEST(RotationPolicyTest, DefaultValues) {
    RotationPolicy p;
    EXPECT_EQ(p.max_entries, 100000u);
    EXPECT_EQ(p.max_age.count(), 720);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: `audit_store.hpp` not found

- [ ] **Step 3: Create audit_store.hpp**

Create `include/agentos/bus/audit_store.hpp`:

```cpp
#pragma once
#include <agentos/core/types.hpp>
#include <agentos/bus/agent_bus.hpp>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace agentos::bus {

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
    std::optional<AgentId>       agent_id;
    std::optional<MessageType>   type;
    std::optional<WallTimePoint> after;
    std::optional<WallTimePoint> before;
    std::optional<std::string>   topic;
    size_t                       limit{100};
    size_t                       offset{0};
};

struct RotationPolicy {
    size_t max_entries{100000};
    std::chrono::hours max_age{24 * 30};  // 30 days
};

class IAuditStore {
public:
    virtual ~IAuditStore() = default;
    [[nodiscard]] virtual Result<void> write(const AuditEntry& entry) = 0;
    [[nodiscard]] virtual Result<void> write_batch(std::span<const AuditEntry> entries) = 0;
    virtual std::vector<AuditEntry> query(const AuditFilter& filter) = 0;
    virtual size_t count(const AuditFilter& filter) = 0;
    [[nodiscard]] virtual Result<void> rotate(const RotationPolicy& policy) = 0;
    virtual void flush() {}
};

// Helper: convert BusMessage → AuditEntry
inline AuditEntry to_audit_entry(const BusMessage& msg) {
    return AuditEntry{
        .id = msg.id,
        .timestamp = std::chrono::system_clock::now(),
        .from_agent = msg.from,
        .to_agent = msg.to,
        .type = msg.type,
        .topic = msg.topic,
        .payload = msg.payload,
        .redacted = msg.redacted
    };
}

} // namespace agentos::bus
```

- [ ] **Step 4: Add test_audit_store.cpp to CMakeLists.txt**

Add to `TEST_SOURCES`:
```cmake
  tests/test_audit_store.cpp
```

- [ ] **Step 5: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='Audit*:Rotation*'`
Expected: 3 tests PASS

- [ ] **Step 6: Commit**

```bash
git add include/agentos/bus/audit_store.hpp tests/test_audit_store.cpp CMakeLists.txt
git commit -m "feat(audit): add IAuditStore interface and AuditEntry types"
```

---

## Task 5: SqliteAuditStore Implementation

**Files:**
- Create: `include/agentos/bus/sqlite_audit_store.hpp`
- Create: `src/bus/sqlite_audit_store.cpp`
- Modify: `tests/test_audit_store.cpp`

- [ ] **Step 1: Write failing tests for SqliteAuditStore**

Append to `tests/test_audit_store.cpp`:

```cpp
#include <agentos/bus/sqlite_audit_store.hpp>
#include <filesystem>

class SqliteAuditStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = (std::filesystem::temp_directory_path() / "test_audit.db").string();
        std::filesystem::remove(db_path_);
        store_ = std::make_unique<SqliteAuditStore>(db_path_);
    }
    void TearDown() override {
        store_.reset();
        std::filesystem::remove(db_path_);
    }
    std::string db_path_;
    std::unique_ptr<SqliteAuditStore> store_;

    AuditEntry make_entry(uint64_t id, AgentId from, AgentId to,
                          MessageType type = MessageType::Request,
                          std::string topic = "test") {
        return AuditEntry{
            .id = id,
            .timestamp = std::chrono::system_clock::now(),
            .from_agent = from,
            .to_agent = to,
            .type = type,
            .topic = std::move(topic),
            .payload = R"({"data":"hello"})",
            .redacted = false
        };
    }
};

TEST_F(SqliteAuditStoreTest, WriteAndQueryBack) {
    auto entry = make_entry(1, 10, 20);
    auto r = store_->write(entry);
    ASSERT_TRUE(r.has_value());

    auto results = store_->query(AuditFilter{});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].from_agent, 10u);
    EXPECT_EQ(results[0].to_agent, 20u);
    EXPECT_EQ(results[0].topic, "test");
}

TEST_F(SqliteAuditStoreTest, WriteBatch) {
    std::vector<AuditEntry> entries;
    for (uint64_t i = 0; i < 5; ++i) {
        entries.push_back(make_entry(i, 10, 20 + i));
    }
    auto r = store_->write_batch(entries);
    ASSERT_TRUE(r.has_value());

    EXPECT_EQ(store_->count(AuditFilter{}), 5u);
}

TEST_F(SqliteAuditStoreTest, QueryFilterByAgent) {
    store_->write(make_entry(1, 10, 20));
    store_->write(make_entry(2, 30, 40));
    store_->write(make_entry(3, 10, 50));

    AuditFilter f;
    f.agent_id = 10;
    auto results = store_->query(f);
    EXPECT_EQ(results.size(), 2u);
}

TEST_F(SqliteAuditStoreTest, QueryFilterByType) {
    store_->write(make_entry(1, 10, 20, MessageType::Request));
    store_->write(make_entry(2, 10, 20, MessageType::Event));
    store_->write(make_entry(3, 10, 20, MessageType::Request));

    AuditFilter f;
    f.type = MessageType::Event;
    auto results = store_->query(f);
    EXPECT_EQ(results.size(), 1u);
}

TEST_F(SqliteAuditStoreTest, QueryPagination) {
    for (uint64_t i = 0; i < 10; ++i) {
        store_->write(make_entry(i, 10, 20));
    }

    AuditFilter f;
    f.limit = 3;
    f.offset = 2;
    auto results = store_->query(f);
    EXPECT_EQ(results.size(), 3u);
}

TEST_F(SqliteAuditStoreTest, RotateByMaxEntries) {
    for (uint64_t i = 0; i < 20; ++i) {
        store_->write(make_entry(i, 10, 20));
    }
    EXPECT_EQ(store_->count(AuditFilter{}), 20u);

    RotationPolicy p;
    p.max_entries = 10;
    auto r = store_->rotate(p);
    ASSERT_TRUE(r.has_value());

    EXPECT_LE(store_->count(AuditFilter{}), 10u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: `sqlite_audit_store.hpp` not found

- [ ] **Step 3: Create SqliteAuditStore header**

Create `include/agentos/bus/sqlite_audit_store.hpp`:

```cpp
#pragma once
#include <agentos/bus/audit_store.hpp>
#include <sqlite3.h>
#include <string>

namespace agentos::bus {

struct SqliteGuard {
    sqlite3* db = nullptr;
    SqliteGuard() = default;
    explicit SqliteGuard(const std::string& path) {
        int rc = sqlite3_open(path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::string err = db ? sqlite3_errmsg(db) : "unknown error";
            if (db) { sqlite3_close(db); db = nullptr; }
            throw std::runtime_error("SqliteGuard: failed to open " + path + ": " + err);
        }
    }
    ~SqliteGuard() { if (db) sqlite3_close(db); }
    SqliteGuard(const SqliteGuard&) = delete;
    SqliteGuard& operator=(const SqliteGuard&) = delete;
    SqliteGuard(SqliteGuard&& o) noexcept : db(o.db) { o.db = nullptr; }
    SqliteGuard& operator=(SqliteGuard&& o) noexcept {
        if (this != &o) { if (db) sqlite3_close(db); db = o.db; o.db = nullptr; }
        return *this;
    }
};

class SqliteAuditStore : public IAuditStore, private NonCopyable {
public:
    explicit SqliteAuditStore(const std::string& db_path);

    Result<void> write(const AuditEntry& entry) override;
    Result<void> write_batch(std::span<const AuditEntry> entries) override;
    std::vector<AuditEntry> query(const AuditFilter& filter) override;
    size_t count(const AuditFilter& filter) override;
    Result<void> rotate(const RotationPolicy& policy) override;
    void flush() override;

private:
    SqliteGuard db_;

    static std::string message_type_to_str(MessageType t);
    static MessageType str_to_message_type(const std::string& s);
    static std::string timepoint_to_iso(WallTimePoint tp);
    static WallTimePoint iso_to_timepoint(const std::string& s);

    void build_where_clause(const AuditFilter& filter,
                            std::string& sql,
                            std::vector<std::string>& bind_values) const;
};

} // namespace agentos::bus
```

- [ ] **Step 4: Create SqliteAuditStore implementation**

Create `src/bus/sqlite_audit_store.cpp`:

```cpp
#include <agentos/bus/sqlite_audit_store.hpp>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace agentos::bus {

SqliteAuditStore::SqliteAuditStore(const std::string& db_path)
    : db_(db_path) {
    // Enable WAL mode
    sqlite3_exec(db_.db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS audit_log (
            id INTEGER PRIMARY KEY,
            timestamp TEXT NOT NULL,
            from_agent INTEGER,
            to_agent INTEGER,
            type TEXT NOT NULL,
            topic TEXT,
            payload TEXT,
            redacted INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(timestamp);
        CREATE INDEX IF NOT EXISTS idx_audit_agent ON audit_log(from_agent, to_agent);
        CREATE INDEX IF NOT EXISTS idx_audit_type_ts ON audit_log(type, timestamp);
    )";
    char* err = nullptr;
    int rc = sqlite3_exec(db_.db, create_sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("SqliteAuditStore: schema init failed: " + msg);
    }
}

Result<void> SqliteAuditStore::write(const AuditEntry& entry) {
    const char* sql = "INSERT INTO audit_log (id, timestamp, from_agent, to_agent, type, topic, payload, redacted) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_error(ErrorCode::MemoryWriteFailed,
                         fmt::format("audit write prepare: {}", sqlite3_errmsg(db_.db)));
    }

    auto ts = timepoint_to_iso(entry.timestamp);
    auto type_str = message_type_to_str(entry.type);

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(entry.id));
    sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(entry.from_agent));
    sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(entry.to_agent));
    sqlite3_bind_text(stmt, 5, type_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, entry.topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, entry.payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, entry.redacted ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return make_error(ErrorCode::MemoryWriteFailed,
                         fmt::format("audit write step: {}", sqlite3_errmsg(db_.db)));
    }
    return {};
}

Result<void> SqliteAuditStore::write_batch(std::span<const AuditEntry> entries) {
    sqlite3_exec(db_.db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (const auto& e : entries) {
        auto r = write(e);
        if (!r.has_value()) {
            sqlite3_exec(db_.db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return r;
        }
    }
    sqlite3_exec(db_.db, "COMMIT;", nullptr, nullptr, nullptr);
    return {};
}

std::vector<AuditEntry> SqliteAuditStore::query(const AuditFilter& filter) {
    std::string sql = "SELECT id, timestamp, from_agent, to_agent, type, topic, payload, redacted FROM audit_log";
    std::vector<std::string> bind_values;
    build_where_clause(filter, sql, bind_values);
    sql += " ORDER BY id ASC";
    sql += fmt::format(" LIMIT {} OFFSET {}", filter.limit, filter.offset);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);

    int idx = 1;
    for (const auto& v : bind_values) {
        sqlite3_bind_text(stmt, idx++, v.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<AuditEntry> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AuditEntry e;
        e.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        e.timestamp = iso_to_timepoint(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
        e.from_agent = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        e.to_agent = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        e.type = str_to_message_type(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
        auto topic_ptr = sqlite3_column_text(stmt, 5);
        e.topic = topic_ptr ? reinterpret_cast<const char*>(topic_ptr) : "";
        auto payload_ptr = sqlite3_column_text(stmt, 6);
        e.payload = payload_ptr ? reinterpret_cast<const char*>(payload_ptr) : "";
        e.redacted = sqlite3_column_int(stmt, 7) != 0;
        results.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    return results;
}

size_t SqliteAuditStore::count(const AuditFilter& filter) {
    std::string sql = "SELECT COUNT(*) FROM audit_log";
    std::vector<std::string> bind_values;
    build_where_clause(filter, sql, bind_values);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);

    int idx = 1;
    for (const auto& v : bind_values) {
        sqlite3_bind_text(stmt, idx++, v.c_str(), -1, SQLITE_TRANSIENT);
    }

    size_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return result;
}

Result<void> SqliteAuditStore::rotate(const RotationPolicy& policy) {
    if (policy.max_entries > 0) {
        size_t current = count(AuditFilter{.limit = 0});
        if (current > policy.max_entries) {
            size_t to_delete = current - policy.max_entries;
            std::string sql = fmt::format(
                "DELETE FROM audit_log WHERE id IN "
                "(SELECT id FROM audit_log ORDER BY id ASC LIMIT {})",
                to_delete);
            char* err = nullptr;
            int rc = sqlite3_exec(db_.db, sql.c_str(), nullptr, nullptr, &err);
            if (rc != SQLITE_OK) {
                std::string msg = err ? err : "unknown";
                sqlite3_free(err);
                return make_error(ErrorCode::MemoryWriteFailed,
                                 "audit rotate: " + msg);
            }
        }
    }

    if (policy.max_age.count() > 0) {
        auto cutoff = std::chrono::system_clock::now() - policy.max_age;
        auto cutoff_str = timepoint_to_iso(cutoff);
        std::string sql = "DELETE FROM audit_log WHERE timestamp < ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, cutoff_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return {};
}

void SqliteAuditStore::flush() {
    sqlite3_wal_checkpoint_v2(db_.db, nullptr, SQLITE_CHECKPOINT_PASSIVE,
                              nullptr, nullptr);
}

// ── Helpers ──

std::string SqliteAuditStore::message_type_to_str(MessageType t) {
    switch (t) {
        case MessageType::Request:   return "Request";
        case MessageType::Response:  return "Response";
        case MessageType::Event:     return "Event";
        case MessageType::Heartbeat: return "Heartbeat";
        default:                     return "Unknown";
    }
}

MessageType SqliteAuditStore::str_to_message_type(const std::string& s) {
    if (s == "Request")   return MessageType::Request;
    if (s == "Response")  return MessageType::Response;
    if (s == "Event")     return MessageType::Event;
    if (s == "Heartbeat") return MessageType::Heartbeat;
    return MessageType::Request;
}

std::string SqliteAuditStore::timepoint_to_iso(WallTimePoint tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;
    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &time_t_val);
#else
    gmtime_r(&time_t_val, &utc_tm);
#endif
    char buf[24];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc_tm);
    return fmt::format("{}.{:03d}Z", buf, static_cast<int>(ms.count()));
}

WallTimePoint SqliteAuditStore::iso_to_timepoint(const std::string& s) {
    struct tm tm = {};
    int ms = 0;
    // Parse "2026-03-21T14:00:01.234Z"
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) >= 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        auto time_t_val = timegm(&tm);
        return std::chrono::system_clock::from_time_t(time_t_val) +
               std::chrono::milliseconds(ms);
    }
    return std::chrono::system_clock::now();
}

void SqliteAuditStore::build_where_clause(const AuditFilter& filter,
                                           std::string& sql,
                                           std::vector<std::string>& bind_values) const {
    std::vector<std::string> conditions;

    if (filter.agent_id) {
        conditions.push_back("(from_agent = ? OR to_agent = ?)");
        auto id_str = std::to_string(*filter.agent_id);
        bind_values.push_back(id_str);
        bind_values.push_back(id_str);
    }
    if (filter.type) {
        conditions.push_back("type = ?");
        bind_values.push_back(message_type_to_str(*filter.type));
    }
    if (filter.after) {
        conditions.push_back("timestamp > ?");
        bind_values.push_back(timepoint_to_iso(*filter.after));
    }
    if (filter.before) {
        conditions.push_back("timestamp < ?");
        bind_values.push_back(timepoint_to_iso(*filter.before));
    }
    if (filter.topic) {
        conditions.push_back("topic = ?");
        bind_values.push_back(*filter.topic);
    }

    if (!conditions.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) sql += " AND ";
            sql += conditions[i];
        }
    }
}

} // namespace agentos::bus
```

- [ ] **Step 5: Add sqlite_audit_store.cpp to CMakeLists.txt**

Add to `AGENTOS_SOURCES`:
```cmake
    src/bus/sqlite_audit_store.cpp
```

- [ ] **Step 6: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='SqliteAuditStore*'`
Expected: 6 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/agentos/bus/sqlite_audit_store.hpp src/bus/sqlite_audit_store.cpp tests/test_audit_store.cpp CMakeLists.txt
git commit -m "feat(audit): implement SqliteAuditStore with WAL mode"
```

---

## Task 6: AgentBus Integration with IAuditStore

**Files:**
- Modify: `include/agentos/bus/agent_bus.hpp`
- Modify: `include/agentos/agent.hpp` (AgentOS constructor)
- Modify: `tests/test_bus.cpp`

- [ ] **Step 1: Write failing test for bus + persistent store**

Append to `tests/test_bus.cpp`:

```cpp
#include <agentos/bus/sqlite_audit_store.hpp>
#include <filesystem>

TEST(AgentBusPersistentTest, WritesToAuditStore) {
    auto db_path = (std::filesystem::temp_directory_path() / "bus_audit_test.db").string();
    std::filesystem::remove(db_path);

    auto store = std::make_shared<bus::SqliteAuditStore>(db_path);
    AgentBus bus_with_store(nullptr, store);

    auto ch_a = bus_with_store.register_agent(100);
    auto ch_b = bus_with_store.register_agent(200);

    auto msg = BusMessage::make_request(100, 200, "hello", "world");
    bus_with_store.send(msg);

    // Check persistent store
    auto results = store->query(bus::AuditFilter{});
    EXPECT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].from_agent, 100u);
    EXPECT_EQ(results[0].topic, "hello");

    std::filesystem::remove(db_path);
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: `AgentBus` constructor does not accept `IAuditStore`

- [ ] **Step 3: Modify AgentBus to accept and use IAuditStore**

In `include/agentos/bus/agent_bus.hpp`:

**a)** Add include:
```cpp
#include <agentos/bus/audit_store.hpp>
```

**b)** Modify constructor:
```cpp
    explicit AgentBus(security::SecurityManager* sec = nullptr,
                      std::shared_ptr<IAuditStore> store = nullptr)
        : security_(sec), store_(std::move(store)) {}
```

**c)** Split `audit_push` into two methods:

```cpp
    // In-memory audit (called under mu_)
    void audit_push_mem(const BusMessage& msg) {
        audit_trail_.push_back(msg);
        for (auto& m : monitors_) m(msg);
        while (audit_trail_.size() > 10000) {
            audit_trail_.pop_front();
        }
    }

    // Persistent store (called OUTSIDE mu_)
    void audit_push_store(const BusMessage& msg) {
        if (store_) {
            auto entry = to_audit_entry(msg);
            (void)store_->write(entry);
        }
    }
```

**d)** Update `send()` to use split audit:

Replace the line `audit_push(msg);` inside `send()` with `audit_push_mem(msg);`, and add `audit_push_store(msg);` AFTER the `lock_guard` scope ends. Same for `publish()`.

For `send()`:
```cpp
    bool send(BusMessage msg) {
        // ... security filtering ...
        BusMessage msg_copy = msg;  // copy before lock
        bool all_accepted = true;
        {
            std::lock_guard lk(mu_);
            audit_push_mem(msg);
            // ... routing logic (unchanged) ...
        }
        audit_push_store(msg_copy);  // outside lock
        return all_accepted;
    }
```

For `publish()`:
```cpp
    void publish(BusMessage event) {
        if (event.type != MessageType::Event) return;

        // Security scan (unchanged)
        if (security_) {
            auto det = security_->detector().scan(event.payload);
            if (det.is_injection) {
                event.payload = "[REDACTED: injection detected in event]";
                event.redacted = true;
            }
        }

        BusMessage event_copy = event;  // copy before lock
        {
            std::lock_guard lk(mu_);
            audit_push_mem(event);
            // ... routing to subscribers (unchanged) ...
        }
        audit_push_store(event_copy);  // outside lock
    }
```

**e)** Add member:
```cpp
    std::shared_ptr<IAuditStore> store_;
```

**f)** Remove old `audit_push()` method.

- [ ] **Step 4: Wire IAuditStore into AgentOS constructor**

In `include/agentos/agent.hpp`, modify the AgentOS constructor where the bus is created (line ~337):

Replace:
```cpp
    bus_ = std::make_unique<bus::AgentBus>(security_.get());
```

With:
```cpp
    // Create audit store if data directory is available
    std::shared_ptr<bus::IAuditStore> audit_store;
    {
        auto audit_db = std::filesystem::path(config_.snapshot_dir) / "audit.db";
        try {
            audit_store = std::make_shared<bus::SqliteAuditStore>(audit_db.string());
        } catch (const std::exception& e) {
            LOG_WARN(::agentos::fmt::format("Audit store init failed: {} — running without persistence", e.what()));
        }
    }
    bus_ = std::make_unique<bus::AgentBus>(security_.get(), std::move(audit_store));
```

Add include at top of `agent.hpp`:
```cpp
#include <agentos/bus/sqlite_audit_store.hpp>
```

- [ ] **Step 5: Build and run ALL bus tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*Bus*:*Channel*'`
Expected: All existing + new tests PASS

- [ ] **Step 6: Commit**

```bash
git add include/agentos/bus/agent_bus.hpp include/agentos/agent.hpp tests/test_bus.cpp
git commit -m "feat(audit): integrate IAuditStore into AgentBus with split-lock pattern"
```

---

## Task 7: HotConfig Core

**Files:**
- Create: `include/agentos/core/hot_config.hpp`
- Create: `src/core/hot_config.cpp`
- Create: `tests/test_hot_config.cpp`

- [ ] **Step 1: Write failing tests for HotConfig**

Create `tests/test_hot_config.cpp`:

```cpp
#include <agentos/core/hot_config.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace agentos;

TEST(HotConfigTest, GetReturnsDefaultForMissingKey) {
    HotConfig cfg;
    EXPECT_EQ(cfg.get<int>("nonexistent", 42), 42);
    EXPECT_EQ(cfg.get<std::string>("nope", "fallback"), "fallback");
}

TEST(HotConfigTest, SetAndGet) {
    HotConfig cfg;
    auto r = cfg.set("tpm_limit", 200000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(cfg.get<int>("tpm_limit", 0), 200000);
}

TEST(HotConfigTest, ReloadFromFile) {
    auto tmp = std::filesystem::temp_directory_path() / "hot_config_test.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({"tpm_limit": 50000, "log_level": "debug"})";
    }

    HotConfig cfg(tmp.string());
    auto r = cfg.reload();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(cfg.get<int>("tpm_limit", 0), 50000);
    EXPECT_EQ(cfg.get<std::string>("log_level", ""), "debug");

    std::filesystem::remove(tmp);
}

TEST(HotConfigTest, ReloadRejectsInvalidFile) {
    auto tmp = std::filesystem::temp_directory_path() / "hot_config_bad.json";
    {
        std::ofstream ofs(tmp);
        ofs << "not valid json{{{";
    }

    HotConfig cfg(tmp.string());
    auto r = cfg.reload();
    EXPECT_FALSE(r.has_value());

    std::filesystem::remove(tmp);
}

TEST(HotConfigTest, ObserverNotifiedOnChange) {
    HotConfig cfg;
    std::string observed_key;
    int observed_val = 0;

    cfg.on_change("tpm_limit", [&](const std::string& key, const nlohmann::json& val) {
        observed_key = key;
        observed_val = val.get<int>();
    });

    cfg.set("tpm_limit", 99999);

    EXPECT_EQ(observed_key, "tpm_limit");
    EXPECT_EQ(observed_val, 99999);
}

TEST(HotConfigTest, ObserverCanCallGetWithoutDeadlock) {
    HotConfig cfg;
    cfg.set("a", 1);

    int read_val = 0;
    cfg.on_change("b", [&](const std::string&, const nlohmann::json&) {
        // Callback reads another key — would deadlock if notify holds exclusive lock
        read_val = cfg.get<int>("a", 0);
    });

    cfg.set("b", 2);
    EXPECT_EQ(read_val, 1);
}

TEST(HotConfigTest, ValidationRejectsNegativeTPM) {
    HotConfig cfg;
    auto r = cfg.set("tpm_limit", -5);
    EXPECT_FALSE(r.has_value());
}

TEST(HotConfigTest, ConcurrentGetSetNoRace) {
    HotConfig cfg;
    cfg.set("counter", 0);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 1000 && !stop; ++i) {
            (void)cfg.set("counter", i);
        }
        stop = true;
    });

    std::thread reader([&] {
        while (!stop) {
            auto v = cfg.get<int>("counter", -1);
            EXPECT_GE(v, -1);
        }
    });

    writer.join();
    reader.join();
}
```

- [ ] **Step 2: Run test to verify it fails**

Expected: `hot_config.hpp` not found

- [ ] **Step 3: Create hot_config.hpp**

First create the `src/core/` directory (does not exist yet):
```bash
mkdir -p src/core
```

Create `include/agentos/core/hot_config.hpp`:

```cpp
#pragma once
#include <agentos/core/types.hpp>
#include <fstream>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agentos {

using ConfigChangeCallback = std::function<void(const std::string& key, const nlohmann::json& value)>;

class HotConfig : private NonCopyable {
public:
    explicit HotConfig(const std::string& config_path = "");

    template<typename T>
    T get(const std::string& key, const T& default_val) const {
        std::shared_lock lk(mu_);
        if (config_.contains(key)) {
            try { return config_[key].get<T>(); }
            catch (const nlohmann::json::exception&) { return default_val; }
        }
        return default_val;
    }

    Result<void> set(const std::string& key, const nlohmann::json& value);
    Result<void> reload();

    void on_change(const std::string& key, ConfigChangeCallback cb);

    void start_watching();
    void stop_watching();

private:
    nlohmann::json config_;
    mutable std::shared_mutex mu_;
    std::string config_path_;
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> observers_;
    std::mutex observers_mu_;  // separate lock for observer registration
    std::jthread watcher_thread_;

    Result<void> validate(const std::string& key, const nlohmann::json& value) const;
    void apply_and_notify(const nlohmann::json& new_config);
    void file_watch_loop(std::stop_token st);
};

} // namespace agentos
```

- [ ] **Step 4: Create hot_config.cpp**

Create `src/core/hot_config.cpp`:

```cpp
#include <agentos/core/hot_config.hpp>
#include <agentos/core/logger.hpp>
#include <chrono>

#ifdef __APPLE__
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#elif __linux__
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace agentos {

HotConfig::HotConfig(const std::string& config_path)
    : config_path_(config_path) {
    if (!config_path_.empty()) {
        (void)reload();  // best-effort on construction
    }
}

Result<void> HotConfig::set(const std::string& key, const nlohmann::json& value) {
    auto vr = validate(key, value);
    if (!vr.has_value()) return vr;

    nlohmann::json new_config;
    {
        std::shared_lock lk(mu_);
        new_config = config_;
    }
    new_config[key] = value;
    apply_and_notify(new_config);
    return {};
}

Result<void> HotConfig::reload() {
    if (config_path_.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no config file path set");
    }

    std::ifstream ifs(config_path_);
    if (!ifs) {
        return make_error(ErrorCode::NotFound,
                         fmt::format("cannot open config file: {}", config_path_));
    }

    nlohmann::json new_config;
    try {
        ifs >> new_config;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(ErrorCode::InvalidArgument,
                         fmt::format("config parse error: {}", e.what()));
    }

    // Validate all keys
    for (auto& [key, val] : new_config.items()) {
        auto vr = validate(key, val);
        if (!vr.has_value()) return vr;
    }

    apply_and_notify(new_config);
    return {};
}

void HotConfig::on_change(const std::string& key, ConfigChangeCallback cb) {
    std::lock_guard lk(observers_mu_);
    observers_[key].push_back(std::move(cb));
}

Result<void> HotConfig::validate(const std::string& key, const nlohmann::json& value) const {
    // Known key validation
    if (key == "tpm_limit") {
        if (!value.is_number_integer() || value.get<int64_t>() <= 0) {
            return make_error(ErrorCode::InvalidArgument,
                             "tpm_limit must be a positive integer");
        }
    } else if (key == "log_level") {
        if (!value.is_string()) {
            return make_error(ErrorCode::InvalidArgument,
                             "log_level must be a string");
        }
        auto s = value.get<std::string>();
        if (s != "debug" && s != "info" && s != "warn" && s != "error" && s != "off") {
            return make_error(ErrorCode::InvalidArgument,
                             "log_level must be one of: debug, info, warn, error, off");
        }
    } else if (key == "injection_threshold") {
        if (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 1.0) {
            return make_error(ErrorCode::InvalidArgument,
                             "injection_threshold must be a number in [0.0, 1.0]");
        }
    } else if (key == "injection_detection_enabled") {
        if (!value.is_boolean()) {
            return make_error(ErrorCode::InvalidArgument,
                             "injection_detection_enabled must be a boolean");
        }
    } else if (key == "audit_rotation_max_entries" || key == "context_default_token_limit") {
        if (!value.is_number_unsigned() || value.get<uint64_t>() == 0) {
            return make_error(ErrorCode::InvalidArgument,
                             key + " must be a positive integer");
        }
    } else if (key == "audit_rotation_max_age_hours") {
        if (!value.is_number_unsigned()) {
            return make_error(ErrorCode::InvalidArgument,
                             "audit_rotation_max_age_hours must be a non-negative integer");
        }
    }
    // Unknown keys are allowed (forward-compatible)
    return {};
}

void HotConfig::apply_and_notify(const nlohmann::json& new_config) {
    // Step 1: diff + swap under exclusive lock
    std::vector<std::pair<std::string, nlohmann::json>> changed;
    {
        std::unique_lock lk(mu_);
        for (auto& [key, val] : new_config.items()) {
            if (!config_.contains(key) || config_[key] != val) {
                changed.emplace_back(key, val);
            }
        }
        config_ = new_config;
    }

    // Step 2: notify OUTSIDE mu_ lock (prevents deadlock when callbacks call get())
    std::lock_guard obs_lk(observers_mu_);
    for (auto& [key, val] : changed) {
        if (auto it = observers_.find(key); it != observers_.end()) {
            for (auto& cb : it->second) {
                cb(key, val);
            }
        }
    }
}

void HotConfig::start_watching() {
    if (config_path_.empty()) return;
    watcher_thread_ = std::jthread([this](std::stop_token st) {
        file_watch_loop(st);
    });
}

void HotConfig::stop_watching() {
    watcher_thread_.request_stop();
    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
}

void HotConfig::file_watch_loop(std::stop_token st) {
#ifdef __APPLE__
    int fd = open(config_path_.c_str(), O_RDONLY);
    if (fd < 0) return;

    int kq = kqueue();
    if (kq < 0) { close(fd); return; }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME, 0, nullptr);
    kevent(kq, &change, 1, nullptr, 0, nullptr);

    while (!st.stop_requested()) {
        struct timespec timeout = {1, 0};  // 1 second poll
        struct kevent event;
        int nev = kevent(kq, nullptr, 0, &event, 1, &timeout);
        if (nev > 0) {
            // Debounce: wait 200ms for editor to finish writing
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto r = reload();
            if (!r.has_value()) {
                LOG_WARN(fmt::format("HotConfig: reload failed: {}", r.error().message));
            }
        }
    }

    close(kq);
    close(fd);
#elif __linux__
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) return;

    int wd = inotify_add_watch(ifd, config_path_.c_str(), IN_MODIFY);
    if (wd < 0) { close(ifd); return; }

    char buf[4096];
    while (!st.stop_requested()) {
        int len = read(ifd, buf, sizeof(buf));
        if (len > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto r = reload();
            if (!r.has_value()) {
                LOG_WARN(fmt::format("HotConfig: reload failed: {}", r.error().message));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
#else
    // Fallback: polling
    auto last_write = std::filesystem::last_write_time(config_path_);
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto current = std::filesystem::last_write_time(config_path_);
        if (current != last_write) {
            last_write = current;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            (void)reload();
        }
    }
#endif
}

} // namespace agentos
```

- [ ] **Step 5: Add to CMakeLists.txt**

Add to `AGENTOS_SOURCES`:
```cmake
    src/core/hot_config.cpp
```

Add to `TEST_SOURCES`:
```cmake
    tests/test_hot_config.cpp
```

- [ ] **Step 6: Build and run tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='HotConfig*'`
Expected: 7 tests PASS

- [ ] **Step 7: Commit**

```bash
git add include/agentos/core/hot_config.hpp src/core/hot_config.cpp tests/test_hot_config.cpp CMakeLists.txt
git commit -m "feat(config): add HotConfig with file watch and validation"
```

---

## Task 8: HotConfig Integration with AgentOS + MCP

**Files:**
- Modify: `include/agentos/agentos.hpp` (AgentOSBuilder)
- Modify: `include/agentos/agent.hpp` (AgentOS constructor)
- Modify: `include/agentos/mcp/mcp_server.hpp`

- [ ] **Step 1: Add config_file() to AgentOSBuilder**

In `include/agentos/agentos.hpp`, add to `AgentOSBuilder`:

```cpp
    /// Set config file for hot reload
    AgentOSBuilder& config_file(std::string path) {
        config_file_ = std::move(path);
        return *this;
    }
```

Add member:
```cpp
    std::string config_file_;
```

In `build()`, after creating AgentOS, wire HotConfig if config_file is set. Since AgentOS constructor is complex, the simplest approach is to add a `configure_hot_reload()` method to AgentOS.

- [ ] **Step 2: Add HotConfig to AgentOS**

In `include/agentos/agent.hpp`, add to `AgentOS`:

```cpp
    #include <agentos/core/hot_config.hpp>

    // Add to public section:
    void configure_hot_reload(const std::string& config_path) {
        hot_config_ = std::make_unique<HotConfig>(config_path);

        // Wire all 7 hot-reloadable parameters from the spec
        hot_config_->on_change("tpm_limit", [this](const std::string&, const nlohmann::json& v) {
            kernel_->rate_limiter().set_rate(v.get<size_t>());
        });
        hot_config_->on_change("log_level", [](const std::string&, const nlohmann::json& v) {
            auto s = v.get<std::string>();
            LogLevel level = LogLevel::Warn;
            if (s == "debug") level = LogLevel::Debug;
            else if (s == "info") level = LogLevel::Info;
            else if (s == "warn") level = LogLevel::Warn;
            else if (s == "error") level = LogLevel::Error;
            else if (s == "off") level = LogLevel::Off;
            Logger::instance().set_level(level);
        });
        hot_config_->on_change("injection_detection_enabled", [this](const std::string&, const nlohmann::json& v) {
            if (security_) security_->detector().set_enabled(v.get<bool>());
        });
        hot_config_->on_change("injection_threshold", [this](const std::string&, const nlohmann::json& v) {
            if (security_) security_->detector().set_threshold(v.get<double>());
        });
        // Note: audit_rotation_* and context_default_token_limit observers
        // will take effect on next rotation/context-creation cycle.
        // They are stored in HotConfig and read on demand.

        hot_config_->reload();
        hot_config_->start_watching();
        // IMPORTANT: observer callbacks must NOT call hot_config_->set()
        // to prevent lock order inversion between mu_ and observers_mu_.
    }

    HotConfig* hot_config() { return hot_config_.get(); }

    // Add to private:
    std::unique_ptr<HotConfig> hot_config_;
```

- [ ] **Step 3: Wire in AgentOSBuilder::build()**

In `AgentOSBuilder::build()`, after `return std::make_unique<AgentOS>(...)`:

Change to:
```cpp
    auto os = std::make_unique<AgentOS>(std::move(be), cfg_);
    if (!config_file_.empty()) {
        os->configure_hot_reload(config_file_);
    }
    return os;
```

- [ ] **Step 4: Add config/* methods to MCPServer**

In `include/agentos/mcp/mcp_server.hpp`:

**a)** Add `HotConfig*` to constructor:
```cpp
    MCPServer(tools::ToolManager& tool_manager,
              std::string server_name,
              std::string version,
              HotConfig* hot_config = nullptr)
        : tool_manager_(tool_manager),
          server_name_(std::move(server_name)),
          version_(std::move(version)),
          hot_config_(hot_config) {}
```

**b)** Add routing in `handle()`:
```cpp
        if (req.method == "config/reload") return handle_config_reload(req);
        if (req.method == "config/get") return handle_config_get(req);
        if (req.method == "config/set") return handle_config_set(req);
```

**c)** Add handler methods:
```cpp
    MCPResponse handle_config_reload(const MCPRequest& req) {
        if (!hot_config_) {
            return make_error_response(req.id, error_code::InternalError, "hot config not enabled");
        }
        auto r = hot_config_->reload();
        if (!r.has_value()) {
            return make_error_response(req.id, error_code::InternalError, r.error().message);
        }
        return {.jsonrpc = "2.0", .result = Json{{"status", "reloaded"}}, .error = nullptr, .id = req.id};
    }

    MCPResponse handle_config_get(const MCPRequest& req) {
        if (!hot_config_) {
            return make_error_response(req.id, error_code::InternalError, "hot config not enabled");
        }
        auto key = req.params.value("key", "");
        if (key.empty()) {
            return make_error_response(req.id, error_code::InvalidParams, "missing 'key'");
        }
        auto val = hot_config_->get<Json>(key, nullptr);
        Json result;
        result["key"] = key;
        result["value"] = val;
        return {.jsonrpc = "2.0", .result = result, .error = nullptr, .id = req.id};
    }

    MCPResponse handle_config_set(const MCPRequest& req) {
        if (!hot_config_) {
            return make_error_response(req.id, error_code::InternalError, "hot config not enabled");
        }
        auto key = req.params.value("key", "");
        if (key.empty() || !req.params.contains("value")) {
            return make_error_response(req.id, error_code::InvalidParams, "missing 'key' or 'value'");
        }
        auto r = hot_config_->set(key, req.params["value"]);
        if (!r.has_value()) {
            return make_error_response(req.id, error_code::InvalidParams, r.error().message);
        }
        return {.jsonrpc = "2.0", .result = Json{{"status", "updated"}}, .error = nullptr, .id = req.id};
    }
```

**d)** Add member:
```cpp
    HotConfig* hot_config_;
```

**e)** Add include:
```cpp
#include <agentos/core/hot_config.hpp>
```

- [ ] **Step 5: Build and run all tests**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*HotConfig*:*MCP*'`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add include/agentos/agentos.hpp include/agentos/agent.hpp include/agentos/mcp/mcp_server.hpp
git commit -m "feat(config): integrate HotConfig with AgentOS and MCP server"
```

---

## Task 9: vcpkg Package Management

**Files:**
- Create: `vcpkg.json`
- Create: `CMakePresets.json`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create vcpkg.json**

Create `vcpkg.json`:

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "agentos",
  "version": "0.1.0",
  "description": "C++23 LLM Agent Operating System",
  "builtin-baseline": "c82f74667287d3dc386bce81e44964370c91a289",
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

Note: `builtin-baseline` is intentionally omitted — it will be pinned when vcpkg is actually used in CI. Users set `VCPKG_ROOT` and use the preset.

- [ ] **Step 2: Create CMakePresets.json**

Create `CMakePresets.json`:

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

- [ ] **Step 3: Add if(NOT TARGET) guards to CMakeLists.txt**

Wrap FetchContent blocks:

**nlohmann_json:**
```cmake
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
```

**googletest:**
```cmake
if(NOT TARGET GTest::gtest)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()
```

**hnswlib:**
```cmake
if(NOT TARGET hnswlib::hnswlib)
    FetchContent_Declare(
      hnswlib
      URL https://github.com/nmslib/hnswlib/archive/refs/tags/v0.8.0.zip
    )
    FetchContent_MakeAvailable(hnswlib)
    get_target_property(_hnswlib_dirs hnswlib INTERFACE_INCLUDE_DIRECTORIES)
    if(_hnswlib_dirs)
        set_target_properties(hnswlib PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hnswlib_dirs}")
    endif()
endif()
```

**sqlite3 (try vcpkg first, then pkg-config):**
```cmake
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(SQLITE3_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SQLITE3 REQUIRED sqlite3)
    set(SQLITE3_TARGET ${SQLITE3_LIBRARIES})
endif()
```

Then update the `target_link_libraries` for `agentos` to use `${SQLITE3_TARGET}` instead of `${SQLITE3_LIBRARIES}`.

- [ ] **Step 4: Verify FetchContent build still works**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos`
Expected: ALL 719+ tests PASS (nothing should break)

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json CMakePresets.json CMakeLists.txt
git commit -m "build: add vcpkg manifest and CMake presets with FetchContent fallback"
```

---

## Task 10: Full Integration Test

**Files:**
- Modify: `tests/test_log_sink.cpp` or `tests/test_integration.cpp`

- [ ] **Step 1: Run the full test suite**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos`
Expected: ALL tests PASS, 0 failures

- [ ] **Step 2: Verify no compiler warnings**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos 2>&1 | grep -i "warning:" | head -20`
Expected: No warnings from agentos sources (third-party warnings OK)

- [ ] **Step 3: Run with ASAN (if available)**

Run: `cd build && cmake .. -DAGENTOS_ENABLE_ASAN=ON && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*Sink*:*Audit*:*HotConfig*'`
Expected: No memory errors

- [ ] **Step 4: Commit final integration state**

```bash
git add -A
git commit -m "test: verify P0 infrastructure integration — all tests pass"
```
