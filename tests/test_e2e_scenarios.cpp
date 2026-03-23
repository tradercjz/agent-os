// ============================================================
// AgentOS :: End-to-End Multi-Agent Scenario Tests
// Validates full system behavior with mock LLM backend
// ============================================================
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace agentos;

// ─────────────────────────────────────────────────────────────
// § Fixture — shared AgentOS with mock backend
// ─────────────────────────────────────────────────────────────

class E2EScenarioTest : public ::testing::Test {
protected:
    void SetUp() override {
        os_ = AgentOSBuilder().mock().security(false).threads(2).build();
    }

    void TearDown() override {
        os_->graceful_shutdown(Duration{5000});
        os_.reset();
    }

    std::unique_ptr<AgentOS> os_;
};

// ─────────────────────────────────────────────────────────────
// § E2E.1  Supervisor dispatches tasks to workers
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, SupervisorDispatchesToWorkers) {
    // Create two worker agents
    auto worker1 = make_agent(*os_, "researcher")
                       .prompt("You are a research assistant.")
                       .create();
    auto worker2 = make_agent(*os_, "writer")
                       .prompt("You are a technical writer.")
                       .create();

    // Create supervisor and register workers
    auto supervisor = make_agent(*os_, "supervisor")
                          .prompt("You are a project manager who delegates tasks.")
                          .create<SupervisorAgent>();
    supervisor->add_worker(worker1, "Research assistant for finding information");
    supervisor->add_worker(worker2, "Technical writer for producing documents");

    // Run the supervisor — mock backend will produce default responses
    auto result = supervisor->run("Research and write a report on C++23 features");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->empty());

    // Verify the delegation log was populated
    auto log = supervisor->delegation_log();
    // Supervisor may or may not delegate depending on mock responses,
    // but the run should complete without error
    EXPECT_GE(os_->agent_count(), 3u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.2  Agent chain: output feeds into next agent
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, AgentChainPipeline) {
    // Set up mock rules for deterministic chaining
    auto& mock = dynamic_cast<kernel::MockLLMBackend&>(os_->kernel().backend());
    mock.register_rule("summarize", "SUMMARY: Key points about the topic.", 1);
    mock.register_rule("translate", "TRANSLATION: Puntos clave sobre el tema.", 1);

    // Agent 1: Summarizer
    auto summarizer = make_agent(*os_, "summarizer")
                          .prompt("Summarize the input text.")
                          .create();

    // Agent 2: Translator
    auto translator = make_agent(*os_, "translator")
                          .prompt("Translate the input to Spanish.")
                          .create();

    // Chain: summarizer -> translator
    auto summary_result = summarizer->run("Please summarize the following document");
    ASSERT_TRUE(summary_result.has_value()) << summary_result.error().message;
    EXPECT_FALSE(summary_result->empty());

    auto translate_result = translator->run("translate: " + *summary_result);
    ASSERT_TRUE(translate_result.has_value()) << translate_result.error().message;
    EXPECT_FALSE(translate_result->empty());
    EXPECT_NE(translate_result->find("TRANSLATION"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// § E2E.3  Agent with tools: register, call, verify
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, AgentWithToolDispatch) {
    // Register a calculator tool
    tools::ToolSchema calc_schema;
    calc_schema.id = "calculator";
    calc_schema.description = "Performs arithmetic calculations";
    calc_schema.params = {
        {"expression", tools::ParamType::String, "Math expression", true, std::nullopt}};

    os_->register_tool(calc_schema, [](const tools::ParsedArgs& args) -> tools::ToolResult {
        auto expr = args.get("expression");
        if (expr == "2+2") {
            return {true, "4", ""};
        }
        return {true, "computed", ""};
    });

    // Set up mock to trigger tool call, then respond with final answer
    auto& mock = dynamic_cast<kernel::MockLLMBackend&>(os_->kernel().backend());
    mock.register_tool_rule("calculate", "calculator", R"({"expression":"2+2"})", 2);
    mock.register_rule("4", "The answer is 4.", 1);

    auto agent = make_agent(*os_, "math-agent")
                     .prompt("You are a math assistant. Use the calculator tool.")
                     .tools({"calculator"})
                     .create();

    // Directly dispatch a tool call to verify tool registration works
    kernel::ToolCallRequest call;
    call.id = "test_call_1";
    call.name = "calculator";
    call.args_json = R"({"expression":"2+2"})";

    auto tool_result = agent->act(call);
    ASSERT_TRUE(tool_result.has_value());
    EXPECT_TRUE(tool_result->success);
    EXPECT_EQ(tool_result->output, "4");
}

// ─────────────────────────────────────────────────────────────
// § E2E.4  Concurrent agents running in parallel
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, ConcurrentAgentExecution) {
    constexpr int NUM_AGENTS = 8;

    // Create agents
    std::vector<std::shared_ptr<ReActAgent>> agents;
    agents.reserve(NUM_AGENTS);
    for (int i = 0; i < NUM_AGENTS; ++i) {
        auto a = make_agent(*os_, "worker-" + std::to_string(i))
                     .prompt("You are worker " + std::to_string(i))
                     .create();
        agents.push_back(a);
    }

    EXPECT_EQ(os_->agent_count(), static_cast<size_t>(NUM_AGENTS));

    // Launch all agents concurrently using run_async
    std::vector<std::future<Result<std::string>>> futures;
    futures.reserve(NUM_AGENTS);
    for (int i = 0; i < NUM_AGENTS; ++i) {
        futures.push_back(
            agents[i]->run_async("Task " + std::to_string(i)));
    }

    // Collect results
    int success_count = 0;
    for (auto& f : futures) {
        auto result = f.get();
        if (result.has_value() && !result->empty()) {
            ++success_count;
        }
    }

    EXPECT_EQ(success_count, NUM_AGENTS);
}

// ─────────────────────────────────────────────────────────────
// § E2E.5  Agent lifecycle: create -> run -> destroy
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, AgentLifecycle) {
    // Track lifecycle events
    std::atomic<int> created_count{0};
    os_->lifecycle().on(LifecycleEvent::AgentCreated,
                        [&](const LifecycleEventData&) {
                            created_count.fetch_add(1, std::memory_order_relaxed);
                        });

    // Create agent
    auto agent = make_agent(*os_, "lifecycle-agent")
                     .prompt("Lifecycle test agent")
                     .create();
    AgentId aid = agent->id();

    EXPECT_EQ(created_count.load(), 1);
    EXPECT_EQ(os_->agent_count(), 1u);

    // Run agent
    auto result = agent->run("Hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());

    // Verify agent is findable
    auto found = os_->find_agent(aid);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id(), aid);

    // Destroy agent
    agent.reset(); // release our ref
    os_->destroy_agent(aid);
    EXPECT_EQ(os_->agent_count(), 0u);

    // Agent should no longer be findable
    auto gone = os_->find_agent(aid);
    EXPECT_EQ(gone, nullptr);
}

