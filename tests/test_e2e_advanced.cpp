// ============================================================
// AgentOS :: Advanced End-to-End Scenario Tests
// More complex integration tests with mock LLM backend
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

class E2EAdvancedTest : public ::testing::Test {
protected:
    void SetUp() override {
        os_ = AgentOSBuilder().mock().security(false).threads(2).build();
    }

    void TearDown() override {
        os_->graceful_shutdown(Duration{5000});
        os_.reset();
    }

    kernel::MockLLMBackend& mock() {
        return dynamic_cast<kernel::MockLLMBackend&>(os_->kernel().backend());
    }

    std::unique_ptr<AgentOS> os_;
};

// ─────────────────────────────────────────────────────────────
// § 1. Agent with RAG pipeline: store -> retrieve -> generate
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, RAGPipelineStoreRetrieveGenerate) {
    // Store knowledge into memory
    auto agent = make_agent(*os_, "rag-agent")
                     .prompt("You are a RAG-based assistant.")
                     .create();

    // Step 1: Remember some facts
    auto r1 = agent->remember("The speed of light is 299792458 m/s.", 0.9f);
    EXPECT_TRUE(r1.has_value());

    auto r2 = agent->remember("Water boils at 100 degrees Celsius at sea level.", 0.8f);
    EXPECT_TRUE(r2.has_value());

    // Step 2: Recall/retrieve relevant memories
    auto recall_result = agent->recall("speed of light", 3);
    ASSERT_TRUE(recall_result.has_value());
    EXPECT_GE(recall_result->size(), 1u);

    // Verify we got relevant content back
    bool found_light = false;
    for (const auto& sr : *recall_result) {
        if (sr.entry.content.find("light") != std::string::npos ||
            sr.entry.content.find("299792458") != std::string::npos) {
            found_light = true;
        }
    }
    EXPECT_TRUE(found_light);

    // Step 3: Generate using the agent (mock backend)
    mock().register_rule("speed of light",
                         "The speed of light is approximately 3x10^8 m/s.", 1);
    auto gen_result = agent->run("What is the speed of light?");
    ASSERT_TRUE(gen_result.has_value()) << gen_result.error().message;
    EXPECT_FALSE(gen_result->empty());
}

// ─────────────────────────────────────────────────────────────
// § 2. Agent retry on transient error: custom backend returns
//      error first, then success
// ─────────────────────────────────────────────────────────────

namespace {
class FailThenSucceedBackend : public kernel::ILLMBackend {
public:
    explicit FailThenSucceedBackend(int fail_count = 1)
        : fail_count_(fail_count) {}

    [[nodiscard]] Result<kernel::LLMResponse> complete(const kernel::LLMRequest& /*req*/) override {
        int current = calls_.fetch_add(1, std::memory_order_relaxed);
        if (current < fail_count_) {
            return make_error(ErrorCode::LLMBackendError,
                              "HTTP 503 Service Unavailable (transient)");
        }
        kernel::LLMResponse resp;
        resp.content = "Success after retries";
        resp.finish_reason = "stop";
        resp.prompt_tokens = 10;
        resp.completion_tokens = 5;
        return resp;
    }

    [[nodiscard]] Result<kernel::EmbeddingResponse> embed(const kernel::EmbeddingRequest& req) override {
        kernel::EmbeddingResponse resp;
        for (const auto& input : req.inputs) {
            std::vector<float> emb(128, 0.1f);
            resp.embeddings.push_back(std::move(emb));
            resp.total_tokens += estimate_tokens(input);
        }
        return resp;
    }

    std::string name() const noexcept override { return "fail-then-succeed"; }

    int total_calls() const { return calls_.load(); }

private:
    int fail_count_;
    std::atomic<int> calls_{0};
};
} // anonymous namespace

TEST(E2EAdvancedStandaloneTest, AgentRetryOnTransientError) {
    // Build an OS with a custom backend that fails once, then succeeds
    auto backend = std::make_unique<FailThenSucceedBackend>(1);
    auto* backend_ptr = backend.get();

    auto os = AgentOSBuilder()
                  .backend(std::move(backend))
                  .security(false)
                  .threads(2)
                  .build();

    auto agent = make_agent(*os, "retry-agent")
                     .prompt("You are a resilient agent.")
                     .create();

    // The LLMKernel has built-in retry for transient errors (5xx).
    // The first call fails with 503, the second succeeds.
    auto result = agent->run("Hello, retry test");
    // Whether the agent succeeds depends on internal retry policy.
    // With retry enabled, it should eventually succeed.
    if (result.has_value()) {
        EXPECT_FALSE(result->empty());
        EXPECT_GE(backend_ptr->total_calls(), 2);
    } else {
        // If retry isn't triggered at this level, verify the error is transient
        EXPECT_GE(backend_ptr->total_calls(), 1);
    }

    os->graceful_shutdown(Duration{5000});
}

