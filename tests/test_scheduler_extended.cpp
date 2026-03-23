#include <agentos/scheduler/scheduler.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::scheduler;

// ── Fixture ──────────────────────────────────────────────────

class SchedulerExtendedTest : public ::testing::Test {
protected:
  void SetUp() override {
    sched = std::make_unique<Scheduler>(SchedulerPolicy::Priority, 2);
    sched->start();
  }
  void TearDown() override { sched->shutdown(); }

  TaskPtr make_task(Priority prio, std::function<void()> work,
                    std::vector<TaskId> deps = {}) {
    auto t = std::make_shared<AgentTaskDescriptor>();
    t->id = Scheduler::new_task_id();
    t->name = "task_" + std::to_string(t->id);
    t->priority = prio;
    t->work = std::move(work);
    t->depends_on = std::move(deps);
    return t;
  }

  std::unique_ptr<Scheduler> sched;
};

// ── 1. Submit task and wait for completion ───────────────────

TEST_F(SchedulerExtendedTest, SubmitAndWaitForCompletion) {
  std::atomic<int> value{0};
  auto task = make_task(Priority::Normal, [&] { value.store(42); });

  auto r = sched->submit(task);
  ASSERT_TRUE(r.has_value());
  bool done = sched->wait_for(*r, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(value.load(), 42);
  EXPECT_EQ(sched->task_state(*r), TaskState::Completed);
}

// ── 2. Submit multiple tasks with priorities ────────────────

TEST_F(SchedulerExtendedTest, SubmitMultipleTasksWithPriorities) {
  std::atomic<int> completed_count{0};

  std::vector<TaskPtr> tasks;
  for (auto prio : {Priority::Low, Priority::Normal, Priority::High, Priority::Critical}) {
    tasks.push_back(make_task(prio, [&] { completed_count++; }));
  }

  for (auto& t : tasks) {
    auto r = sched->submit(t);
    ASSERT_TRUE(r.has_value());
  }

  bool drained = sched->drain(Duration{5000});
  EXPECT_TRUE(drained);
  EXPECT_EQ(completed_count.load(), 4);
}

// ── 3. High priority task runs before normal ────────────────

TEST_F(SchedulerExtendedTest, HighPriorityRunsBeforeNormal) {
  // Use a single-threaded scheduler for deterministic ordering
  sched->shutdown();
  sched = std::make_unique<Scheduler>(SchedulerPolicy::Priority, 1);
  sched->start();

  std::vector<int> execution_order;
  std::mutex order_mu;

  // Submit a blocker so all tasks queue up before any runs
  std::atomic<bool> gate{false};
  auto blocker = make_task(Priority::Critical, [&] {
    while (!gate.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  (void)sched->submit(blocker);
  // Wait for blocker to start running
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Now submit low and high priority tasks; they will queue behind blocker
  auto low_task = make_task(Priority::Low, [&] {
    std::lock_guard lk(order_mu);
    execution_order.push_back(0); // low
  });
  auto high_task = make_task(Priority::High, [&] {
    std::lock_guard lk(order_mu);
    execution_order.push_back(10); // high
  });

  (void)sched->submit(low_task);
  (void)sched->submit(high_task);

  // Release the blocker
  gate.store(true);

  sched->wait_for(low_task->id, Duration{5000});
  sched->wait_for(high_task->id, Duration{5000});

  std::lock_guard lk(order_mu);
  ASSERT_EQ(execution_order.size(), 2u);
  // High priority (10) should run before low (0)
  EXPECT_EQ(execution_order[0], 10);
  EXPECT_EQ(execution_order[1], 0);
}

// ── 4. Task cancellation ────────────────────────────────────

TEST_F(SchedulerExtendedTest, TaskCancellation) {
  // Create a task that depends on a never-finishing blocker
  auto blocker = make_task(Priority::Normal, [] {
    std::this_thread::sleep_for(std::chrono::seconds(30));
  });
  auto dependent = make_task(Priority::Normal, [] {}, {blocker->id});

  (void)sched->submit(blocker);
  (void)sched->submit(dependent);

  bool cancelled = sched->cancel(dependent->id);
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(sched->task_state(dependent->id), TaskState::Cancelled);

  // wait_for should return true immediately since Cancelled is terminal
  bool done = sched->wait_for(dependent->id, Duration{1000});
  EXPECT_TRUE(done);
}

// ── 5. Task state: Pending -> Running -> Completed ──────────

TEST_F(SchedulerExtendedTest, StateTransitionPendingRunningCompleted) {
  std::atomic<bool> gate{false};
  std::atomic<bool> running_observed{false};

  auto task = make_task(Priority::Normal, [&] {
    // Signal that we are running
    running_observed.store(true);
    while (!gate.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  // Before submit: state is Pending (default)
  EXPECT_EQ(task->state.load(), TaskState::Pending);

  (void)sched->submit(task);

  // Wait until the task is actually running
  for (int i = 0; i < 200 && !running_observed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(running_observed.load());
  EXPECT_EQ(sched->task_state(task->id), TaskState::Running);

  // Release the task
  gate.store(true);
  bool done = sched->wait_for(task->id, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(sched->task_state(task->id), TaskState::Completed);
}

// ── 6. Task state: Pending -> Running -> Failed ─────────────

TEST_F(SchedulerExtendedTest, StateTransitionPendingRunningFailed) {
  auto task = make_task(Priority::Normal, [] {
    throw std::runtime_error("intentional failure");
  });

  (void)sched->submit(task);
  bool done = sched->wait_for(task->id, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(sched->task_state(task->id), TaskState::Failed);
  EXPECT_GE(sched->metrics().tasks_failed.load(), 1u);
}

// ── 7. DAG task dependencies: A -> B -> C execution order ───

TEST_F(SchedulerExtendedTest, DAGDependencyExecutionOrder) {
  std::vector<int> order;
  std::mutex order_mu;

  auto task_a = make_task(Priority::Normal, [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::lock_guard lk(order_mu);
    order.push_back(1);
  });

  auto task_b = make_task(Priority::Normal, [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::lock_guard lk(order_mu);
    order.push_back(2);
  }, {task_a->id});

  auto task_c = make_task(Priority::Normal, [&] {
    std::lock_guard lk(order_mu);
    order.push_back(3);
  }, {task_b->id});

  (void)sched->submit(task_a);
  (void)sched->submit(task_b);
  (void)sched->submit(task_c);

  bool done = sched->wait_for(task_c->id, Duration{10000});
  EXPECT_TRUE(done);

  std::lock_guard lk(order_mu);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1); // A first
  EXPECT_EQ(order[1], 2); // B second
  EXPECT_EQ(order[2], 3); // C third
}

// ── 8. Concurrent task submission ───────────────────────────

TEST_F(SchedulerExtendedTest, ConcurrentTaskSubmission) {
  constexpr int kNumThreads = 4;
  constexpr int kTasksPerThread = 25;
  std::atomic<int> total_completed{0};

  std::vector<std::thread> submitters;
  for (int t = 0; t < kNumThreads; ++t) {
    submitters.emplace_back([&] {
      for (int i = 0; i < kTasksPerThread; ++i) {
        auto task = make_task(Priority::Normal, [&] {
          total_completed++;
        });
        auto r = sched->submit(task);
        EXPECT_TRUE(r.has_value());
      }
    });
  }

  for (auto& th : submitters) th.join();

  bool drained = sched->drain(Duration{10000});
  EXPECT_TRUE(drained);
  EXPECT_EQ(total_completed.load(), kNumThreads * kTasksPerThread);
}

// ── 9. Scheduler drain: wait for all tasks ──────────────────

TEST_F(SchedulerExtendedTest, DrainWaitsForAllTasks) {
  std::atomic<int> counter{0};
  constexpr int kNumTasks = 10;

  for (int i = 0; i < kNumTasks; ++i) {
    auto task = make_task(Priority::Normal, [&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      counter++;
    });
    (void)sched->submit(task);
  }

  bool drained = sched->drain(Duration{10000});
  EXPECT_TRUE(drained);
  EXPECT_EQ(counter.load(), kNumTasks);
  EXPECT_EQ(sched->active_task_count(), 0u);
}

// ── 10. Active task count tracking ──────────────────────────

TEST_F(SchedulerExtendedTest, ActiveTaskCountTracking) {
  EXPECT_EQ(sched->active_task_count(), 0u);

  std::atomic<bool> gate{false};
  std::vector<TaskPtr> tasks;

  for (int i = 0; i < 3; ++i) {
    auto task = make_task(Priority::Normal, [&] {
      while (!gate.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    tasks.push_back(task);
    (void)sched->submit(task);
  }

  // Wait for tasks to start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // At least some tasks should be active (running or pending)
  EXPECT_GE(sched->active_task_count(), 1u);

  // Release all tasks
  gate.store(true);
  bool drained = sched->drain(Duration{5000});
  EXPECT_TRUE(drained);
  EXPECT_EQ(sched->active_task_count(), 0u);
}

// ── 11. Task with timeout (task exceeding max_runtime) ──────

TEST_F(SchedulerExtendedTest, TaskWithTimeout) {
  // Note: The scheduler does not enforce max_runtime automatically.
  // The work function must cooperate or be wrapped. This test verifies
  // that the timeout field is settable and a task that finishes quickly
  // still works when max_runtime is set.
  auto task = make_task(Priority::Normal, [] {
    // Finishes quickly
  });
  task->max_runtime = Duration{100}; // 100ms timeout

  auto r = sched->submit(task);
  ASSERT_TRUE(r.has_value());
  bool done = sched->wait_for(*r, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(sched->task_state(*r), TaskState::Completed);
}

// ── 12. Task result retrieval (state-based) ─────────────────

TEST_F(SchedulerExtendedTest, TaskResultRetrieval) {
  // Since AgentTaskDescriptor does not store a result value,
  // we verify state transitions and use external storage for results.
  std::string result_value;
  std::mutex result_mu;

  auto task = make_task(Priority::Normal, [&] {
    std::lock_guard lk(result_mu);
    result_value = "computed_result_42";
  });

  auto r = sched->submit(task);
  ASSERT_TRUE(r.has_value());
  bool done = sched->wait_for(*r, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(sched->task_state(*r), TaskState::Completed);

  std::lock_guard lk(result_mu);
  EXPECT_EQ(result_value, "computed_result_42");

  // Verify metrics
  EXPECT_GE(sched->metrics().tasks_submitted.load(), 1u);
  EXPECT_GE(sched->metrics().tasks_completed.load(), 1u);
}
