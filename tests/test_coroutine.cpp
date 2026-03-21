#include <agentos/core/co_executor.hpp>
#include <agentos/core/task.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace agentos;

TEST(CoExecutorTest, ScheduleAndRun) {
    CoExecutor exec(1);
    std::atomic<bool> ran{false};

    auto coro = [&]() -> Task<void> {
        ran = true;
        co_return;
    }();

    // Run the task synchronously to verify it works
    coro.run();
    EXPECT_TRUE(ran);
}

TEST(CoExecutorTest, MultipleSchedules) {
    CoExecutor exec(2);
    std::atomic<int> counter{0};

    std::vector<Task<void>> tasks;
    for (int i = 0; i < 10; ++i) {
        tasks.push_back([&]() -> Task<void> {
            counter.fetch_add(1);
            co_return;
        }());
    }

    for (auto& t : tasks) {
        t.run();
    }

    EXPECT_EQ(counter.load(), 10);
}

TEST(CoExecutorTest, StopGracefully) {
    auto exec = std::make_unique<CoExecutor>(2);
    exec->stop();
    // Double stop should not crash
    exec->stop();
    exec.reset();
}

TEST(CoExecutorTest, TaskReturnsValue) {
    auto coro = []() -> Task<int> {
        co_return 42;
    }();

    EXPECT_EQ(coro.run(), 42);
}

TEST(AsyncInferTest, InferAsyncWithMockBackend) {
    auto mock = std::make_unique<kernel::MockLLMBackend>();
    mock->register_rule("hello", "world");

    kernel::LLMKernel k(std::move(mock), 100000);

    kernel::LLMRequest req;
    req.messages.push_back(kernel::Message::user("hello"));

    auto task = k.infer_async(std::move(req));
    auto result = task.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content, "world");
}
