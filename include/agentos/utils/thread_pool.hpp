#pragma once

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>

namespace agentos::utils {

class ThreadPool {
public:
    ThreadPool(size_t threads);

    // The enqueued function must accept a std::stop_token as its first argument
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::pair<std::future<std::invoke_result_t<std::decay_t<F>, std::stop_token, std::decay_t<Args>...>>, std::stop_source>;

    ~ThreadPool();

private:
    std::vector<std::jthread> workers; // Using std::jthread allows automatic cooperative cancellation
    std::queue<std::function<void(std::stop_token)>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t threads)
    : stop(false)
{
    for(size_t i = 0; i<threads; ++i) {
        workers.emplace_back(
            [this](std::stop_token worker_stoken)
            {
                for(;;)
                {
                    std::function<void(std::stop_token)> task;

                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock,
                            [this, &worker_stoken]{ return this->stop || !this->tasks.empty() || worker_stoken.stop_requested(); });

                        // We also need to be able to exit when stop token is requested
                        if(this->stop || worker_stoken.stop_requested()) {
                            if (this->tasks.empty()) {
                                return;
                            }
                        }

                        if(this->tasks.empty()) continue; // Spurious wakeup

                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    // Pass the worker's stop token. The task will be a lambda that captures
                    // its own stop_source token and ignores this one, OR it checks both.
                    // Wait, the task itself is packaged to use its own stop_token, so we just
                    // call it with worker_stoken and it will ignore it internally.
                    task(worker_stoken);
                }
            }
        );
    }
}

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::pair<std::future<std::invoke_result_t<std::decay_t<F>, std::stop_token, std::decay_t<Args>...>>, std::stop_source>
{
    using return_type = std::invoke_result_t<std::decay_t<F>, std::stop_token, std::decay_t<Args>...>;

    std::stop_source stop_source;
    auto task_stoken = stop_source.get_token();

    auto task = std::make_shared< std::packaged_task<return_type()> >(
            [f = std::forward<F>(f), st = task_stoken, ...args = std::forward<Args>(args)]() mutable -> return_type {
                return std::invoke(std::move(f), st, std::move(args)...);
            }
        );

    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if(stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        // The task in the queue accepts the worker_stoken but ignores it since we capture our own task_stoken
        tasks.emplace([task](std::stop_token /*worker_stoken*/){ (*task)(); });
    }
    condition.notify_one();
    return {std::move(res), std::move(stop_source)};
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for(std::jthread &worker: workers) {
        worker.request_stop();
    }
    // jthreads auto-join on destruction
}

} // namespace agentos::utils
