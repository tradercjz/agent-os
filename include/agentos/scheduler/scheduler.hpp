#pragma once
// ============================================================
// AgentOS :: Module 2 — Scheduler
// 优先级调度器：FIFO / Round-Robin / 抢占，依赖 DAG，死锁检测
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/core/task.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos::scheduler {

// ─────────────────────────────────────────────────────────────
// § 2.1  AgentTask（可调度的协程工作单元）
// ─────────────────────────────────────────────────────────────

enum class TaskState { Pending, Running, Suspended, Completed, Failed, Cancelled };

struct AgentTaskDescriptor {
    TaskId                         id;
    AgentId                        agent_id;
    Priority                       priority;
    std::string                    name;
    std::function<void()>          work;      // 实际执行体（协程 resume 或普通函数）
    std::vector<TaskId>            depends_on; // 依赖的任务 ID
    std::optional<TimePoint>       deadline;
    std::atomic<TaskState>         state{TaskState::Pending};
    TimePoint                      enqueue_time{now()};
    Duration                       max_runtime{Duration{30000}}; // 30s
};

using TaskPtr = std::shared_ptr<AgentTaskDescriptor>;

// ─────────────────────────────────────────────────────────────
// § 2.2  依赖关系图（DAG）+ 关键路径分析
// ─────────────────────────────────────────────────────────────

class DependencyGraph {
public:
    // 注册任务节点
    void add_task(TaskId id, Priority priority) {
        std::lock_guard lk(mu_);
        nodes_[id] = {priority, {}};
    }

    // 添加依赖：task_id 依赖 dep_id（dep_id 必须先完成）
    Result<void> add_dependency(TaskId task_id, TaskId dep_id) {
        std::lock_guard lk(mu_);
        nodes_[task_id].deps.insert(dep_id);
        // 检测循环依赖
        if (has_cycle_locked()) {
            nodes_[task_id].deps.erase(dep_id);
            return make_error(ErrorCode::CircularDependency,
                fmt::format("Adding edge {}->{} would create a cycle",
                            task_id, dep_id));
        }
        return {};
    }

    // 标记任务完成，返回新就绪的任务列表
    std::vector<TaskId> complete_task(TaskId id) {
        std::lock_guard lk(mu_);
        completed_.insert(id);
        std::vector<TaskId> newly_ready;
        for (auto& [tid, node] : nodes_) {
            if (completed_.contains(tid)) continue;
            if (enqueued_.contains(tid)) continue; // 已入队的不再返回，避免重复
            if (all_deps_satisfied_locked(tid)) {
                enqueued_.insert(tid);
                newly_ready.push_back(tid);
            }
        }
        return newly_ready;
    }

    // 标记任务已入队（避免 complete_task 重复返回）
    void mark_enqueued(TaskId id) {
        std::lock_guard lk(mu_);
        enqueued_.insert(id);
    }

    // 检查任务是否就绪（所有依赖已完成）
    bool is_ready(TaskId id) const {
        std::lock_guard lk(mu_);
        return all_deps_satisfied_locked(id);
    }

    // 计算关键路径，提升关键路径上任务的有效优先级
    void boost_critical_path(std::unordered_map<TaskId, int>& priority_boost) {
        std::lock_guard lk(mu_);
        // 拓扑排序后，从终点倒推关键路径长度
        auto topo = topological_sort_locked();
        std::unordered_map<TaskId, int> depth;
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            TaskId tid = *it;
            int max_dep_depth = 0;
            for (TaskId dep : nodes_[tid].deps) {
                max_dep_depth = std::max(max_dep_depth, depth[dep]);
            }
            depth[tid] = max_dep_depth + 1;
        }
        // 深度越大 → 越靠近起点 → 关键性越高
        int max_depth = 0;
        for (auto& [tid, d] : depth) max_depth = std::max(max_depth, d);
        for (auto& [tid, d] : depth) {
            priority_boost[tid] = d; // 直接用深度作为加权
        }
    }

    // 死锁检测：检查是否存在循环等待
    bool detect_deadlock(const std::vector<TaskId>& waiting_tasks) {
        std::lock_guard lk(mu_);
        // 构建等待图并检测环路
        std::unordered_set<TaskId> waiting_set(waiting_tasks.begin(),
                                               waiting_tasks.end());
        for (TaskId tid : waiting_tasks) {
            if (cycle_from_locked(tid, waiting_set)) return true;
        }
        return false;
    }

private:
    struct Node {
        Priority              priority;
        std::unordered_set<TaskId> deps;
    };

    bool all_deps_satisfied_locked(TaskId id) const {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) return true;
        for (TaskId dep : it->second.deps) {
            if (!completed_.contains(dep)) return false;
        }
        return true;
    }

    bool has_cycle_locked() const {
        std::unordered_set<TaskId> visited, in_stack;
        for (auto& [id, _] : nodes_) {
            if (!visited.contains(id) && dfs_cycle(id, visited, in_stack))
                return true;
        }
        return false;
    }

    bool dfs_cycle(TaskId id,
                   std::unordered_set<TaskId>& visited,
                   std::unordered_set<TaskId>& in_stack) const {
        visited.insert(id);
        in_stack.insert(id);
        auto it = nodes_.find(id);
        if (it != nodes_.end()) {
            for (TaskId dep : it->second.deps) {
                if (!visited.contains(dep)) {
                    if (dfs_cycle(dep, visited, in_stack)) return true;
                } else if (in_stack.contains(dep)) {
                    return true;
                }
            }
        }
        in_stack.erase(id);
        return false;
    }

    bool cycle_from_locked(TaskId start,
                           const std::unordered_set<TaskId>& waiting) const {
        std::unordered_set<TaskId> visited;
        std::function<bool(TaskId)> dfs = [&](TaskId id) -> bool {
            if (!visited.insert(id).second) return false;
            auto it = nodes_.find(id);
            if (it == nodes_.end()) return false;
            for (TaskId dep : it->second.deps) {
                if (dep == start) return true;
                if (waiting.contains(dep) && dfs(dep)) return true;
            }
            return false;
        };
        return dfs(start);
    }

    std::vector<TaskId> topological_sort_locked() const {
        std::unordered_map<TaskId, int> in_degree;
        for (auto& [id, node] : nodes_) {
            if (!in_degree.contains(id)) in_degree[id] = 0;
            for (TaskId dep : node.deps) in_degree[dep]++;
        }
        std::queue<TaskId> q;
        for (auto& [id, deg] : in_degree)
            if (deg == 0) q.push(id);
        std::vector<TaskId> result;
        while (!q.empty()) {
            TaskId cur = q.front(); q.pop();
            result.push_back(cur);
            auto it = nodes_.find(cur);
            if (it == nodes_.end()) continue;
            for (TaskId dep : it->second.deps) {
                if (--in_degree[dep] == 0) q.push(dep);
            }
        }
        return result;
    }

    mutable std::mutex                     mu_;
    std::unordered_map<TaskId, Node>       nodes_;
    std::unordered_set<TaskId>             completed_;
    std::unordered_set<TaskId>             enqueued_; // 已入队的任务，避免 complete_task 重复返回
};

