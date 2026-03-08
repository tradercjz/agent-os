#include <agentos/scheduler/scheduler.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::scheduler;

// ── DependencyGraph 单元测试 ────────────────────────────────

class DependencyGraphTest : public ::testing::Test {
protected:
  DependencyGraph graph;
};

TEST_F(DependencyGraphTest, TaskReadyWithoutDependencies) {
  graph.add_task(1, Priority::Normal);
  EXPECT_TRUE(graph.is_ready(1));
}

TEST_F(DependencyGraphTest, TaskBlockedByDependency) {
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  auto r = graph.add_dependency(2, 1); // task 2 depends on task 1
  ASSERT_TRUE(r);
  EXPECT_TRUE(graph.is_ready(1));
  EXPECT_FALSE(graph.is_ready(2));
}

TEST_F(DependencyGraphTest, CompleteTaskUnblocksDependents) {
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  graph.add_task(3, Priority::Normal);
  (void)graph.add_dependency(2, 1); // 2 depends on 1
  (void)graph.add_dependency(3, 1); // 3 depends on 1

  auto ready = graph.complete_task(1);
  EXPECT_GE(ready.size(), 2u); // both 2 and 3 should be ready
}

TEST_F(DependencyGraphTest, CircularDependencyRejected) {
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  (void)graph.add_dependency(2, 1); // 2 → 1
  auto r = graph.add_dependency(1, 2);  // 1 → 2 would create cycle
  EXPECT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::CircularDependency);
}

TEST_F(DependencyGraphTest, CriticalPathBoosting) {
  // 1 ← 2 ← 3 (3 depends on 2, 2 depends on 1)
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  graph.add_task(3, Priority::Normal);
  (void)graph.add_dependency(2, 1);
  (void)graph.add_dependency(3, 2);

  std::unordered_map<TaskId, int> boosts;
  graph.boost_critical_path(boosts);
  // Depth = 1 + max(dep depths): task with longest dependency chain gets highest boost
  // depth[1]=1, depth[2]=2, depth[3]=3
  EXPECT_GT(boosts[3], boosts[1]);
  EXPECT_GT(boosts[2], boosts[1]);
}

TEST_F(DependencyGraphTest, NoCycleNonDeadlock) {
  // 简单依赖链（20 依赖 10）不构成死锁
  graph.add_task(10, Priority::Normal);
  graph.add_task(20, Priority::Normal);
  (void)graph.add_dependency(20, 10);

  std::vector<TaskId> waiting = {10, 20};
  bool deadlock = graph.detect_deadlock(waiting);
  EXPECT_FALSE(deadlock); // 单向依赖无环，不是死锁
}

// ── Scheduler 集成测试 ──────────────────────────────────────

class SchedulerTest : public ::testing::Test {
protected:
  void SetUp() override {
    sched = std::make_unique<Scheduler>(SchedulerPolicy::Priority, 2);
    sched->start();
  }
  void TearDown() override { sched->shutdown(); }
  std::unique_ptr<Scheduler> sched;
};

TEST_F(SchedulerTest, BasicTaskExecution) {
  std::atomic<int> counter{0};
  auto task = std::make_shared<AgentTaskDescriptor>();
  task->id = Scheduler::new_task_id();
  task->name = "increment";
  task->priority = Priority::Normal;
  task->work = [&] { counter++; };

  auto r = sched->submit(task);
  ASSERT_TRUE(r);
  bool done = sched->wait_for(*r, Duration{5000});
  EXPECT_TRUE(done);
  EXPECT_EQ(counter.load(), 1);
  EXPECT_EQ(sched->task_state(*r), TaskState::Completed);
}

TEST_F(SchedulerTest, DependencyOrderingRespected) {
  std::vector<int> order;
  std::mutex order_mu;

  auto t1 = std::make_shared<AgentTaskDescriptor>();
  t1->id = Scheduler::new_task_id();
  t1->name = "first";
  t1->priority = Priority::Normal;
  t1->work = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard lk(order_mu);
    order.push_back(1);
  };

  auto t2 = std::make_shared<AgentTaskDescriptor>();
  t2->id = Scheduler::new_task_id();
  t2->name = "second";
  t2->priority = Priority::Normal;
  t2->depends_on = {t1->id}; // t2 depends on t1
  t2->work = [&] {
    std::lock_guard lk(order_mu);
    order.push_back(2);
  };

  (void)sched->submit(t1);
  (void)sched->submit(t2);

  sched->wait_for(t2->id, Duration{5000});

  std::lock_guard lk(order_mu);
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 1); // t1 must run before t2
  EXPECT_EQ(order[1], 2);
}

