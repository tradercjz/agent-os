#pragma once
// ============================================================
// AgentOS :: Kernel — HTTP Client
// Shared HTTP transport layer (libcurl RAII wrappers)
// ============================================================
#include <agentos/core/types.hpp>
#include <functional>
#include <string>
#include <vector>

namespace agentos::kernel {

/// HTTP response from a POST request
struct HttpResponse {
    long status_code{0};
    std::string body;
};

/// Reusable HTTP client wrapping libcurl.
/// Thread-safe: each call creates its own CURL easy handle.
class HttpClient {
public:
    HttpClient();

    /// Synchronous POST; collects full response body.
    [[nodiscard]] Result<HttpResponse> post(const std::string& url,
                                            const std::string& body,
                                            const std::vector<std::string>& headers,
                                            int timeout_sec = 60) const;

    /// Streaming POST; feeds chunks to caller via on_data callback.
    /// The callback receives (data, length) and returns the number of
    /// bytes consumed (must equal length to continue the transfer).
    [[nodiscard]] Result<HttpResponse> post_stream(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers,
        std::function<size_t(const char*, size_t)> on_data,
        int timeout_sec = 120) const;
};

} // namespace agentos::kernel
