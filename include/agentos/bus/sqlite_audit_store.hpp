#pragma once
// ============================================================
// AgentOS :: SqliteAuditStore — SQLite-backed audit persistence
// WAL mode, indexed, with rotation support
// ============================================================
#include <agentos/bus/audit_store.hpp>
#include <sqlite3.h>
#include <mutex>
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
    mutable std::mutex mu_;  // serialize all SQLite operations (thread-safe)
    SqliteGuard db_;

    Result<void> write_one(const AuditEntry& entry);  // internal, caller holds mu_
    static std::string message_type_to_str(MessageType t);
    static MessageType str_to_message_type(const std::string& s);
    static std::string timepoint_to_iso(WallTimePoint tp);
    static WallTimePoint iso_to_timepoint(const std::string& s);

    void build_where_clause(const AuditFilter& filter,
                            std::string& sql,
                            std::vector<std::string>& bind_values) const;
};

} // namespace agentos::bus
