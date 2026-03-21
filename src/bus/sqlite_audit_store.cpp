// ============================================================
// AgentOS :: SqliteAuditStore implementation
// ============================================================
#include <agentos/bus/sqlite_audit_store.hpp>
#include <agentos/core/logger.hpp>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace agentos::bus {

// ── Constructor: create schema + indexes + WAL mode ──────────

SqliteAuditStore::SqliteAuditStore(const std::string& db_path)
    : db_(db_path)
{
    const char* pragmas[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
    };
    for (const auto* p : pragmas) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_.db, p, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error(fmt::format("SqliteAuditStore pragma failed: {}", msg));
        }
    }

    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS audit_log (
            id        INTEGER PRIMARY KEY,
            timestamp TEXT NOT NULL,
            from_agent INTEGER,
            to_agent   INTEGER,
            type       TEXT NOT NULL,
            topic      TEXT,
            payload    TEXT,
            redacted   INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(timestamp);
        CREATE INDEX IF NOT EXISTS idx_audit_agent ON audit_log(from_agent, to_agent);
        CREATE INDEX IF NOT EXISTS idx_audit_type_ts ON audit_log(type, timestamp);
    )SQL";

    char* err = nullptr;
    int rc = sqlite3_exec(db_.db, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error(fmt::format("SqliteAuditStore schema creation failed: {}", msg));
    }
}

// ── MessageType conversion ───────────────────────────────────

std::string SqliteAuditStore::message_type_to_str(MessageType t) {
    switch (t) {
        case MessageType::Request:   return "Request";
        case MessageType::Response:  return "Response";
        case MessageType::Event:     return "Event";
        case MessageType::Heartbeat: return "Heartbeat";
    }
    return "Unknown";
}

MessageType SqliteAuditStore::str_to_message_type(const std::string& s) {
    if (s == "Request")   return MessageType::Request;
    if (s == "Response")  return MessageType::Response;
    if (s == "Event")     return MessageType::Event;
    if (s == "Heartbeat") return MessageType::Heartbeat;
    return MessageType::Request; // fallback
}

// ── Time conversion ──────────────────────────────────────────

std::string SqliteAuditStore::timepoint_to_iso(WallTimePoint tp) {
    auto epoch = tp.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch) - secs;

    std::time_t tt = static_cast<std::time_t>(secs.count());
    struct tm utc{};
    gmtime_r(&tt, &utc);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(millis.count()));
    return buf;
}

WallTimePoint SqliteAuditStore::iso_to_timepoint(const std::string& s) {
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0, ms = 0;
    std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
                &year, &mon, &day, &hour, &min, &sec, &ms);

    struct tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon  = mon - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min  = min;
    utc.tm_sec  = sec;

    std::time_t tt = timegm(&utc);
    auto tp = std::chrono::system_clock::from_time_t(tt);
    tp += std::chrono::milliseconds(ms);
    return tp;
}

// ── write ────────────────────────────────────────────────────

// Internal write without locking (caller must hold mu_)
Result<void> SqliteAuditStore::write_one(const AuditEntry& entry) {
    const char* sql = "INSERT INTO audit_log (id, timestamp, from_agent, to_agent, type, topic, payload, redacted) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return make_error(ErrorCode::MemoryWriteFailed,
            fmt::format("SqliteAuditStore::write prepare failed: {}", sqlite3_errmsg(db_.db)));
    }

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(entry.id));
    sqlite3_bind_text(stmt, 2, timepoint_to_iso(entry.timestamp).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(entry.from_agent));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(entry.to_agent));
    sqlite3_bind_text(stmt, 5, message_type_to_str(entry.type).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, entry.topic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, entry.payload.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, entry.redacted ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return make_error(ErrorCode::MemoryWriteFailed,
            fmt::format("SqliteAuditStore::write step failed: {}", sqlite3_errmsg(db_.db)));
    }

    return Result<void>{};
}

Result<void> SqliteAuditStore::write(const AuditEntry& entry) {
    std::lock_guard lk(mu_);
    return write_one(entry);
}

// ── write_batch ──────────────────────────────────────────────

Result<void> SqliteAuditStore::write_batch(std::span<const AuditEntry> entries) {
    std::lock_guard lk(mu_);
    char* err = nullptr;
    int rc = sqlite3_exec(db_.db, "BEGIN TRANSACTION", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        return make_error(ErrorCode::MemoryWriteFailed,
            fmt::format("SqliteAuditStore::write_batch BEGIN failed: {}", msg));
    }

    for (const auto& entry : entries) {
        auto r = write_one(entry);
        if (!r.has_value()) {
            // Rollback
            sqlite3_exec(db_.db, "ROLLBACK", nullptr, nullptr, nullptr);
            return r;
        }
    }

    rc = sqlite3_exec(db_.db, "COMMIT", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        (void)sqlite3_exec(db_.db, "ROLLBACK", nullptr, nullptr, nullptr);
        return make_error(ErrorCode::MemoryWriteFailed,
            fmt::format("SqliteAuditStore::write_batch COMMIT failed: {}", msg));
    }

    return Result<void>{};
}