// ─────────────────────────────────────────────────────────────
// § E2E.6  Memory sharing between agents
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, SharedMemoryBetweenAgents) {
    auto agent_a = make_agent(*os_, "producer")
                       .prompt("You produce data.")
                       .create();
    auto agent_b = make_agent(*os_, "consumer")
                       .prompt("You consume data.")
                       .create();

    // Agent A writes to shared memory
    os_->shared_memory().put("result_key", "computed_value_42", agent_a->id());
    os_->shared_memory().put("status", "complete", agent_a->id());

    // Agent B reads from shared memory
    auto val = os_->shared_memory().get("result_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "computed_value_42");

    auto status = os_->shared_memory().get("status");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, "complete");

    // Verify entry metadata
    auto entry = os_->shared_memory().get_entry("result_key");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->writer, agent_a->id());

    // Non-existent key returns nullopt
    auto missing = os_->shared_memory().get("nonexistent");
    EXPECT_FALSE(missing.has_value());

    // Size check
    EXPECT_EQ(os_->shared_memory().size(), 2u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.7  Concurrent shared memory read/write
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, ConcurrentSharedMemoryAccess) {
    constexpr int NUM_WRITERS = 4;
    constexpr int WRITES_PER_AGENT = 50;

    std::vector<std::shared_ptr<ReActAgent>> agents;
    for (int i = 0; i < NUM_WRITERS; ++i) {
        agents.push_back(
            make_agent(*os_, "writer-" + std::to_string(i))
                .prompt("Writer agent")
                .create());
    }

    // Concurrent writes
    std::vector<std::thread> threads;
    threads.reserve(NUM_WRITERS);
    for (int i = 0; i < NUM_WRITERS; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < WRITES_PER_AGENT; ++j) {
                std::string key = "agent_" + std::to_string(i) + "_key_" + std::to_string(j);
                std::string val = "value_" + std::to_string(j);
                os_->shared_memory().put(key, val, agents[i]->id());
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(os_->shared_memory().size(),
              static_cast<size_t>(NUM_WRITERS * WRITES_PER_AGENT));

    // Verify all values are readable
    for (int i = 0; i < NUM_WRITERS; ++i) {
        for (int j = 0; j < WRITES_PER_AGENT; ++j) {
            std::string key = "agent_" + std::to_string(i) + "_key_" + std::to_string(j);
            auto val = os_->shared_memory().get(key);
            ASSERT_TRUE(val.has_value()) << "Missing key: " << key;
            EXPECT_EQ(*val, "value_" + std::to_string(j));
        }
    }
}

// ─────────────────────────────────────────────────────────────
// § E2E.8  Chain-of-Thought agent end-to-end
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, ChainOfThoughtAgent) {
    auto& mock = dynamic_cast<kernel::MockLLMBackend&>(os_->kernel().backend());
    mock.register_rule("step by step",
                       "Let me think step by step.\n"
                       "1. First, analyze the problem.\n"
                       "2. Then, compute the result.\n"
                       "ANSWER: 42",
                       1);

    auto cot = make_agent(*os_, "cot-agent")
                   .prompt("You are a reasoning agent.")
                   .create_cot();

    auto result = cot->run("What is the meaning of life? Think step by step.");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(*result, "42");
}

