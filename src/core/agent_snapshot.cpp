// ============================================================
// AgentOS :: Core — Agent State Snapshot Implementation
// ============================================================
#include <agentos/core/agent_snapshot.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace agentos {

// ── ISO-8601 timestamp helper ────────────────────────────────
static std::string iso8601_now() {
    auto sys_now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(sys_now);
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

// ── Serialization ────────────────────────────────────────────

std::string AgentSnapshot::to_json() const {
    nlohmann::json j;
    j["agent_id"] = agent_id;
    j["name"] = name;
    j["role_prompt"] = role_prompt;
    j["security_role"] = security_role;
    j["context_limit"] = context_limit;
    j["allowed_tools"] = allowed_tools;
    j["snapshot_version"] = snapshot_version;
    j["created_at"] = created_at.empty() ? iso8601_now() : created_at;

    // Messages as array of {role, content} objects
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& [role, content] : messages) {
        msgs.push_back({{"role", role}, {"content", content}});
    }
    j["messages"] = msgs;

    j["memory_entries_json"] = memory_entries_json;

    return j.dump(2);
}

Result<AgentSnapshot> AgentSnapshot::from_json(const std::string& json_str) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(ErrorCode::InvalidArgument,
                          fmt::format("AgentSnapshot::from_json: parse error: {}", e.what()));
    }

    AgentSnapshot snap;
    try {
        snap.agent_id = j.value("agent_id", AgentId{0});
        snap.name = j.value("name", "");
        snap.role_prompt = j.value("role_prompt", "");
        snap.security_role = j.value("security_role", "standard");
        snap.context_limit = j.value("context_limit", TokenCount{8192});
        snap.snapshot_version = j.value("snapshot_version", "1.0");
        snap.created_at = j.value("created_at", "");

        if (j.contains("allowed_tools") && j["allowed_tools"].is_array()) {
            snap.allowed_tools = j["allowed_tools"].get<std::vector<std::string>>();
        }

        if (j.contains("messages") && j["messages"].is_array()) {
            for (const auto& m : j["messages"]) {
                snap.messages.emplace_back(
                    m.value("role", ""),
                    m.value("content", ""));
            }
        }

        if (j.contains("memory_entries_json") && j["memory_entries_json"].is_array()) {
            snap.memory_entries_json = j["memory_entries_json"].get<std::vector<std::string>>();
        }
    } catch (const nlohmann::json::type_error& e) {
        return make_error(ErrorCode::InvalidArgument,
                          fmt::format("AgentSnapshot::from_json: type error: {}", e.what()));
    }

    return snap;
}

// ── File I/O (atomic write) ──────────────────────────────────

Result<void> AgentSnapshot::save(const std::string& path) const {
    namespace fs = std::filesystem;

    // Ensure parent directory exists
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            return make_error(ErrorCode::MemoryWriteFailed,
                              fmt::format("AgentSnapshot::save: cannot create directory '{}': {}",
                                          parent.string(), ec.message()));
        }
    }

    // Atomic write: temp file + rename
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        if (!ofs) {
            return make_error(ErrorCode::MemoryWriteFailed,
                              fmt::format("AgentSnapshot::save: cannot open '{}'", tmp_path));
        }
        ofs << to_json();
        ofs.flush();
        if (!ofs.good()) {
            std::remove(tmp_path.c_str());
            return make_error(ErrorCode::MemoryWriteFailed,
                              fmt::format("AgentSnapshot::save: write failed to '{}'", tmp_path));
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        std::remove(tmp_path.c_str());
        return make_error(ErrorCode::MemoryWriteFailed,
                          fmt::format("AgentSnapshot::save: rename failed: {}", ec.message()));
    }

    return {};
}

Result<AgentSnapshot> AgentSnapshot::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("AgentSnapshot::load: cannot open '{}'", path));
    }

    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (!ifs.good() && !ifs.eof()) {
        return make_error(ErrorCode::MemoryReadFailed,
                          fmt::format("AgentSnapshot::load: read error from '{}'", path));
    }

    return from_json(ss.str());
}

} // namespace agentos
