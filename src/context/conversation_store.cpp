#include <agentos/context/conversation_store.hpp>
#include <agentos/core/logger.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace agentos::context {

ConversationStore::ConversationStore(const std::string& dir) : dir_(dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // Best-effort: directory may already exist or caller handles errors
}

std::string ConversationStore::path_for(const std::string& id) const {
    return (std::filesystem::path(dir_) / (id + ".json")).string();
}

std::string ConversationStore::role_to_string(kernel::Role r) {
    switch (r) {
        case kernel::Role::System:    return "system";
        case kernel::Role::User:      return "user";
        case kernel::Role::Assistant: return "assistant";
        case kernel::Role::Tool:      return "tool";
    }
    return "user"; // unreachable but satisfies compiler
}

kernel::Role ConversationStore::string_to_role(const std::string& s) {
    if (s == "system")    return kernel::Role::System;
    if (s == "assistant") return kernel::Role::Assistant;
    if (s == "tool")      return kernel::Role::Tool;
    return kernel::Role::User;
}

Result<void> ConversationStore::save(const ConversationRecord& record) {
    nlohmann::json j;
    j["conversation_id"] = record.conversation_id;
    j["agent_id"] = record.agent_id;
    j["agent_name"] = record.agent_name;
    j["created_at"] = record.created_at;
    j["updated_at"] = record.updated_at;

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : record.messages) {
        nlohmann::json mj;
        mj["role"] = role_to_string(m.role);
        mj["content"] = m.content;
        if (!m.tool_call_id.empty()) {
            mj["tool_call_id"] = m.tool_call_id;
        }
        if (!m.name.empty()) {
            mj["name"] = m.name;
        }
        msgs.push_back(std::move(mj));
    }
    j["messages"] = std::move(msgs);

    // Atomic write: temp file + good() check + rename
    auto target = path_for(record.conversation_id);
    auto tmp = target + ".tmp";

    {
        std::ofstream ofs(tmp);
        if (!ofs) {
            return make_error(ErrorCode::MemoryWriteFailed,
                              fmt::format("ConversationStore: cannot open temp file '{}'", tmp));
        }
        ofs << j.dump(2);
        if (!ofs.good()) {
            std::filesystem::remove(tmp);
            return make_error(ErrorCode::MemoryWriteFailed,
                              fmt::format("ConversationStore: write error to '{}'", tmp));
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return make_error(ErrorCode::MemoryWriteFailed,
                          fmt::format("ConversationStore: rename failed: {}", ec.message()));
    }

    return {};
}

Result<ConversationRecord> ConversationStore::load(const std::string& conversation_id) {
    auto path = path_for(conversation_id);

    std::ifstream ifs(path);
    if (!ifs) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("ConversationStore: conversation '{}' not found", conversation_id));
    }

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(ErrorCode::MemoryReadFailed,
                          fmt::format("ConversationStore: parse error: {}", e.what()));
    }

    ConversationRecord record;
    record.conversation_id = j.value("conversation_id", "");
    record.agent_id = j.value("agent_id", AgentId{0});
    record.agent_name = j.value("agent_name", "");
    record.created_at = j.value("created_at", "");
    record.updated_at = j.value("updated_at", "");

    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& mj : j["messages"]) {
            kernel::Message msg;
            msg.role = string_to_role(mj.value("role", "user"));
            msg.content = mj.value("content", "");
            msg.tool_call_id = mj.value("tool_call_id", "");
            msg.name = mj.value("name", "");
            record.messages.push_back(std::move(msg));
        }
    }

    return record;
}

std::vector<std::string> ConversationStore::list() const {
    std::vector<std::string> ids;
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(dir_, ec);
    if (ec) return ids;

    for (const auto& entry : it) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            ids.push_back(entry.path().stem().string());
        }
    }
    return ids;
}

Result<void> ConversationStore::remove(const std::string& conversation_id) {
    auto path = path_for(conversation_id);
    std::error_code ec;
    if (!std::filesystem::remove(path, ec)) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("ConversationStore: conversation '{}' not found for removal", conversation_id));
    }
    return {};
}

size_t ConversationStore::count() const {
    return list().size();
}

} // namespace agentos::context
