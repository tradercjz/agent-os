#include <agentos/tracing.hpp>
#include <agentos/agent.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <filesystem>

using namespace agentos;
using namespace agentos::tracing;
using json = nlohmann::json;

// ── Basic Tracer tests (no AgentOS needed) ───────────────────

class TracerTest : public ::testing::Test {
protected:
    Tracer tracer_{TracerConfig{.max_traces = 10, .max_input_length = 50}};
};

// 1. BeginEndTrace
TEST_F(TracerTest, BeginEndTrace) {
    AgentId aid = 42;
    auto tid = tracer_.begin_trace(aid, "solve math problem");
    ASSERT_FALSE(tid.empty());

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->trace_id, tid);
    EXPECT_EQ(t->agent_id, aid);
    EXPECT_EQ(t->goal, "solve math problem");
    EXPECT_TRUE(t->spans.empty());
    EXPECT_TRUE(t->success);  // default before end

    tracer_.end_trace(tid, true);

    auto t2 = tracer_.get_trace(tid);
    ASSERT_TRUE(t2.has_value());
    EXPECT_TRUE(t2->success);
    EXPECT_EQ(t2->total_tokens, 0u);
}

// 2. BeginEndSpan
TEST_F(TracerTest, BeginEndSpan) {
    auto tid = tracer_.begin_trace(1, "test goal");
    auto sid = tracer_.begin_span(tid, "", "think", "user says hi");
    ASSERT_FALSE(sid.empty());

    tracer_.end_span(tid, sid, "assistant says hello", 100, true, "");
    tracer_.end_trace(tid);

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->spans.size(), 1u);

    const auto& sp = t->spans[0];
    EXPECT_EQ(sp.id, sid);
    EXPECT_EQ(sp.parent_id, "");
    EXPECT_EQ(sp.operation, "think");
    EXPECT_EQ(sp.input, "user says hi");
    EXPECT_EQ(sp.output, "assistant says hello");
    EXPECT_EQ(sp.tokens_used, 100u);
    EXPECT_TRUE(sp.success);
    EXPECT_TRUE(sp.error.empty());
}

// 3. NestedSpans
TEST_F(TracerTest, NestedSpans) {
    auto tid = tracer_.begin_trace(1, "nested test");
    auto root_sid = tracer_.begin_span(tid, "", "think", "question");
    auto child_sid = tracer_.begin_span(tid, root_sid, "tool:http_fetch", "url");

    tracer_.end_span(tid, child_sid, "fetched data", 50);
    tracer_.end_span(tid, root_sid, "final answer", 80);
    tracer_.end_trace(tid);

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->spans.size(), 2u);

    // Root span has no parent
    EXPECT_EQ(t->spans[0].parent_id, "");
    // Child span references root as parent
    EXPECT_EQ(t->spans[1].parent_id, root_sid);
}

// 4. TraceTokenAggregation
TEST_F(TracerTest, TraceTokenAggregation) {
    auto tid = tracer_.begin_trace(1, "token test");
    auto s1 = tracer_.begin_span(tid, "", "think", "q1");
    tracer_.end_span(tid, s1, "a1", 120);
    auto s2 = tracer_.begin_span(tid, "", "act", "q2");
    tracer_.end_span(tid, s2, "a2", 80);
    auto s3 = tracer_.begin_span(tid, "", "think", "q3");
    tracer_.end_span(tid, s3, "a3", 200);

    tracer_.end_trace(tid);

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->total_tokens, 400u);  // 120 + 80 + 200
}

// 5. TraceDuration
TEST_F(TracerTest, TraceDuration) {
    auto tid = tracer_.begin_trace(1, "duration test");
    // Introduce a small delay so duration is non-zero
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    tracer_.end_trace(tid);

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    EXPECT_GT(t->duration().count(), 0);
}

