#include <agentos/kernel/async_http_client.hpp>
#include <agentos/core/logger.hpp>
#include <fcntl.h>
#include <unistd.h>

// ============================================================
// File-local helpers
// ============================================================

namespace {

/// Standard write callback: append response data to std::string
size_t async_write_callback(void* contents, size_t size, size_t nmemb,
                            void* userp) {
    auto* buf = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    buf->append(static_cast<char*>(contents), total);
    return total;
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
// AsyncHttpClient implementation
// ============================================================

namespace agentos::kernel {

AsyncHttpClient::AsyncHttpClient() {
    ensure_curl_global_init();

    multi_ = curl_multi_init();
    if (!multi_) {
        LOG_ERROR("AsyncHttpClient: curl_multi_init failed");
        return;
    }

    // Create wake pipe (non-blocking read end)
    if (::pipe(wake_pipe_) != 0) {
        LOG_ERROR("AsyncHttpClient: pipe() failed");
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
        return;
    }
    ::fcntl(wake_pipe_[0], F_SETFL, O_NONBLOCK);

    // Start the event loop thread
    loop_thread_ = std::jthread([this](std::stop_token) { event_loop(); });
}

AsyncHttpClient::~AsyncHttpClient() {
    stop();
}

void AsyncHttpClient::stop() {
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        return; // already stopped
    }

    // Wake the event loop so it exits
    if (wake_pipe_[1] >= 0) {
        (void)::write(wake_pipe_[1], "x", 1);
    }

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }

    // Cancel all pending requests
    {
        std::lock_guard lk(mu_);
        for (auto* req : new_requests_) {
            if (req->promise_ptr) {
                req->promise_ptr->set_value(
                    make_error(ErrorCode::Cancelled, "AsyncHttpClient shutting down"));
            }
            if (req->result_ptr) {
                *req->result_ptr =
                    make_error(ErrorCode::Cancelled, "AsyncHttpClient shutting down");
            }
            if (req->continuation) {
                req->continuation.resume();
            }
            cleanup_request(req);
        }
        new_requests_.clear();
    }

    if (multi_) {
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
    }

    if (wake_pipe_[0] >= 0) { ::close(wake_pipe_[0]); wake_pipe_[0] = -1; }
    if (wake_pipe_[1] >= 0) { ::close(wake_pipe_[1]); wake_pipe_[1] = -1; }
}

CURL* AsyncHttpClient::make_easy(PendingRequest* req, int timeout_sec) {
    CURL* easy = curl_easy_init();
    if (!easy) return nullptr;

    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(easy, CURLOPT_URL, req->url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->header_list);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, async_write_callback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &req->response_body);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    // Store PendingRequest* for retrieval in info_read
    curl_easy_setopt(easy, CURLOPT_PRIVATE, req);

    return easy;
}

Task<Result<HttpResponse>> AsyncHttpClient::post_async(
    std::string url,
    std::string body,
    std::vector<std::string> headers,
    int timeout_sec) {

    // Build the PendingRequest (heap-allocated, owned until completion)
    auto* req = new PendingRequest();
    req->url = std::move(url);

    // Build header list
    struct curl_slist* hdr_list = nullptr;
    for (const auto& h : headers) {
        hdr_list = curl_slist_append(hdr_list, h.c_str());
    }
    req->header_list = hdr_list;

    // Build easy handle
    CURL* easy = make_easy(req, timeout_sec);
    if (!easy) {
        cleanup_request(req);
        co_return make_error(ErrorCode::LLMBackendError,
                             "Failed to init libcurl easy handle");
    }

    // Set request body -- we need to copy body into req so it lives long enough
    // Store body in response_body temporarily?  No, use a separate field.
    // Actually, CURLOPT_COPYPOSTFIELDS copies the data, so body can go out of scope.
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    req->easy = easy;

    // The result will be written here by the event loop
    Result<HttpResponse> result =
        make_error(ErrorCode::Unknown, "request not completed");

    req->result_ptr = &result;

    // Custom awaitable: suspend coroutine, store continuation
    struct AsyncAwaitable {
        PendingRequest* req;
        AsyncHttpClient* client;

        bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept {
            req->continuation = h;
            // Enqueue the request
            {
                std::lock_guard lk(client->mu_);
                client->new_requests_.push_back(req);
            }
            // Wake the event loop
            if (client->wake_pipe_[1] >= 0) {
                (void)::write(client->wake_pipe_[1], "x", 1);
            }
        }

        void await_resume() noexcept {}
    };

    co_await AsyncAwaitable{req, this};

    co_return std::move(result);
}

