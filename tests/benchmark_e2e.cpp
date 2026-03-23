// ============================================================
// AgentOS :: End-to-End System Benchmarks
// Standalone executable (NOT a GTest file)
// ============================================================
#include <agentos/agentos.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace agentos;
using HiResClock = std::chrono::high_resolution_clock;

// ─────────────────────────────────────────────────────────────
// § Formatting helper — print a benchmark result row
// ─────────────────────────────────────────────────────────────

struct BenchResult {
    std::string name;
    int ops;
    double ms;
    double ops_sec;
};

static std::vector<BenchResult> g_results;

static void record(const std::string& name, int ops, double ms) {
    double ops_sec = (ms > 0.0) ? (static_cast<double>(ops) / ms * 1000.0) : 0.0;
    g_results.push_back({name, ops, ms, ops_sec});
}

static void print_table() {
    std::cout << "\n";
    std::cout << std::left << std::setw(40) << "Benchmark"
              << std::right << std::setw(10) << "Ops"
              << std::setw(14) << "Time (ms)"
              << std::setw(16) << "Ops/sec"
              << "\n";
    std::cout << std::string(80, '-') << "\n";
    for (const auto& r : g_results) {
        std::cout << std::left << std::setw(40) << r.name
                  << std::right << std::setw(10) << r.ops
                  << std::setw(14) << std::fixed << std::setprecision(2) << r.ms
                  << std::setw(16) << std::fixed << std::setprecision(0) << r.ops_sec
                  << "\n";
    }
    std::cout << std::string(80, '-') << "\n";
}

// ─────────────────────────────────────────────────────────────
// § Bench.1  Agent creation time
// ─────────────────────────────────────────────────────────────