// 6. InputTruncation
TEST_F(TracerTest, InputTruncation) {
    // Config has max_input_length = 50
    auto tid = tracer_.begin_trace(1, "truncation test");
    std::string long_input(200, 'x');  // 200 chars, way over 50
    auto sid = tracer_.begin_span(tid, "", "think", long_input);
    tracer_.end_span(tid, sid, long_input, 10);
    tracer_.end_trace(tid);

    auto t = tracer_.get_trace(tid);
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->spans.size(), 1u);

    // Input should be truncated to 50 chars + "...[truncated]"
    const auto& input = t->spans[0].input;
    EXPECT_LE(input.size(), 50u + std::string("...[truncated]").size());
    EXPECT_TRUE(input.find("...[truncated]") != std::string::npos);

    // Output should also be truncated
    const auto& output = t->spans[0].output;
    EXPECT_TRUE(output.find("...[truncated]") != std::string::npos);
}

// 7. RecentTraces
TEST_F(TracerTest, RecentTraces) {
    // Create 5 traces
    for (int i = 0; i < 5; ++i) {
        auto tid = tracer_.begin_trace(static_cast<AgentId>(i), "goal " + std::to_string(i));
        tracer_.end_trace(tid);
    }
    EXPECT_EQ(tracer_.trace_count(), 5u);

    // Ask for latest 3
    auto recent = tracer_.recent_traces(3);
    ASSERT_EQ(recent.size(), 3u);

    // Should be the last 3 (agent_ids 2, 3, 4)
    EXPECT_EQ(recent[0].agent_id, 2u);
    EXPECT_EQ(recent[1].agent_id, 3u);
    EXPECT_EQ(recent[2].agent_id, 4u);
}

// 8. CapacityEviction
TEST_F(TracerTest, CapacityEviction) {
    // max_traces = 10, create 11
    std::string first_tid;
    for (int i = 0; i < 11; ++i) {
        auto tid = tracer_.begin_trace(static_cast<AgentId>(i), "goal");
        if (i == 0) first_tid = tid;
        tracer_.end_trace(tid);
    }

    // Should have exactly max_traces
    EXPECT_EQ(tracer_.trace_count(), 10u);

    // The oldest (first) trace should be evicted
    auto evicted = tracer_.get_trace(first_tid);
    EXPECT_FALSE(evicted.has_value());

    // The most recent should still exist
    auto recent = tracer_.recent_traces(1);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].agent_id, 10u);
}

// 9. JsonExport
TEST_F(TracerTest, JsonExport) {
    auto tid = tracer_.begin_trace(7, "json export test");
    auto s1 = tracer_.begin_span(tid, "", "think", "hello");
    tracer_.end_span(tid, s1, "world", 42);
    auto s2 = tracer_.begin_span(tid, s1, "tool:search", "query");
    tracer_.end_span(tid, s2, "results", 10, false, "timeout");
    tracer_.end_trace(tid);

    auto json_str = tracer_.export_json(tid);
    ASSERT_FALSE(json_str.empty());
    ASSERT_NE(json_str, "{}");

    auto j = json::parse(json_str);
    EXPECT_EQ(j["trace_id"].get<std::string>(), tid);
    EXPECT_EQ(j["agent_id"].get<uint64_t>(), 7u);
    EXPECT_EQ(j["goal"].get<std::string>(), "json export test");
    EXPECT_TRUE(j.contains("start_time_ms"));
    EXPECT_TRUE(j.contains("duration_ms"));
    EXPECT_EQ(j["total_tokens"].get<uint32_t>(), 52u);
    EXPECT_TRUE(j["success"].get<bool>());

    auto& spans = j["spans"];
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0]["operation"].get<std::string>(), "think");
    EXPECT_EQ(spans[0]["input"].get<std::string>(), "hello");
    EXPECT_EQ(spans[0]["output"].get<std::string>(), "world");
    EXPECT_EQ(spans[0]["tokens_used"].get<uint32_t>(), 42u);
    EXPECT_TRUE(spans[0]["success"].get<bool>());

    EXPECT_EQ(spans[1]["operation"].get<std::string>(), "tool:search");
    EXPECT_EQ(spans[1]["parent_span_id"].get<std::string>(), s1);
    EXPECT_FALSE(spans[1]["success"].get<bool>());
    EXPECT_EQ(spans[1]["error"].get<std::string>(), "timeout");
}

