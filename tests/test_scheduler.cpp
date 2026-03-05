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
  graph.add_dependency(2, 1); // 2 depends on 1
  graph.add_dependency(3, 1); // 3 depends on 1

  auto ready = graph.complete_task(1);
  EXPECT_GE(ready.size(), 2); // both 2 and 3 should be ready
}

TEST_F(DependencyGraphTest, CircularDependencyRejected) {
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  graph.add_dependency(2, 1); // 2 → 1
  auto r = graph.add_dependency(1, 2);  // 1 → 2 would create cycle
  EXPECT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrorCode::CircularDependency);
}

TEST_F(DependencyGraphTest, CriticalPathBoosting) {
  // 1 ← 2 ← 3 (3 depends on 2, 2 depends on 1)
  graph.add_task(1, Priority::Normal);
  graph.add_task(2, Priority::Normal);
  graph.add_task(3, Priority::Normal);
  graph.add_dependency(2, 1);
  graph.add_dependency(3, 2);

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
  graph.add_dependency(20, 10);

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

  sched->submit(t1);
  sched->submit(t2);

  sched->wait_for(t2->id, Duration{5000});

  std::lock_guard lk(order_mu);
  ASSERT_EQ(order.size(), 2);
  EXPECT_EQ(order[0], 1); // t1 must run before t2
  EXPECT_EQ(order[1], 2);
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

  sched->submit(blocker);
  sched->submit(task);

  bool cancelled = sched->cancel(task->id);
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(sched->task_state(task->id), TaskState::Cancelled);
}