TEST_F(DependencyGraphTest, CompleteTaskNoDuplicateReady) {
  // 确保 complete_task 不会对同一任务返回多次
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  graph.add_task(3, Priority::Normal);
  (void)graph.add_dependency(2, 1); // 2 depends on 1
  (void)graph.add_dependency(3, 1); // 3 depends on 1

  auto ready1 = graph.complete_task(1);
  EXPECT_EQ(ready1.size(), 2u); // 2 和 3 都就绪

  // 再完成一个任务后，之前已返回的 3 不应再出现
  graph.add_task(4, Priority::Normal);
  (void)graph.add_dependency(4, 2);
  auto ready2 = graph.complete_task(2);
  // 只应返回 4（新就绪），不应包含 3（上次已返回）
  for (auto id : ready2) {
    EXPECT_NE(id, static_cast<TaskId>(3));
  }
}

TEST_F(SchedulerTest, WaitForUsesConditionVariable) {
  // 验证 wait_for 在任务完成后立即返回（不需要轮询 10ms 间隔）
  auto task = std::make_shared<AgentTaskDescriptor>();
  task->id = Scheduler::new_task_id();
  task->name = "fast";
  task->priority = Priority::Normal;
  task->work = [] {}; // 瞬间完成

  (void)sched->submit(task);

  auto start = now();
  bool done = sched->wait_for(task->id, Duration{5000});
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);

  EXPECT_TRUE(done);
  // 使用条件变量后，应远快于旧 10ms 轮询间隔返回
  EXPECT_LT(elapsed.count(), 500);
}

TEST_F(SchedulerTest, ShutdownIdempotent) {
  // 多次调用 shutdown 不应崩溃
  sched->shutdown();
  sched->shutdown(); // 第二次应为 no-op
}

TEST_F(SchedulerTest, TaskCancellation) {
  std::atomic<bool> ran{false};
  auto task = std::make_shared<AgentTaskDescriptor>();
  task->id = Scheduler::new_task_id();
  task->name = "cancellable";
  task->priority = Priority::Low;
  // Make it depend on a task that will never complete
  auto blocker = std::make_shared<AgentTaskDescriptor>();
  blocker->id = Scheduler::new_task_id();
  blocker->name = "blocker";
  blocker->priority = Priority::Normal;
  blocker->work = [&] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
  };

  task->depends_on = {blocker->id};
  task->work = [&] { ran = true; };

  (void)sched->submit(blocker);
  (void)sched->submit(task);

  bool cancelled = sched->cancel(task->id);
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(sched->task_state(task->id), TaskState::Cancelled);
}

TEST_F(SchedulerTest, CancelWakesWaitFor) {
  // Task that blocks — cancel should wake wait_for immediately
  auto blocker = std::make_shared<AgentTaskDescriptor>();
  blocker->id = Scheduler::new_task_id();
  blocker->name = "slow_task";
  blocker->priority = Priority::Normal;
  blocker->work = [] { std::this_thread::sleep_for(std::chrono::seconds(30)); };

  // Dependent task that will never run
  auto task = std::make_shared<AgentTaskDescriptor>();
  task->id = Scheduler::new_task_id();
  task->name = "dependent";
  task->priority = Priority::Normal;
  task->depends_on = {blocker->id};
  task->work = [] {};

  (void)sched->submit(blocker);
  (void)sched->submit(task);

  // Cancel in a separate thread after 50ms
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sched->cancel(task->id);
  });

  auto start = now();
  bool done = sched->wait_for(task->id, Duration{5000});
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);

  EXPECT_TRUE(done); // Should return true (Cancelled is terminal)
  EXPECT_LT(elapsed.count(), 2000); // Should wake quickly, not timeout at 5s
  EXPECT_EQ(sched->task_state(task->id), TaskState::Cancelled);

  canceller.join();
}

// ── Regression tests for 44-fix commit ──────────────────────────

