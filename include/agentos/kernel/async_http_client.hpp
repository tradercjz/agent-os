#pragma once
// ============================================================
// AgentOS :: Kernel -- Async HTTP Client (curl_multi)
// Non-blocking HTTP transport with coroutine integration
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/core/task.hpp>
#include <agentos/kernel/http_client.hpp>
#include <curl/curl.h>
#include <atomic>
#include <coroutine>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agentos::kernel {

// Non-blocking HTTP client using curl_multi.
// Runs an event loop on a dedicated thread; dispatches results
// via coroutine continuation (symmetric transfer).
class AsyncHttpClient {
public:
    AsyncHttpClient();
    ~AsyncHttpClient();

    AsyncHttpClient(const AsyncHttpClient&) = delete;
    AsyncHttpClient& operator=(const AsyncHttpClient&) = delete;

    // Non-blocking POST -- returns a Task that completes when the
    // response arrives.  The coroutine suspends until the event loop
    // finishes the transfer, then resumes with the result.
    Task<Result<HttpResponse>> post_async(std::string url,
                                          std::string body,
                                          std::vector<std::string> headers,
                                          int timeout_sec = 60);

    // Blocking convenience -- submits the request and waits for the
    // result on a std::future.  Useful from non-coroutine callers.
    Result<HttpResponse> post_blocking(std::string url,
                                       std::string body,
                                       std::vector<std::string> headers,
                                       int timeout_sec = 60);

    // Number of in-flight requests (approximate)
    size_t active_requests() const;

    // Stop the event loop (idempotent).
    void stop();

private:
    struct PendingRequest {
        CURL* easy{nullptr};
        struct curl_slist* header_list{nullptr};
        std::string response_body;
        std::string url;
        // Coroutine path
        std::coroutine_handle<> continuation;
        Result<HttpResponse>* result_ptr{nullptr};
        // Future path (for post_blocking)
        std::promise<Result<HttpResponse>>* promise_ptr{nullptr};
    };

    void event_loop();
    void process_completed(CURLMsg* msg);
    void cleanup_request(PendingRequest* req);
    CURL* make_easy(PendingRequest* req, int timeout_sec);

    CURLM* multi_{nullptr};
    std::jthread loop_thread_;
    mutable std::mutex mu_;
    std::vector<PendingRequest*> new_requests_;  // staging area
    std::atomic<size_t> active_{0};
    std::atomic<bool> stop_{false};

    // Pipe for waking up curl_multi_poll when new requests arrive
    int wake_pipe_[2]{-1, -1};
};

} // namespace agentos::kernel