static void bench_agent_creation() {
    constexpr int N = 100;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        (void)make_agent(*os, "agent-" + std::to_string(i))
                  .prompt("Benchmark agent " + std::to_string(i))
                  .create();
    }
    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("agent_creation", N, ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.2  Tool dispatch latency
// ─────────────────────────────────────────────────────────────

static void bench_tool_dispatch() {
    constexpr int N = 1000;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    // Register a lightweight tool
    tools::ToolSchema schema;
    schema.id = "noop";
    schema.description = "No-op benchmark tool";
    schema.params = {
        {"input", tools::ParamType::String, "Input value", true, std::nullopt}};

    os->register_tool(schema, [](const tools::ParsedArgs& args) -> tools::ToolResult {
        (void)args;
        return {true, "ok", ""};
    });

    auto agent = make_agent(*os, "tool-bench")
                     .prompt("Tool benchmark agent")
                     .tools({"noop"})
                     .create();

    kernel::ToolCallRequest call;
    call.id = "bench_call";
    call.name = "noop";
    call.args_json = R"({"input":"test"})";

    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        call.id = "call_" + std::to_string(i);
        auto r = agent->act(call);
        (void)r;
    }
    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("tool_dispatch", N, ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.3  Memory store/retrieve cycle
// ─────────────────────────────────────────────────────────────

static void bench_memory_cycle() {
    constexpr int N = 500;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    // Use shared memory for fast key-value benchmarking
    auto& smem = os->shared_memory();

    // Write phase
    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        smem.put("key_" + std::to_string(i),
                 "value_" + std::to_string(i), 1);
    }
    auto t1 = HiResClock::now();
    double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("memory_write (shared)", N, write_ms);

    // Read phase
    auto t2 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        auto val = smem.get("key_" + std::to_string(i));
        (void)val;
    }
    auto t3 = HiResClock::now();
    double read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    record("memory_read (shared)", N, read_ms);

    // Working memory write
    auto& wm = os->memory().working();
    auto t4 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        memory::MemoryEntry entry;
        entry.content = "benchmark memory entry " + std::to_string(i);
        entry.source = "bench";
        entry.importance = 0.5f;
        (void)wm.write(std::move(entry));
    }
    auto t5 = HiResClock::now();
    double wm_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();
    record("memory_write (working)", N, wm_ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.4  Concurrent agent throughput (N agents, M tasks)
// ─────────────────────────────────────────────────────────────

static void bench_concurrent_throughput() {
    constexpr int NUM_AGENTS = 8;
    constexpr int TASKS_PER_AGENT = 10;
    constexpr int TOTAL = NUM_AGENTS * TASKS_PER_AGENT;

    auto os = AgentOSBuilder().mock().security(false).threads(4).build();

    std::vector<std::shared_ptr<ReActAgent>> agents;
    agents.reserve(NUM_AGENTS);
    for (int i = 0; i < NUM_AGENTS; ++i) {
        agents.push_back(
            make_agent(*os, "concurrent-" + std::to_string(i))
                .prompt("Concurrent benchmark agent")
                .create());
    }

    std::atomic<int> completed{0};

    auto t0 = HiResClock::now();

    // Launch tasks across all agents concurrently
    std::vector<std::future<void>> futures;
    futures.reserve(TOTAL);
    for (int i = 0; i < NUM_AGENTS; ++i) {
        for (int j = 0; j < TASKS_PER_AGENT; ++j) {
            futures.push_back(std::async(std::launch::async, [&, i, j] {
                auto result = agents[i]->run("Task " + std::to_string(j));
                if (result.has_value()) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }));
        }
    }

    for (auto& f : futures) f.get();

    auto t1 = HiResClock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("concurrent_throughput (" +
               std::to_string(NUM_AGENTS) + "x" +
               std::to_string(TASKS_PER_AGENT) + ")",
           completed.load(), ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.5  Context window management
// ─────────────────────────────────────────────────────────────

static void bench_context_window() {
    constexpr int N = 2000;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    auto agent = make_agent(*os, "context-bench")
                     .prompt("Context benchmark agent")
                     .context(1000000)  // large window
                     .create();

    auto& ctx_win = os->ctx().get_window(agent->id(), agent->config().context_limit);

    // Add messages to context window
    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        kernel::Message msg = kernel::Message::user(
            "Context message number " + std::to_string(i) +
            " with some padding content to simulate real messages.");
        ctx_win.try_add(msg);
    }
    auto t1 = HiResClock::now();
    double add_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("context_window_add", N, add_ms);

    // Read messages from context window
    auto t2 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        volatile size_t count = ctx_win.message_count();
        (void)count;
        volatile auto util = ctx_win.utilization();
        (void)util;
    }
    auto t3 = HiResClock::now();
    double read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    record("context_window_query", N, read_ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.6  Agent run (think cycle) latency
// ─────────────────────────────────────────────────────────────

static void bench_agent_run_latency() {
    constexpr int N = 50;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    auto agent = make_agent(*os, "run-bench")
                     .prompt("Run latency benchmark agent")
                     .create();

    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        auto result = agent->run("Benchmark input " + std::to_string(i));
        (void)result;
    }
    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("agent_run (mock)", N, ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Bench.7  Supervisor delegation overhead
// ─────────────────────────────────────────────────────────────

static void bench_supervisor_delegation() {
    constexpr int N = 20;
    auto os = AgentOSBuilder().mock().security(false).threads(2).build();

    auto worker = make_agent(*os, "sv-worker")
                      .prompt("Worker agent")
                      .create();

    auto supervisor = make_agent(*os, "sv-supervisor")
                          .prompt("Supervisor agent")
                          .create<SupervisorAgent>();
    supervisor->add_worker(worker, "General purpose worker");

    auto t0 = HiResClock::now();
    for (int i = 0; i < N; ++i) {
        auto result = supervisor->run("Delegate task " + std::to_string(i));
        (void)result;
    }
    auto t1 = HiResClock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    record("supervisor_delegation", N, ms);

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § Main
// ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "==========================================\n";
    std::cout << "  AgentOS End-to-End Benchmarks\n";
    std::cout << "==========================================\n\n";

    bench_agent_creation();
    bench_tool_dispatch();
    bench_memory_cycle();
    bench_concurrent_throughput();
    bench_context_window();
    bench_agent_run_latency();
    bench_supervisor_delegation();

    print_table();

    std::cout << "\n  Done.\n";
    std::cout << "==========================================\n";

    return 0;
}