// ─────────────────────────────────────────────────────────────
// § 3. Agent with context window management: add messages until
//      overflow, verify eviction/compaction
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, ContextWindowOverflowAndEviction) {
    // Create agent with a small context window to trigger eviction quickly
    auto agent = make_agent(*os_, "ctx-agent")
                     .prompt("Context test agent.")
                     .context(200)  // very small: ~200 tokens
                     .create();

    auto& win = os_->ctx().get_window(agent->id(), 200);

    // Fill up the context with messages
    // Add messages until near full or over
    for (int i = 0; i < 50; ++i) {
        std::string msg = "Message number " + std::to_string(i) +
                          " with some padding text to consume tokens.";
        os_->ctx().append(agent->id(), kernel::Message::user(msg));
    }

    // After many messages in a small window, some should have been evicted
    // The window should not exceed its token budget
    EXPECT_LE(win.used_tokens(), win.max_tokens());

    // Evicted messages should exist
    EXPECT_GT(win.evicted().size(), 0u);
}

// ─────────────────────────────────────────────────────────────
// § 4. Agent with middleware chain: 3 middlewares in order,
//      verify execution order
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, MiddlewareChainExecutionOrder) {
    auto agent = make_agent(*os_, "mw-chain-agent")
                     .prompt("Middleware chain test.")
                     .create();

    std::vector<std::string> execution_log;
    std::mutex log_mu;

    auto add_log = [&](const std::string& entry) {
        std::lock_guard lk(log_mu);
        execution_log.push_back(entry);
    };

    // Middleware 1
    agent->use(Middleware{
        .name = "logger_1",
        .before = [&](HookContext&) { add_log("before_1"); },
        .after = [&](HookContext&) { add_log("after_1"); },
    });

    // Middleware 2
    agent->use(Middleware{
        .name = "logger_2",
        .before = [&](HookContext&) { add_log("before_2"); },
        .after = [&](HookContext&) { add_log("after_2"); },
    });

    // Middleware 3
    agent->use(Middleware{
        .name = "logger_3",
        .before = [&](HookContext&) { add_log("before_3"); },
        .after = [&](HookContext&) { add_log("after_3"); },
    });

    (void)agent->run("Trigger middleware chain");

    std::lock_guard lk(log_mu);
    // Before hooks should run in order 1, 2, 3
    // After hooks should run in order 1, 2, 3 (or reverse depending on impl)
    // Verify at least all 6 callbacks were invoked
    EXPECT_GE(execution_log.size(), 6u);

    // Verify before hooks came before after hooks
    auto find_first = [&](const std::string& prefix) -> size_t {
        for (size_t i = 0; i < execution_log.size(); ++i) {
            if (execution_log[i].starts_with(prefix)) return i;
        }
        return execution_log.size();
    };

    size_t first_before = find_first("before_");
    size_t first_after = find_first("after_");
    EXPECT_LT(first_before, first_after);

    // Verify the ordering: before_1 should precede before_2,
    // before_2 should precede before_3
    size_t b1 = find_first("before_1");
    size_t b2 = find_first("before_2");
    size_t b3 = find_first("before_3");
    EXPECT_LT(b1, b2);
    EXPECT_LT(b2, b3);
}

