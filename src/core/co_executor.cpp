#include <agentos/core/co_executor.hpp>

namespace agentos {

CoExecutor::CoExecutor(size_t threads) {
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

CoExecutor::~CoExecutor() { stop(); }

void CoExecutor::schedule(std::coroutine_handle<> h) {
    {
        std::lock_guard lk(mu_);
        queue_.push(h);
    }
    cv_.notify_one();
}

void CoExecutor::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

void CoExecutor::worker_loop() {
    while (true) {
        std::coroutine_handle<> h;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [this] { return !queue_.empty() || stop_; });
            if (stop_ && queue_.empty()) return;
            h = queue_.front();
            queue_.pop();
        }
        if (h && !h.done()) h.resume();
    }
}

} // namespace agentos
