#pragma once
// ============================================================
// AgentOS :: Coroutine Task<T>
// C++23 协程任务类型，支持 co_await 链式组合
// ============================================================
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

namespace agentos {

// ── 前向声明 ──────────────────────────────────────────────────
template<typename T = void>
class Task;

namespace detail {

// ── Promise 基类（共享逻辑） ───────────────────────────────────
template<typename T>
struct TaskPromiseBase {
    std::coroutine_handle<> continuation{std::noop_coroutine()};
    std::exception_ptr exception;

    // 初始挂起：lazy（创建后不自动执行）
    auto initial_suspend() noexcept { return std::suspend_always{}; }

    // 最终挂起：恢复 continuation（调用方协程）
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<TaskPromiseBase> h) noexcept {
            return h.promise().continuation;
        }
        void await_resume() noexcept {}
    };
    auto final_suspend() noexcept { return FinalAwaiter{}; }

    void unhandled_exception() {
        exception = std::current_exception();
    }
};

// ── Promise<T>（有返回值）─────────────────────────────────────
template<typename T>
struct TaskPromise : TaskPromiseBase<T> {
    std::variant<std::monostate, T> value;

    Task<T> get_return_object();

    void return_value(T v) {
        value = std::move(v);
    }

    T& result() {
        if (this->exception) std::rethrow_exception(this->exception);
        return std::get<T>(value);
    }
};

// ── Promise<void>（无返回值）──────────────────────────────────
template<>
struct TaskPromise<void> : TaskPromiseBase<void> {
    Task<void> get_return_object();

    void return_void() noexcept {}

    void result() {
        if (exception) std::rethrow_exception(exception);
    }
};

} // namespace detail

// ── Task<T> 主体 ──────────────────────────────────────────────
template<typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle h) noexcept : handle_(h) {}
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }
    ~Task() { if (handle_) handle_.destroy(); }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // ── co_await 支持 ────────────────────────────────────────
    struct Awaiter {
        Handle handle;

        bool await_ready() noexcept { return handle.done(); }

        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> caller) noexcept {
            handle.promise().continuation = caller;
            return handle; // 对称转移到被等待的任务
        }

        decltype(auto) await_resume() {
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() && noexcept {
        return Awaiter{handle_};
    }

    // ── 同步运行（在非协程环境中使用）────────────────────────
    /// Synchronously drives this coroutine to completion.
    /// PRECONDITION: This coroutine must NOT co_await any externally-completed
    /// awaitables (e.g., network I/O, timers). Use only for pure-compute coroutines
    /// that co_yield intermediate values or co_return final results immediately.
    /// For async coroutines, integrate with an event loop instead.
    [[nodiscard]] T run() {
        static constexpr size_t kMaxIterations = 1'000'000;
        size_t iter = 0;
        handle_.resume();
        while (!handle_.done()) {
            if (++iter > kMaxIterations) {
                throw std::runtime_error("Task::run() exceeded max iterations - "
                    "coroutine may be awaiting external event; use async runner instead");
            }
            handle_.resume();
        }
        return handle_.promise().result();
    }

    bool done() const noexcept { return !handle_ || handle_.done(); }
    Handle raw_handle() const noexcept { return handle_; }

private:
    Handle handle_;
};

// ── get_return_object 实现 ────────────────────────────────────
namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

} // namespace detail

// ── 调度器驱动用的句柄包装 ────────────────────────────────────
struct SchedulableTask {
    std::coroutine_handle<> handle;
    int priority{0};
    bool operator<(const SchedulableTask& o) const {
        return priority < o.priority; // 高优先级在堆顶
    }
};

} // namespace agentos
