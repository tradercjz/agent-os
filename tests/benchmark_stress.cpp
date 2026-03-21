// ============================================================
// AgentOS :: Stress / Performance Benchmarks
// Standalone executable (NOT a GTest file)
// ============================================================
#include <agentos/scheduler/scheduler.hpp>
#include <agentos/memory/memory.hpp>
#include <agentos/bus/agent_bus.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace agentos;
using HiResClock = std::chrono::high_resolution_clock;

// ── Benchmark 1: Concurrent agent scheduling throughput ──────
static void benchmark_scheduler_throughput() {
    constexpr int NUM_TASKS = 1000;
    constexpr uint32_t THREAD_POOL = 4;

    scheduler::Scheduler sched(scheduler::SchedulerPolicy::FIFO, THREAD_POOL);
    sched.start();

    std::atomic<int> completed{0};

    auto t0 = HiResClock::now();

    for (int i = 0; i < NUM_TASKS; ++i) {
        auto task = std::make_shared<scheduler::AgentTaskDescriptor>();
        task->id = scheduler::Scheduler::new_task_id();
        task->agent_id = 1;
        task->priority = Priority::Normal;
        task->name = "bench_task_" + std::to_string(i);
        task->work = [&completed] {
            // Simulate a tiny bit of work
            volatile int x = 0;
            for (int j = 0; j < 100; ++j) x += j;
            (void)x;
            completed.fetch_add(1, std::memory_order_relaxed);
        };
        (void)sched.submit(task);
    }

    // Wait for all tasks to finish
    (void)sched.drain(Duration{30'000});
    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ops_sec = (ms > 0.0) ? (static_cast<double>(completed.load()) / ms * 1000.0) : 0.0;

    std::cout << "scheduler_throughput: " << completed.load() << " ops in "
              << ms << " ms (" << ops_sec << " ops/sec)\n";

    sched.shutdown();
}

// ── Benchmark 2: Memory system write throughput ──────────────
static void benchmark_memory_write() {
    constexpr int NUM_ENTRIES = 10000;

    memory::WorkingMemory wm(NUM_ENTRIES + 1);

    auto t0 = HiResClock::now();

    for (int i = 0; i < NUM_ENTRIES; ++i) {
        memory::MemoryEntry entry;
        entry.content = "benchmark entry " + std::to_string(i);
        entry.source = "bench";
        entry.importance = 0.5f;
        (void)wm.write(std::move(entry));
    }

    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ops_sec = (ms > 0.0) ? (static_cast<double>(NUM_ENTRIES) / ms * 1000.0) : 0.0;

    std::cout << "memory_write: " << NUM_ENTRIES << " ops in "
              << ms << " ms (" << ops_sec << " ops/sec)\n";
}

// ── Benchmark 3: Concurrent bus message throughput ───────────
static void benchmark_bus_throughput() {
    constexpr int NUM_AGENTS = 10;
    constexpr int MSGS_PER_AGENT = 1000;

    bus::AgentBus bus_hub;

    // Register agents and subscribe to a common topic
    std::vector<std::shared_ptr<bus::Channel>> channels;
    for (int i = 1; i <= NUM_AGENTS; ++i) {
        auto ch = bus_hub.register_agent(static_cast<AgentId>(i));
        channels.push_back(ch);
        bus_hub.subscribe(static_cast<AgentId>(i), "bench");
    }

    std::atomic<int> sent{0};

    auto t0 = HiResClock::now();

    std::vector<std::thread> senders;
    senders.reserve(NUM_AGENTS);
    for (int i = 1; i <= NUM_AGENTS; ++i) {
        senders.emplace_back([&bus_hub, &sent, i] {
            for (int m = 0; m < MSGS_PER_AGENT; ++m) {
                auto msg = bus::BusMessage::make_event(
                    static_cast<AgentId>(i), "bench",
                    "{\"seq\":" + std::to_string(m) + "}");
                bus_hub.publish(std::move(msg));
                sent.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : senders) t.join();

    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int total = sent.load();
    double ops_sec = (ms > 0.0) ? (static_cast<double>(total) / ms * 1000.0) : 0.0;

    std::cout << "bus_throughput: " << total << " ops in "
              << ms << " ms (" << ops_sec << " ops/sec)\n";
}

int main() {
    std::cout << "==========================================\n";
    std::cout << "  AgentOS Stress Benchmarks\n";
    std::cout << "==========================================\n";

    benchmark_scheduler_throughput();
    benchmark_memory_write();
    benchmark_bus_throughput();

    std::cout << "==========================================\n";
    std::cout << "  Done.\n";
    std::cout << "==========================================\n";

    return 0;
}