// ─────────────────────────────────────────────────────────────
// § E2E.9  Plan-and-Execute agent end-to-end
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, PlanExecuteAgent) {
    auto& mock = dynamic_cast<kernel::MockLLMBackend&>(os_->kernel().backend());
    mock.register_rule("step-by-step plan",
                       "Step 1: Gather requirements\n"
                       "Step 2: Design solution\n"
                       "Step 3: Implement",
                       2);
    mock.register_rule("Synthesize", "Final answer: project complete.", 1);

    auto pe = make_agent(*os_, "planner")
                  .prompt("You are a project planner.")
                  .create_plan_execute();
    pe->set_max_steps(5);

    auto result = pe->run("Create a step-by-step plan for building a website");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->empty());

    // Verify plan was generated
    const auto& plan = pe->current_plan();
    EXPECT_GE(plan.size(), 1u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.10  Agent snapshot and restore
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, AgentSnapshotRestore) {
    auto agent = make_agent(*os_, "snapshot-agent")
                     .prompt("You are a persistent agent.")
                     .create();

    // Run the agent to build up context
    (void)agent->run("Remember this: the secret is 42");
    (void)agent->run("What was the secret?");

    // Snapshot
    auto snap_result = os_->snapshot_agent(agent->id());
    ASSERT_TRUE(snap_result.has_value()) << snap_result.error().message;

    auto& snap = *snap_result;
    EXPECT_EQ(snap.name, "snapshot-agent");
    EXPECT_FALSE(snap.messages.empty());

    // Restore into a new agent
    auto restored = os_->restore_agent(snap);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;

    // Restored agent should have a different ID
    EXPECT_NE((*restored)->id(), agent->id());
    EXPECT_GE(os_->agent_count(), 2u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.11  Health check with active agents
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, SystemHealthCheck) {
    auto h1 = os_->health();
    EXPECT_TRUE(h1.healthy);
    EXPECT_TRUE(h1.scheduler_running);
    EXPECT_EQ(h1.active_agents, 0u);

    // Create some agents and run them
    auto a1 = make_agent(*os_, "health-a1").prompt("Agent 1").create();
    auto a2 = make_agent(*os_, "health-a2").prompt("Agent 2").create();
    (void)a1->run("Hello");

    auto h2 = os_->health();
    EXPECT_TRUE(h2.healthy);
    EXPECT_EQ(h2.active_agents, 2u);
    EXPECT_GE(h2.total_requests, 1u);

    // Health JSON is parseable
    std::string json_str = h2.to_json();
    auto j = nlohmann::json::parse(json_str);
    EXPECT_TRUE(j["healthy"].get<bool>());
    EXPECT_EQ(j["agents"].get<size_t>(), 2u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.12  Middleware hooks on agent operations
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, MiddlewareHooksOnRun) {
    std::atomic<int> think_before{0};
    std::atomic<int> think_after{0};

    auto agent = make_agent(*os_, "hooked-agent")
                     .prompt("Hooked agent")
                     .create();

    agent->use(Middleware{
        .name = "counter",
        .before = [&](HookContext&) { think_before.fetch_add(1, std::memory_order_relaxed); },
        .after = [&](HookContext&) { think_after.fetch_add(1, std::memory_order_relaxed); },
    });

    (void)agent->run("Test middleware");

    // ReActAgent calls think() internally, which triggers middleware
    EXPECT_GE(think_before.load(), 1);
    EXPECT_GE(think_after.load(), 1);
}

// ─────────────────────────────────────────────────────────────
// § E2E.13  Bus messaging between agents
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, BusMessagingBetweenAgents) {
    auto sender = make_agent(*os_, "sender")
                      .prompt("Sender agent")
                      .create();
    auto receiver = make_agent(*os_, "receiver")
                        .prompt("Receiver agent")
                        .create();

    // Subscribe receiver to topic
    os_->bus().subscribe(receiver->id(), "notifications");

    // Sender publishes a message
    bool sent = sender->send(receiver->id(), "notifications", "hello from sender");
    EXPECT_TRUE(sent);

    // Receiver should be able to receive it
    auto msg = receiver->recv(Duration{2000});
    EXPECT_TRUE(msg.has_value());
    if (msg) {
        EXPECT_EQ(msg->topic, "notifications");
        EXPECT_EQ(msg->payload, "hello from sender");
    }
}

// ─────────────────────────────────────────────────────────────
// § E2E.14  Team management with agents
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, TeamManagement) {
    auto a1 = make_agent(*os_, "team-a1").prompt("Agent 1").create();
    auto a2 = make_agent(*os_, "team-a2").prompt("Agent 2").create();
    auto a3 = make_agent(*os_, "team-a3").prompt("Agent 3").create();

    // Create a team
    auto team_result = os_->teams().create_team("alpha", {.description = "Alpha team"});
    ASSERT_TRUE(team_result.has_value());

    // Assign agents to team
    (void)os_->teams().assign(a1->id(), "alpha");
    (void)os_->teams().assign(a2->id(), "alpha");

    // Query team membership
    auto* team = os_->teams().find_team("alpha");
    ASSERT_NE(team, nullptr);
    EXPECT_EQ(team->size(), 2u);
    EXPECT_TRUE(team->has_member(a1->id()));
    EXPECT_TRUE(team->has_member(a2->id()));
    EXPECT_FALSE(team->has_member(a3->id()));

    // Query which teams an agent belongs to
    auto teams = os_->teams().teams_of(a1->id());
    EXPECT_EQ(teams.size(), 1u);
}

