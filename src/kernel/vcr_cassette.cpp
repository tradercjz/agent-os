// ============================================================
// AgentOS :: Kernel — VCR Cassette Implementation
// ============================================================
#include <agentos/kernel/vcr_cassette.hpp>
#include <agentos/core/logger.hpp>
#include <filesystem>
#include <fstream>

namespace agentos::kernel {

void VCRCassette::record(const std::string& url, const std::string& body,
                          const HttpResponse& response) {
    std::lock_guard lk(mu_);
    entries_.push_back(VCREntry{url, body, response});
}

Result<HttpResponse> VCRCassette::replay() {
    std::lock_guard lk(mu_);
    if (replay_index_ >= entries_.size()) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("VCR cassette exhausted: {} entries, index {}",
                                      entries_.size(), replay_index_));
    }
    auto& entry = entries_[replay_index_++];
    return HttpResponse{entry.response.status_code, entry.response.body};
}

void VCRCassette::save() const {
    std::lock_guard lk(mu_);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries_) {
        arr.push_back({
            {"url", e.url},
            {"request_body", e.request_body},
            {"response", {
                {"status_code", e.response.status_code},
                {"body", e.response.body}
            }}
        });
    }

    // Atomic write: temp file + rename
    namespace fs = std::filesystem;
    auto tmp_path = path_ + ".tmp";
    {
        std::ofstream ofs(tmp_path);
        if (!ofs) {
            LOG_ERROR(fmt::format("[VCR] Failed to open temp file for writing: {}", tmp_path));
            return;
        }
        ofs << arr.dump(2);
        if (!ofs.good()) {
            LOG_ERROR(fmt::format("[VCR] Write failed for: {}", tmp_path));
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp_path, path_, ec);
    if (ec) {
        LOG_ERROR(fmt::format("[VCR] Rename failed: {} -> {}: {}", tmp_path, path_, ec.message()));
    }
}

void VCRCassette::load() {
    std::lock_guard lk(mu_);

    std::ifstream ifs(path_);
    if (!ifs) {
        LOG_WARN(fmt::format("[VCR] Cassette file not found: {}", path_));
        return;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(ifs);
    } catch (const nlohmann::json::exception& ex) {
        LOG_ERROR(fmt::format("[VCR] Failed to parse cassette: {}", ex.what()));
        return;
    }

    entries_.clear();
    replay_index_ = 0;

    for (const auto& item : arr) {
        VCREntry entry;
        entry.url = item.value("url", "");
        entry.request_body = item.value("request_body", "");
        if (item.contains("response")) {
            const auto& resp = item["response"];
            entry.response.status_code = resp.value("status_code", 0L);
            entry.response.body = resp.value("body", "");
        }
        entries_.push_back(std::move(entry));
    }
}

size_t VCRCassette::size() const {
    std::lock_guard lk(mu_);
    return entries_.size();
}

} // namespace agentos::kernel
