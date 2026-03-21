#include <agentos/kernel/http_client.hpp>
#include <agentos/core/logger.hpp>
#include <curl/curl.h>
#include <mutex>

// ============================================================
// File-local RAII helpers (libcurl resource management)
// ============================================================

namespace {

/// RAII wrapper for CURL* easy handle
struct CurlHandle {
    CURL *handle = nullptr;
    char errbuf[CURL_ERROR_SIZE] = {};

    CurlHandle() : handle(curl_easy_init()) {
        if (handle) {
            curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        }
    }
    ~CurlHandle() {
        if (handle)
            curl_easy_cleanup(handle);
    }

    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;

    explicit operator bool() const { return handle != nullptr; }
    CURL *get() const { return handle; }
};

/// RAII wrapper for curl_slist* header list
struct CurlHeaders {
    curl_slist *list = nullptr;

    CurlHeaders() = default;

    void append(const char *header) {
        list = curl_slist_append(list, header);
    }
    ~CurlHeaders() {
        if (list)
            curl_slist_free_all(list);
    }

    CurlHeaders(const CurlHeaders &) = delete;
    CurlHeaders &operator=(const CurlHeaders &) = delete;
};

/// Standard write callback: append response data to std::string
size_t write_string_callback(void *contents, size_t size, size_t nmemb,
                              void *userp) {
    auto *buf = static_cast<std::string *>(userp);
    size_t total = size * nmemb;
    buf->append(static_cast<char *>(contents), total);
    return total;
}

/// Streaming write callback context
struct StreamCallbackCtx {
    std::function<size_t(const char*, size_t)> on_data;
};

/// Streaming write callback: forward chunks to caller's callback
size_t stream_callback_wrapper(void *contents, size_t size, size_t nmemb,
                                void *userp) {
    auto *ctx = static_cast<StreamCallbackCtx *>(userp);
    if (nmemb != 0 && size > (SIZE_MAX / nmemb)) return 0;
    size_t total = size * nmemb;
    return ctx->on_data(static_cast<const char *>(contents), total);
}

/// Ensure curl_global_init is called exactly once
void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

} // anonymous namespace

// ============================================================
// HttpClient implementation
// ============================================================

namespace agentos::kernel {

HttpClient::HttpClient() {
    ensure_curl_global_init();
}

Result<HttpResponse> HttpClient::post(const std::string& url,
                                       const std::string& body,
                                       const std::vector<std::string>& headers,
                                       int timeout_sec) const {
    CurlHandle curl;
    if (!curl)
        return make_error(ErrorCode::LLMBackendError, "Failed to init libcurl");

    std::string response_body;

    CurlHeaders hdr;
    for (const auto& h : headers) {
        hdr.append(h.c_str());
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdr.list);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_string_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);

    CURLcode res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) {
        return make_error(
            ErrorCode::LLMBackendError,
            fmt::format("HTTP request failed: {} ({})", curl.errbuf,
                        curl_easy_strerror(res)));
    }

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

    return HttpResponse{http_code, std::move(response_body)};
}

Result<HttpResponse> HttpClient::post_stream(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers,
    std::function<size_t(const char*, size_t)> on_data,
    int timeout_sec) const {

    CurlHandle curl;
    if (!curl)
        return make_error(ErrorCode::LLMBackendError, "Failed to init libcurl");

    CurlHeaders hdr;
    for (const auto& h : headers) {
        hdr.append(h.c_str());
    }

    StreamCallbackCtx ctx;
    ctx.on_data = std::move(on_data);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdr.list);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, stream_callback_wrapper);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);

    CURLcode res = curl_easy_perform(curl.get());

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

    // CURLE_WRITE_ERROR can be expected if the callback intentionally
    // aborted the transfer (e.g. after receiving [DONE] in SSE).
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        return make_error(
            ErrorCode::LLMBackendError,
            fmt::format("HTTP stream request failed: {} ({})", curl.errbuf,
                        curl_easy_strerror(res)));
    }

    return HttpResponse{http_code, {}};
}

} // namespace agentos::kernel