// 10. DisabledTracer
TEST(TracerDisabledTest, DisabledTracer) {
    TracerConfig cfg;
    cfg.enabled = false;
    Tracer tracer(cfg);

    EXPECT_FALSE(tracer.enabled());

    auto tid = tracer.begin_trace(1, "should be disabled");
    EXPECT_TRUE(tid.empty());  // disabled tracer returns empty trace_id

    // Operations on empty trace_id should be no-ops (no crash)
    auto sid = tracer.begin_span(tid, "", "think", "input");
    EXPECT_TRUE(sid.empty());
    tracer.end_span(tid, sid, "output", 100);
    tracer.end_trace(tid);

    EXPECT_EQ(tracer.trace_count(), 0u);
    auto recent = tracer.recent_traces(10);
    EXPECT_TRUE(recent.empty());
}

// 11. MiddlewareIntegration — use middleware hooks to record tracing spans
class TracerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mock = std::make_unique<kernel::MockLLMBackend>("test-llm");
        mock->register_rule("test", "response text");
        mock->register_rule("hello", "hello back");

        AgentOS::Config cfg;
        cfg.scheduler_threads = 2;
        cfg.snapshot_dir = (std::filesystem::temp_directory_path() /
                            "agentos_tracing_test_snap").string();
        cfg.ltm_dir = (std::filesystem::temp_directory_path() /
                       "agentos_tracing_test_ltm").string();

        os_ = std::make_unique<AgentOS>(std::move(mock), cfg);
    }

    void TearDown() override {
        os_.reset();
        std::filesystem::remove_all(
            std::filesystem::temp_directory_path() / "agentos_tracing_test_snap");
        std::filesystem::remove_all(
            std::filesystem::temp_directory_path() / "agentos_tracing_test_ltm");
    }

    std::unique_ptr<AgentOS> os_;
};

TEST_F(TracerIntegrationTest, MiddlewareIntegration) {
    auto& tracer = os_->tracer();

    AgentConfig acfg;
    acfg.name = "TracedAgent";
    acfg.role_prompt = "You are helpful.";

    auto agent = os_->create_agent(acfg);

    // Start a trace for this agent
    auto tid = tracer.begin_trace(agent->id(), "middleware integration test");
    ASSERT_FALSE(tid.empty());

    // Install middleware that records spans via the tracer
    std::string captured_tid = tid;
    agent->use(Middleware{
        .name = "tracing_middleware",
        .before = [&tracer, captured_tid](HookContext& ctx) {
            // Record span start — store span_id in input for retrieval
            auto sid = tracer.begin_span(captured_tid, "", ctx.operation, ctx.input);
            // Stash span_id into cancel_reason field (hack for testing)
            ctx.cancel_reason = sid;
        },
        .after = [&tracer, captured_tid](HookContext& ctx) {
            auto sid = ctx.cancel_reason;
            if (!sid.empty()) {
                tracer.end_span(captured_tid, sid, "completed", 50);
            }
        }
    });

    // Run think, which should trigger the middleware
    auto resp = agent->think("hello");
    ASSERT_TRUE(resp);

    tracer.end_trace(tid);

    // Verify the tracer recorded spans
    auto traces = tracer.recent_traces(1);
    ASSERT_EQ(traces.size(), 1u);
    EXPECT_EQ(traces[0].trace_id, tid);
    EXPECT_FALSE(traces[0].spans.empty());

    // Check that a "think" span was recorded
    bool found_think = false;
    for (const auto& sp : traces[0].spans) {
        if (sp.operation == "think") {
            found_think = true;
            EXPECT_EQ(sp.tokens_used, 50u);
        }
    }
    EXPECT_TRUE(found_think) << "Expected a 'think' span from middleware";
    EXPECT_EQ(traces[0].total_tokens, 50u);
}
