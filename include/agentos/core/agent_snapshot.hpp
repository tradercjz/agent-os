#pragma once
// ============================================================
// AgentOS :: Core — Agent State Snapshot
// Save/restore agent configuration, context, and working memory
// ============================================================
#include <agentos/core/types.hpp>
#include <string>
#include <utility>
#include <vector>

namespace agentos {

struct AgentSnapshot {
    AgentId agent_id{0};
    std::string name;
    std::string role_prompt;
    std::string security_role{"standard"};
    TokenCount context_limit{8192};
    std::vector<std::string> allowed_tools;

    // Context window state (messages as role/content pairs)
    std::vector<std::pair<std::string, std::string>> messages;

    // Working memory entries (serialized as JSON strings)
    std::vector<std::string> memory_entries_json;

    // Metadata
    std::string created_at;
    std::string snapshot_version{"1.0"};

    // Serialize to/from JSON
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static Result<AgentSnapshot> from_json(const std::string& json_str);

    // Save/load to file (atomic write: temp + rename)
    [[nodiscard]] Result<void> save(const std::string& path) const;
    [[nodiscard]] static Result<AgentSnapshot> load(const std::string& path);
};

} // namespace agentos