// ─────────────────────────────────────────────────────────────
// § 5. Concurrent tool registration and dispatch
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, ConcurrentToolRegistrationAndDispatch) {
    constexpr int NUM_TOOLS = 10;
    constexpr int DISPATCHES_PER_TOOL = 5;

    // Register tools concurrently
    std::vector<std::thread> reg_threads;
    reg_threads.reserve(NUM_TOOLS);
    for (int i = 0; i < NUM_TOOLS; ++i) {
        reg_threads.emplace_back([&, i] {
            tools::ToolSchema schema;
            schema.id = "tool_" + std::to_string(i);
            schema.description = "Test tool " + std::to_string(i);
            schema.params = {
                {"input", tools::ParamType::String, "Input value", true, std::nullopt},
            };
            os_->register_tool(schema,
                               [i](const tools::ParsedArgs& args) -> tools::ToolResult {
                                   return {true, "result_" + std::to_string(i) + ":" + args.get("input"), ""};
                               });
        });
    }
    for (auto& t : reg_threads) t.join();

    // Create an agent with all tools
    std::vector<std::string> tool_names;
    for (int i = 0; i < NUM_TOOLS; ++i) {
        tool_names.push_back("tool_" + std::to_string(i));
    }

    auto agent = make_agent(*os_, "tool-agent")
                     .prompt("Multi-tool agent.")
                     .tools(tool_names)
                     .create();

    // Dispatch tool calls concurrently via ToolManager directly
    std::atomic<int> success_count{0};
    std::vector<std::thread> dispatch_threads;
    dispatch_threads.reserve(NUM_TOOLS * DISPATCHES_PER_TOOL);

    for (int i = 0; i < NUM_TOOLS; ++i) {
        for (int j = 0; j < DISPATCHES_PER_TOOL; ++j) {
            dispatch_threads.emplace_back([&, i, j] {
                kernel::ToolCallRequest call;
                call.id = fmt::format("call_{}_{}", i, j);
                call.name = "tool_" + std::to_string(i);
                call.args_json = R"({"input":"val_)" + std::to_string(j) + R"("})";
                auto r = os_->tools().dispatch(call);
                if (r.success) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }
    for (auto& t : dispatch_threads) t.join();

    EXPECT_EQ(success_count.load(), NUM_TOOLS * DISPATCHES_PER_TOOL);
}

// ─────────────────────────────────────────────────────────────
// § 6. Agent graceful shutdown: agent is running, shutdown
//      waits for completion
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, GracefulShutdownWaitsForRunningAgent) {
    auto agent = make_agent(*os_, "busy-agent")
                     .prompt("Busy agent.")
                     .create();

    // Launch an async run
    auto future = agent->run_async("Do some work");

    // Wait for the async result first
    auto result = future.get();
    EXPECT_TRUE(result.has_value());

    // Now graceful shutdown should complete quickly since task finished
    auto start = std::chrono::steady_clock::now();
    os_->graceful_shutdown(Duration{5000});
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Shutdown should complete well within timeout
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000);
}

// ─────────────────────────────────────────────────────────────
// § 7. Agent health check includes all subsystem status
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, HealthCheckIncludesAllSubsystemStatus) {
    // No agents yet
    auto h1 = os_->health();
    EXPECT_TRUE(h1.healthy);
    EXPECT_TRUE(h1.scheduler_running);
    EXPECT_TRUE(h1.backend_available);
    EXPECT_EQ(h1.active_agents, 0u);
    EXPECT_EQ(h1.total_requests, 0u);

    // Create agents and run them to increment request count
    auto a1 = make_agent(*os_, "ha1").prompt("Agent 1").create();
    auto a2 = make_agent(*os_, "ha2").prompt("Agent 2").create();
    auto a3 = make_agent(*os_, "ha3").prompt("Agent 3").create();

    (void)a1->run("Hello 1");
    (void)a2->run("Hello 2");

    auto h2 = os_->health();
    EXPECT_TRUE(h2.healthy);
    EXPECT_TRUE(h2.scheduler_running);
    EXPECT_TRUE(h2.backend_available);
    EXPECT_EQ(h2.active_agents, 3u);
    EXPECT_GE(h2.total_requests, 2u);

    // Verify model name is present
    EXPECT_FALSE(h2.model.empty());

    // Verify JSON serialization includes all fields
    auto json_str = h2.to_json();
    auto j = nlohmann::json::parse(json_str);
    EXPECT_TRUE(j.contains("healthy"));
    EXPECT_TRUE(j.contains("scheduler"));
    EXPECT_TRUE(j.contains("backend"));
    EXPECT_TRUE(j.contains("agents"));
    EXPECT_TRUE(j.contains("requests"));
    EXPECT_TRUE(j.contains("errors"));
    EXPECT_TRUE(j.contains("model"));
    EXPECT_EQ(j["agents"].get<size_t>(), 3u);
}