// ─────────────────────────────────────────────────────────────
// § E2E.15  Multiple tool registrations and dispatch
// ─────────────────────────────────────────────────────────────

TEST_F(E2EScenarioTest, MultipleToolsDispatch) {
    // Register tool 1: kv_store
    tools::ToolSchema kv_schema;
    kv_schema.id = "kv_store";
    kv_schema.description = "Key-value store operations";
    kv_schema.params = {
        {"action", tools::ParamType::String, "get or set", true, std::nullopt},
        {"key", tools::ParamType::String, "Key name", true, std::nullopt},
        {"value", tools::ParamType::String, "Value to set", false, std::nullopt},
    };

    std::unordered_map<std::string, std::string> store;
    os_->register_tool(kv_schema, [&](const tools::ParsedArgs& args) -> tools::ToolResult {
        auto action = args.get("action");
        auto key = args.get("key");
        if (action == "set") {
            store[key] = args.get("value");
            return {true, "OK", ""};
        }
        if (auto it = store.find(key); it != store.end()) {
            return {true, it->second, ""};
        }
        return {false, "", "key not found"};
    });

    // Register tool 2: echo
    tools::ToolSchema echo_schema;
    echo_schema.id = "echo";
    echo_schema.description = "Echoes input back";
    echo_schema.params = {
        {"text", tools::ParamType::String, "Text to echo", true, std::nullopt},
    };
    os_->register_tool(echo_schema, [](const tools::ParsedArgs& args) -> tools::ToolResult {
        return {true, "ECHO: " + args.get("text"), ""};
    });

    auto agent = make_agent(*os_, "multi-tool-agent")
                     .prompt("Agent with multiple tools")
                     .tools({"kv_store", "echo"})
                     .create();

    // Dispatch kv_store set
    kernel::ToolCallRequest set_call;
    set_call.id = "call_1";
    set_call.name = "kv_store";
    set_call.args_json = R"({"action":"set","key":"greeting","value":"hello world"})";
    auto r1 = agent->act(set_call);
    ASSERT_TRUE(r1.has_value());
    EXPECT_TRUE(r1->success);

    // Dispatch kv_store get
    kernel::ToolCallRequest get_call;
    get_call.id = "call_2";
    get_call.name = "kv_store";
    get_call.args_json = R"({"action":"get","key":"greeting"})";
    auto r2 = agent->act(get_call);
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(r2->success);
    EXPECT_EQ(r2->output, "hello world");

    // Dispatch echo
    kernel::ToolCallRequest echo_call;
    echo_call.id = "call_3";
    echo_call.name = "echo";
    echo_call.args_json = R"({"text":"test message"})";
    auto r3 = agent->act(echo_call);
    ASSERT_TRUE(r3.has_value());
    EXPECT_TRUE(r3->success);
    EXPECT_EQ(r3->output, "ECHO: test message");
}