// ─────────────────────────────────────────────────────────────
// § 2.3  Scheduler 主体
// ─────────────────────────────────────────────────────────────

enum class SchedulerPolicy { FIFO, RoundRobin, Priority };

class Scheduler : private NonCopyable {
public:
    explicit Scheduler(SchedulerPolicy policy = SchedulerPolicy::Priority,
                       uint32_t thread_pool_size = 4)
        : policy_(policy), thread_pool_size_(thread_pool_size) {}

    ~Scheduler() { shutdown(); }

    void start() {
        running_ = true;
        for (uint32_t i = 0; i < thread_pool_size_; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        dispatcher_ = std::jthread([this](std::stop_token st) {
            dispatch_loop(st);
        });
    }

    void shutdown() {
        bool was_running = running_.exchange(false);
        if (!was_running) return; // 防止重复 shutdown
        cv_.notify_all();
        done_cv_.notify_all();
        auto this_id = std::this_thread::get_id();
        for (auto& w : workers_) {
            // Avoid self-join if shutdown called from worker thread
            if (w.joinable() && w.get_id() != this_id)
                w.join();
        }
        workers_.clear();
        if (dispatcher_.joinable()) {
            dispatcher_.request_stop();
            if (dispatcher_.get_id() != this_id)
                dispatcher_.join();
        }
    }

    // 提交任务
    Result<TaskId> submit(TaskPtr task) {
        // 先注册节点，再添加依赖边（否则 add_task 会覆盖 deps）
        dep_graph_.add_task(task->id, task->priority);
        for (TaskId dep : task->depends_on) {
            auto res = dep_graph_.add_dependency(task->id, dep);
            if (!res) return make_unexpected(res.error());
        }

        {
            std::lock_guard lk(mu_);
            all_tasks_[task->id] = task;
            // 若无未满足依赖，立即加入就绪队列
            if (dep_graph_.is_ready(task->id)) {
                dep_graph_.mark_enqueued(task->id);
                enqueue_ready_locked(task);
            }
        }
        cv_.notify_one();
        return task->id;
    }

    // 取消任务
    bool cancel(TaskId id) {
        {
            std::lock_guard lk(mu_);
            auto it = all_tasks_.find(id);
            if (it == all_tasks_.end()) return false;
            it->second->state = TaskState::Cancelled;
        }
        done_cv_.notify_all(); // Wake up wait_for() callers
        return true;
    }

    // 等待任务完成（同步），使用条件变量替代轮询
    bool wait_for(TaskId id, Duration timeout = Duration{10000}) {
        std::unique_lock lk(mu_);
        auto it = all_tasks_.find(id);
        if (it == all_tasks_.end()) return false;
        return done_cv_.wait_for(lk, timeout, [&] {
            auto state = it->second->state.load();
            return state == TaskState::Completed || state == TaskState::Failed
                || state == TaskState::Cancelled;
        });
    }

    TaskState task_state(TaskId id) const {
        std::lock_guard lk(mu_);
        auto it = all_tasks_.find(id);
        if (it == all_tasks_.end()) return TaskState::Failed;
        return it->second->state.load();
    }

    DependencyGraph& dep_graph() { return dep_graph_; }

private:
    // 内部就绪队列入口（需持有 mu_）
    void enqueue_ready_locked(TaskPtr task) {
        task->state = TaskState::Pending;
        if (policy_ == SchedulerPolicy::Priority) {
            // 计算有效优先级（基础 + 关键路径加权）
            std::unordered_map<TaskId, int> boosts;
            dep_graph_.boost_critical_path(boosts);
            int eff_priority = static_cast<int>(task->priority);
            if (auto it = boosts.find(task->id); it != boosts.end())
                eff_priority += it->second;
            priority_queue_.emplace(eff_priority, task);
        } else {
            fifo_queue_.push(task);
        }
    }

    // 工作线程主循环
    void worker_loop() {
        while (running_) {
            TaskPtr task = dequeue_task();
            if (!task) continue;

            // Atomic state transition: only run if still Pending
            auto expected = TaskState::Pending;
            if (!task->state.compare_exchange_strong(expected, TaskState::Running)) {
                continue; // Cancelled or already running
            }

            try {
                task->work();
                task->state = TaskState::Completed;
            } catch (const std::exception& e) {
                task->state = TaskState::Failed;
            }

            // 唤醒所有在 wait_for() 上等待的线程
            done_cv_.notify_all();

            // 通知依赖该任务的下游任务就绪
            auto ready_ids = dep_graph_.complete_task(task->id);
            {
                std::lock_guard lk(mu_);
                for (TaskId rid : ready_ids) {
                    auto it = all_tasks_.find(rid);
                    if (it != all_tasks_.end() &&
                        it->second->state == TaskState::Pending) {
                        enqueue_ready_locked(it->second);
                        cv_.notify_one();
                    }
                }
            }
        }
    }

    // 调度分发循环（死锁监控）
    void dispatch_loop(std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // 死锁检测
            std::vector<TaskId> waiting;
            {
                std::lock_guard lk(mu_);
                for (auto& [id, t] : all_tasks_) {
                    if (t->state == TaskState::Running ||
                        t->state == TaskState::Pending) {
                        waiting.push_back(id);
                    }
                }
            }
            if (dep_graph_.detect_deadlock(waiting)) {
                // 升级：将最老的 Pending 任务强制完成以打破死锁
                std::lock_guard lk(mu_);
                TaskId oldest = 0;
                TimePoint oldest_time = TimePoint::max();
                for (TaskId wid : waiting) {
                    auto it = all_tasks_.find(wid);
                    if (it != all_tasks_.end() &&
                        it->second->state == TaskState::Pending &&
                        it->second->enqueue_time < oldest_time) {
                        oldest_time = it->second->enqueue_time;
                        oldest = wid;
                    }
                }
                if (oldest) {
                    all_tasks_[oldest]->state = TaskState::Cancelled;
                    dep_graph_.complete_task(oldest); // 强制解除阻塞
                }
            }
        }
    }