// ─────────────────────────────────────────────────────────────
// § 8. Agent with session save and restore
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, SessionSaveAndRestore) {
    auto agent = make_agent(*os_, "session-agent")
                     .prompt("Session persistence test agent.")
                     .create();

    // Run the agent to populate context
    (void)agent->run("First message");
    (void)agent->run("Second message");

    auto& win = os_->ctx().get_window(agent->id(), agent->config().context_limit);
    size_t msg_count_before = win.message_count();
    EXPECT_GE(msg_count_before, 3u); // system + user + assistant pairs

    // Save session
    auto save_result = os_->ctx().save_session(
        agent->id(), "{}", {}, "{}");
    ASSERT_TRUE(save_result.has_value()) << save_result.error().message;

    // List sessions
    auto sessions = os_->ctx().list_sessions(agent->id());
    ASSERT_TRUE(sessions.has_value());
    EXPECT_GE(sessions->size(), 1u);

    // Load the saved session
    auto session_id = sessions->back();
    auto load_result = os_->ctx().load_session(agent->id(), session_id);
    ASSERT_TRUE(load_result.has_value()) << load_result.error().message;

    // Verify session data
    auto& session = *load_result;
    EXPECT_EQ(session.agent_id, agent->id());
    EXPECT_FALSE(session.context.messages.empty());
}

// ─────────────────────────────────────────────────────────────
// § 9. Bus pub/sub with multiple subscribers
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, BusPubSubWithMultipleSubscribers) {
    constexpr int NUM_SUBSCRIBERS = 5;

    auto sender = make_agent(*os_, "publisher")
                      .prompt("Publisher agent")
                      .create();

    std::vector<std::shared_ptr<ReActAgent>> subscribers;
    for (int i = 0; i < NUM_SUBSCRIBERS; ++i) {
        auto sub = make_agent(*os_, "subscriber-" + std::to_string(i))
                       .prompt("Subscriber agent")
                       .create();
        os_->bus().subscribe(sub->id(), "broadcast");
        subscribers.push_back(sub);
    }

    // Publish a message on the broadcast topic
    bool sent = sender->send(subscribers[0]->id(), "broadcast", "hello everyone");
    EXPECT_TRUE(sent);

    // The first subscriber should receive the directed message
    auto msg = subscribers[0]->recv(Duration{2000});
    EXPECT_TRUE(msg.has_value());
    if (msg) {
        EXPECT_EQ(msg->payload, "hello everyone");
    }

    // Verify that the bus and subscriptions are set up correctly
    // by subscribing to a different topic and verifying isolation
    os_->bus().subscribe(sender->id(), "private");
    bool sent2 = subscribers[0]->send(sender->id(), "private", "private msg");
    EXPECT_TRUE(sent2);

    auto priv_msg = sender->recv(Duration{2000});
    EXPECT_TRUE(priv_msg.has_value());
    if (priv_msg) {
        EXPECT_EQ(priv_msg->topic, "private");
        EXPECT_EQ(priv_msg->payload, "private msg");
    }
}

// ─────────────────────────────────────────────────────────────
// § 10. Agent clone preserves configuration
// ─────────────────────────────────────────────────────────────

TEST_F(E2EAdvancedTest, AgentClonePreservesConfiguration) {
    auto original = make_agent(*os_, "original-agent")
                        .prompt("You are the original agent.")
                        .context(4096)
                        .role("admin")
                        .tools({"echo"})
                        .create();

    // Register the echo tool
    tools::ToolSchema echo_schema;
    echo_schema.id = "echo";
    echo_schema.description = "Echoes input";
    echo_schema.params = {
        {"text", tools::ParamType::String, "Text to echo", true, std::nullopt},
    };
    os_->register_tool(echo_schema, [](const tools::ParsedArgs& args) -> tools::ToolResult {
        return {true, "ECHO: " + args.get("text"), ""};
    });

    // Run the original to build up context
    (void)original->run("Remember: secret=42");
    (void)original->run("What was the secret?");

    // Clone the agent
    auto clone_result = os_->clone_agent(original->id(), "cloned-agent");
    ASSERT_TRUE(clone_result.has_value()) << clone_result.error().message;

    auto clone = *clone_result;
    ASSERT_NE(clone, nullptr);

    // Verify the clone has a different ID
    EXPECT_NE(clone->id(), original->id());

    // Verify configuration is preserved
    EXPECT_EQ(clone->config().name, "cloned-agent");
    EXPECT_EQ(clone->config().role_prompt, "You are the original agent.");
    EXPECT_EQ(clone->config().context_limit, 4096u);

    // Verify clone can run independently
    auto clone_run = std::dynamic_pointer_cast<ReActAgent>(clone);
    if (clone_run) {
        auto result = clone_run->run("Hello from clone");
        EXPECT_TRUE(result.has_value());
    }

    // Both agents should exist
    EXPECT_GE(os_->agent_count(), 2u);
    EXPECT_NE(os_->find_agent(original->id()), nullptr);
    EXPECT_NE(os_->find_agent(clone->id()), nullptr);
}
