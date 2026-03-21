#pragma once
// ============================================================
// AgentOS :: Context — Conversation History Persistence
// Save/load conversation records as JSON files
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <string>
#include <vector>

namespace agentos::context {

struct ConversationRecord {
    std::string conversation_id;
    AgentId agent_id{0};
    std::string agent_name;
    std::vector<kernel::Message> messages;
    std::string created_at;    // ISO-8601
    std::string updated_at;    // ISO-8601
};

class ConversationStore : private NonCopyable {
public:
    explicit ConversationStore(const std::string& dir);

    /// Save a conversation (atomic write: temp + rename)
    [[nodiscard]] Result<void> save(const ConversationRecord& record);

    /// Load a conversation by ID
    [[nodiscard]] Result<ConversationRecord> load(const std::string& conversation_id);

    /// List all conversation IDs
    std::vector<std::string> list() const;

    /// Delete a conversation
    [[nodiscard]] Result<void> remove(const std::string& conversation_id);

    /// Get conversation count
    size_t count() const;

private:
    std::string dir_;
    std::string path_for(const std::string& id) const;

    static std::string role_to_string(kernel::Role r);
    static kernel::Role string_to_role(const std::string& s);
};

} // namespace agentos::context