    TaskPtr dequeue_task() {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] {
            return !running_ ||
                   !priority_queue_.empty() ||
                   !fifo_queue_.empty();
        });
        if (!running_) return nullptr;

        if (policy_ == SchedulerPolicy::Priority && !priority_queue_.empty()) {
            auto task = priority_queue_.top().second;
            priority_queue_.pop();
            return task;
        }
        if (!fifo_queue_.empty()) {
            auto task = fifo_queue_.front();
            fifo_queue_.pop();
            return task;
        }
        return nullptr;
    }

    // ── 优先级队列（最大堆）────────────────────────────────────
    using PQEntry = std::pair<int, TaskPtr>;
    struct PQCmp {
        bool operator()(const PQEntry& a, const PQEntry& b) const {
            return a.first < b.first; // 高优先级在顶
        }
    };
    std::priority_queue<PQEntry, std::vector<PQEntry>, PQCmp> priority_queue_;
    std::queue<TaskPtr>                                        fifo_queue_;

    mutable std::mutex                         mu_;
    std::condition_variable                    cv_;       // 工作线程调度用
    std::condition_variable                    done_cv_;  // wait_for() 用
    std::atomic<bool>                          running_{false};
    std::vector<std::thread>                   workers_;
    std::jthread                               dispatcher_;
    std::unordered_map<TaskId, TaskPtr>        all_tasks_;
    DependencyGraph                            dep_graph_;
    SchedulerPolicy                            policy_;
    uint32_t                                   thread_pool_size_;

    static std::atomic<TaskId> next_task_id_;
public:
    static TaskId new_task_id() { return ++next_task_id_; }
};

} // namespace agentos::scheduler
