#pragma once
// ============================================================
// AgentOS :: Audit Store — IAuditStore interface + AuditEntry
// Structured audit trail for bus messages
// ============================================================
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
    std::chrono::hours max_age{24 * 30};
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
