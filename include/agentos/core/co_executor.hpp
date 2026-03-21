#pragma once
// ============================================================
// AgentOS :: CoExecutor — Coroutine scheduler with thread pool
// ============================================================
#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace agentos {

class CoExecutor {
public:
    explicit CoExecutor(size_t threads = 2);
    ~CoExecutor();

    CoExecutor(const CoExecutor&) = delete;
    CoExecutor& operator=(const CoExecutor&) = delete;

    void schedule(std::coroutine_handle<> h);
    void stop();

private:
    void worker_loop();

    std::queue<std::coroutine_handle<>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::jthread> workers_;
    bool stop_{false};
};

} // namespace agentos
