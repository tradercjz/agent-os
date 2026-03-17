#include <gtest/gtest.h>
#include "agentos/core/task.hpp"

using namespace agentos;

// Simple task returning a value
Task<int> compute_value() {
    co_return 42;
}

// Simple task returning void
Task<void> compute_void(int& flag) {
    flag = 1;
    co_return;
}

// Task throwing an exception
Task<int> compute_throw() {
    throw std::runtime_error("Test exception");
    co_return 0; // unreachable
}

// Task awaiting another task
Task<int> compute_chained() {
    int v = co_await compute_value();
    co_return v * 2;
}

// Task yielding/returning multiple times? No, Task<T> only has co_return.
// We can test deeply nested await.
Task<int> compute_deeply_chained() {
    int v1 = co_await compute_chained();
    int v2 = co_await compute_chained();
    co_return v1 + v2;
}

TEST(TaskTest, ReturnValue) {
    auto task = compute_value();
    int result = task.run();
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(task.done());
}

TEST(TaskTest, ReturnVoid) {
    int flag = 0;
    auto task = compute_void(flag);
    task.run();
    EXPECT_EQ(flag, 1);
    EXPECT_TRUE(task.done());
}

TEST(TaskTest, ExceptionHandling) {
    auto task = compute_throw();
    EXPECT_THROW({ (void)task.run(); }, std::runtime_error);
    EXPECT_TRUE(task.done());
}

TEST(TaskTest, CoroutineChaining) {
    auto task = compute_chained();
    int result = task.run();
    EXPECT_EQ(result, 84);
    EXPECT_TRUE(task.done());
}

TEST(TaskTest, DeeplyChained) {
    auto task = compute_deeply_chained();
    int result = task.run();
    EXPECT_EQ(result, 168);
    EXPECT_TRUE(task.done());
}

TEST(TaskTest, MoveSemantics) {
    auto task1 = compute_value();
    auto task2 = std::move(task1);

    // task1 should be empty, task2 should have the handle
    EXPECT_TRUE(task1.done()); // an empty task is considered done

    int result = task2.run();
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(task2.done());
}

TEST(TaskTest, TaskDestroyedWithoutRunning) {
    // This just tests that memory doesn't leak or crash
    auto task = compute_value();
    // task goes out of scope and is destroyed
}