Result<HttpResponse> AsyncHttpClient::post_blocking(
    std::string url,
    std::string body,
    std::vector<std::string> headers,
    int timeout_sec) {

    auto* req = new PendingRequest();
    req->url = std::move(url);

    struct curl_slist* hdr_list = nullptr;
    for (const auto& h : headers) {
        hdr_list = curl_slist_append(hdr_list, h.c_str());
    }
    req->header_list = hdr_list;

    CURL* easy = make_easy(req, timeout_sec);
    if (!easy) {
        cleanup_request(req);
        return make_error(ErrorCode::LLMBackendError,
                          "Failed to init libcurl easy handle");
    }

    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    req->easy = easy;

    std::promise<Result<HttpResponse>> prom;
    auto fut = prom.get_future();
    req->promise_ptr = &prom;

    // Enqueue
    {
        std::lock_guard lk(mu_);
        new_requests_.push_back(req);
    }
    if (wake_pipe_[1] >= 0) {
        (void)::write(wake_pipe_[1], "x", 1);
    }

    return fut.get();
}

size_t AsyncHttpClient::active_requests() const {
    return active_.load(std::memory_order_relaxed);
}

void AsyncHttpClient::event_loop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        // Drain wake pipe
        {
            char buf[64];
            while (::read(wake_pipe_[0], buf, sizeof(buf)) > 0) {}
        }

        // Add any new requests under lock
        {
            std::lock_guard lk(mu_);
            for (auto* req : new_requests_) {
                curl_multi_add_handle(multi_, req->easy);
                active_.fetch_add(1, std::memory_order_relaxed);
            }
            new_requests_.clear();
        }

        // Poll with 100ms timeout
        int numfds = 0;

        // Use extra fd for wake pipe
        struct curl_waitfd extra_fd;
        extra_fd.fd = wake_pipe_[0];
        extra_fd.events = CURL_WAIT_POLLIN;
        extra_fd.revents = 0;

        curl_multi_poll(multi_, &extra_fd, 1, 100, &numfds);

        // Perform transfers
        int running = 0;
        curl_multi_perform(multi_, &running);

        // Check completed transfers
        CURLMsg* msg = nullptr;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(multi_, &msgs_left)) != nullptr) {
            if (msg->msg == CURLMSG_DONE) {
                process_completed(msg);
            }
        }
    }

    // Drain any remaining active handles on shutdown
    // (they will be cleaned up by stop())
}

void AsyncHttpClient::process_completed(CURLMsg* msg) {
    CURL* easy = msg->easy_handle;
    PendingRequest* req = nullptr;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);

    if (!req) {
        LOG_ERROR("AsyncHttpClient: completed transfer with no PendingRequest");
        curl_multi_remove_handle(multi_, easy);
        curl_easy_cleanup(easy);
        return;
    }

    // Remove from multi (easy handle stays valid until curl_easy_cleanup)
    curl_multi_remove_handle(multi_, easy);
    active_.fetch_sub(1, std::memory_order_relaxed);

    // Build result
    Result<HttpResponse> result =
        make_error(ErrorCode::Unknown, "internal error");

    if (msg->data.result != CURLE_OK) {
        result = make_error(
            ErrorCode::LLMBackendError,
            fmt::format("HTTP request failed: {}",
                        curl_easy_strerror(msg->data.result)));
    } else {
        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
        result = HttpResponse{http_code, std::move(req->response_body)};
    }

    // Save handles before cleanup
    auto continuation = req->continuation;
    auto* promise_ptr = req->promise_ptr;
    auto* result_ptr = req->result_ptr;

    // Deliver result: exactly one of coroutine or promise path is active
    if (result_ptr) {
        *result_ptr = std::move(result);
    } else if (promise_ptr) {
        promise_ptr->set_value(std::move(result));
    }

    // Cleanup the request (frees easy, headers, deletes req)
    cleanup_request(req);

    // Resume coroutine continuation (after cleanup so req is done)
    if (continuation) {
        continuation.resume();
    }
}

void AsyncHttpClient::cleanup_request(PendingRequest* req) {
    if (!req) return;
    if (req->header_list) {
        curl_slist_free_all(req->header_list);
        req->header_list = nullptr;
    }
    if (req->easy) {
        curl_easy_cleanup(req->easy);
        req->easy = nullptr;
    }
    delete req;
}

} // namespace agentos::kernel
