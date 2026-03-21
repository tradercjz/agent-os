#pragma once
// ============================================================
// AgentOS :: Kernel — VCR Cassette (Record/Replay HTTP)
// Stores request/response pairs for deterministic test replay
// ============================================================
#include <agentos/kernel/http_client.hpp>
#include <agentos/core/types.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace agentos::kernel {

/// A single recorded HTTP interaction.
struct VCREntry {
    std::string url;
    std::string request_body;
    HttpResponse response;
};

/// Records HTTP request/response pairs to a JSON cassette file,
/// or replays previously recorded responses in sequential order.
///
/// Usage (record mode):
///   VCRCassette c("path.json");
///   c.record(url, body, response);
///   c.save();
///
/// Usage (replay mode):
///   VCRCassette c("path.json");
///   c.load();
///   auto r = c.replay();  // returns Result<HttpResponse>
class VCRCassette {
public:
    explicit VCRCassette(std::string path) : path_(std::move(path)) {}

    /// Record a request/response pair.
    void record(const std::string& url, const std::string& body,
                const HttpResponse& response);

    /// Replay the next recorded response (sequential).
    /// Returns error if all entries have been exhausted.
    [[nodiscard]] Result<HttpResponse> replay();

    /// Save all recorded entries to the JSON cassette file.
    void save() const;

    /// Load entries from the JSON cassette file.
    void load();

    /// Number of recorded entries.
    [[nodiscard]] size_t size() const;

private:
    std::string path_;
    std::vector<VCREntry> entries_;
    size_t replay_index_{0};
    mutable std::mutex mu_;
};

} // namespace agentos::kernel