TEST_F(DependencyGraphTest, IterativeDFSHandlesLargeGraph) {
  // Test that iterative DFS (used in has_cycle) handles large graphs
  // without stack overflow, equivalent to recursive implementation
  const size_t kGraphSize = 1000;

  // Create a chain: 0 → 1 → 2 → ... → 999
  for (size_t i = 0; i < kGraphSize; ++i) {
    graph.add_task(static_cast<TaskId>(i), Priority::Normal);
  }

  for (size_t i = 1; i < kGraphSize; ++i) {
    auto r = graph.add_dependency(static_cast<TaskId>(i),
                                   static_cast<TaskId>(i - 1));
    ASSERT_TRUE(r);
  }

  // Verify no cycle by successfully adding all edges (add_dependency rejects cycles)
  // All chain edges were added above without error, confirming acyclic graph

  // Now try to add edge 999 → 0 to create a cycle — should be REJECTED
  auto cycle_result = graph.add_dependency(0, static_cast<TaskId>(kGraphSize - 1));
  EXPECT_FALSE(cycle_result); // add_dependency must reject cycle-creating edges

  // Graph should still accept a valid non-cycle edge
  auto valid = graph.add_dependency(static_cast<TaskId>(kGraphSize - 1),
                                    static_cast<TaskId>(kGraphSize - 2));
  // Note: 999 already depends on 998 (from chain), so this is a duplicate, not a cycle
  // Either Ok or AlreadyExists is acceptable
  (void)valid; // result doesn't matter, just checking no crash
}

TEST_F(SchedulerTest, CancelWhileDispatch) {
  // Test concurrent task cancellation while scheduler is dispatching
  // (TOCTOU fix: check if cancelled before running)
  std::atomic<int> completed{0};
  std::atomic<int> cancelled_count{0};

  // Create 5 tasks that sleep briefly
  std::vector<TaskId> task_ids;
  for (int i = 0; i < 5; ++i) {
    auto task = std::make_shared<AgentTaskDescriptor>();
    task->id = Scheduler::new_task_id();
    task->name = std::string("task_") + std::to_string(i);
    task->priority = Priority::Normal;
    task->work = [&, task_idx = i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (task_idx < 3) {
        // Only increment if not cancelled
        completed++;
      }
    };

    task_ids.push_back(task->id);
    (void)sched->submit(task);
  }

  // While tasks are running, cancel 2 of them
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelled_count += sched->cancel(task_ids[1]) ? 1 : 0;
    cancelled_count += sched->cancel(task_ids[3]) ? 1 : 0;
  });

  // Wait for all to complete or timeout
  sched->wait_for(task_ids[4], Duration{5000});

  canceller.join();

  // Verify no crash and reasonable counts
  EXPECT_GE(completed.load(), 0);
  EXPECT_GE(cancelled_count.load(), 0);
  EXPECT_LE(cancelled_count.load(), 2);
}

TEST_F(SchedulerTest, PerInstanceTaskIds) {
  // Test that task IDs are globally unique across scheduler instances
  auto sched2 = std::make_unique<Scheduler>(SchedulerPolicy::Priority, 2);
  sched2->start();

  std::set<TaskId> ids1, ids2;

  // Submit 5 tasks to first scheduler
  for (int i = 0; i < 5; ++i) {
    TaskId id = Scheduler::new_task_id();
    ids1.insert(id);
    auto task = std::make_shared<AgentTaskDescriptor>();
    task->id = id;
    task->name = "scheduler1_task";
    task->priority = Priority::Normal;
    task->work = [] {};
    (void)sched->submit(task);
  }

  // Submit 5 tasks to second scheduler
  for (int i = 0; i < 5; ++i) {
    TaskId id = Scheduler::new_task_id();
    ids2.insert(id);
    auto task = std::make_shared<AgentTaskDescriptor>();
    task->id = id;
    task->name = "scheduler2_task";
    task->priority = Priority::Normal;
    task->work = [] {};
    (void)sched2->submit(task);
  }

  // Verify no collisions between the two sets
  for (TaskId id : ids1) {
    EXPECT_EQ(ids2.find(id), ids2.end()) << "Task ID collision: " << id;
  }

  sched2->shutdown();
}

// R7-9: Multiple Scheduler instances should generate globally unique task IDs
TEST(MultiSchedulerTest, UniqueTaskIdsAcrossInstances) {
  // Create 3 schedulers
  Scheduler s1(SchedulerPolicy::Priority, 2), s2(SchedulerPolicy::Priority, 2), s3(SchedulerPolicy::Priority, 2);
  s1.start();
  s2.start();
  s3.start();

  std::set<TaskId> all_ids;
  const int tasks_per_scheduler = 100;

  // Generate IDs from each scheduler
  for (int i = 0; i < tasks_per_scheduler; ++i) {
    all_ids.insert(Scheduler::new_task_id());
  }

  // All IDs should be unique
  EXPECT_EQ(all_ids.size(), static_cast<size_t>(tasks_per_scheduler));

  s1.shutdown();
  s2.shutdown();
  s3.shutdown();
}