// ── build_where_clause ───────────────────────────────────────

void SqliteAuditStore::build_where_clause(const AuditFilter& filter,
                                           std::string& sql,
                                           std::vector<std::string>& bind_values) const {
    std::vector<std::string> conditions;

    if (filter.agent_id.has_value()) {
        auto id_str = std::to_string(*filter.agent_id);
        conditions.push_back("(from_agent = ? OR to_agent = ?)");
        bind_values.push_back(id_str);
        bind_values.push_back(id_str);
    }

    if (filter.type.has_value()) {
        conditions.push_back("type = ?");
        bind_values.push_back(message_type_to_str(*filter.type));
    }

    if (filter.after.has_value()) {
        conditions.push_back("timestamp > ?");
        bind_values.push_back(timepoint_to_iso(*filter.after));
    }

    if (filter.before.has_value()) {
        conditions.push_back("timestamp < ?");
        bind_values.push_back(timepoint_to_iso(*filter.before));
    }

    if (filter.topic.has_value()) {
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

// ── query ────────────────────────────────────────────────────

std::vector<AuditEntry> SqliteAuditStore::query(const AuditFilter& filter) {
    std::lock_guard lk(mu_);
    std::string sql = "SELECT id, timestamp, from_agent, to_agent, type, topic, payload, redacted FROM audit_log";
    std::vector<std::string> bind_values;
    build_where_clause(filter, sql, bind_values);
    sql += " ORDER BY id ASC";
    sql += fmt::format(" LIMIT {} OFFSET {}", filter.limit, filter.offset);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(fmt::format("SqliteAuditStore::query prepare failed: {}", sqlite3_errmsg(db_.db)));
        return {};
    }

    for (size_t i = 0; i < bind_values.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), bind_values[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<AuditEntry> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        AuditEntry entry{};
        entry.id = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));

        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        entry.timestamp = ts ? iso_to_timepoint(ts) : WallTimePoint{};

        entry.from_agent = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        entry.to_agent = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));

        const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        entry.type = type_str ? str_to_message_type(type_str) : MessageType::Request;

        const char* topic = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        entry.topic = topic ? topic : "";

        const char* payload = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        entry.payload = payload ? payload : "";

        entry.redacted = sqlite3_column_int(stmt, 7) != 0;

        results.push_back(std::move(entry));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ── count ────────────────────────────────────────────────────

size_t SqliteAuditStore::count(const AuditFilter& filter) {
    std::lock_guard lk(mu_);
    std::string sql = "SELECT COUNT(*) FROM audit_log";
    std::vector<std::string> bind_values;
    build_where_clause(filter, sql, bind_values);

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR(fmt::format("SqliteAuditStore::count prepare failed: {}", sqlite3_errmsg(db_.db)));
        return 0;
    }

    for (size_t i = 0; i < bind_values.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), bind_values[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    size_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return result;
}

// ── rotate ───────────────────────────────────────────────────

Result<void> SqliteAuditStore::rotate(const RotationPolicy& policy) {
    std::lock_guard lk(mu_);
    // 1. Delete by max_entries: keep newest, delete oldest
    {
        std::string sql = fmt::format(
            "DELETE FROM audit_log WHERE id NOT IN "
            "(SELECT id FROM audit_log ORDER BY id DESC LIMIT {})",
            policy.max_entries);

        char* err = nullptr;
        int rc = sqlite3_exec(db_.db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            return make_error(ErrorCode::MemoryWriteFailed,
                fmt::format("SqliteAuditStore::rotate max_entries failed: {}", msg));
        }
    }

    // 2. Delete by max_age: delete older than cutoff
    {
        auto cutoff = std::chrono::system_clock::now() - policy.max_age;
        std::string cutoff_iso = timepoint_to_iso(cutoff);

        std::string sql = "DELETE FROM audit_log WHERE timestamp < ?";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.db, sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return make_error(ErrorCode::MemoryWriteFailed,
                fmt::format("SqliteAuditStore::rotate max_age prepare failed: {}", sqlite3_errmsg(db_.db)));
        }

        sqlite3_bind_text(stmt, 1, cutoff_iso.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            return make_error(ErrorCode::MemoryWriteFailed,
                fmt::format("SqliteAuditStore::rotate max_age step failed: {}", sqlite3_errmsg(db_.db)));
        }
    }

    return Result<void>{};
}

// ── flush ────────────────────────────────────────────────────

void SqliteAuditStore::flush() {
    std::lock_guard lk(mu_);
    sqlite3_wal_checkpoint_v2(db_.db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
}

} // namespace agentos::bus
