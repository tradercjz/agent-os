// ============================================================
// Tests for DelegationManager — agent-to-agent task delegation
// ============================================================
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(DelegationTest, DelegateToAgent) {
    auto os = quickstart_mock();
    auto* mock = dynamic_cast<kernel::MockLLMBackend*>(&os->kernel().backend());
    ASSERT_NE(mock, nullptr);
    mock->register_rule("subtask", "subtask result");

    auto agent_a = os->create_agent<ReActAgent>(AgentConfig{.name = "delegator", .role_prompt = "You delegate tasks"});
    auto agent_b = os->create_agent<ReActAgent>(AgentConfig{.name = "worker", .role_prompt = "You do work"});

    DelegationRequest req;
    req.from = agent_a->id();
    req.to = agent_b->id();
    req.task = "subtask";

    auto r = os->delegation().delegate(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->success);
    EXPECT_FALSE(r->output.empty());
}

TEST(DelegationTest, DelegateToNonexistent) {
    auto os = quickstart_mock();

    DelegationRequest req;
    req.from = 1;
    req.to = 99999;
    req.task = "impossible";

    auto r = os->delegation().delegate(req);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(DelegationTest, FanOutToMultiple) {
    auto os = quickstart_mock();
    auto a = os->create_agent<ReActAgent>(AgentConfig{.name = "coordinator", .role_prompt = "coordinate"});
    auto b = os->create_agent<ReActAgent>(AgentConfig{.name = "worker1", .role_prompt = "work"});
    auto c = os->create_agent<ReActAgent>(AgentConfig{.name = "worker2", .role_prompt = "work"});

    auto results = os->delegation().fan_out(
        a->id(), {b->id(), c->id()}, "parallel task");
    EXPECT_EQ(results.size(), 2u);
    for (const auto& r : results) {
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->success);
    }
}

TEST(DelegationTest, DelegateAsyncReturnsResult) {
    auto os = quickstart_mock();
    auto agent_a = os->create_agent<ReActAgent>(AgentConfig{.name = "async_delegator"});
    auto agent_b = os->create_agent<ReActAgent>(AgentConfig{.name = "async_worker"});

    DelegationRequest req;
    req.from = agent_a->id();
    req.to = agent_b->id();
    req.task = "async work";

    auto future = os->delegation().delegate_async(std::move(req));
    auto r = future.get();
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->success);
}

TEST(DelegationTest, DelegateTracksElapsed) {
    auto os = quickstart_mock();
    auto a = os->create_agent<ReActAgent>(AgentConfig{.name = "timer_delegator"});
    auto b = os->create_agent<ReActAgent>(AgentConfig{.name = "timer_worker"});

    DelegationRequest req;
    req.from = a->id();
    req.to = b->id();
    req.task = "timed task";

    auto r = os->delegation().delegate(req);
    ASSERT_TRUE(r.has_value());
    // elapsed should be non-negative (could be 0ms for fast mock)
    EXPECT_GE(r->elapsed.count(), 0);
}

TEST(DelegationTest, FanOutPartialFailure) {
    auto os = quickstart_mock();
    auto a = os->create_agent<ReActAgent>(AgentConfig{.name = "coord"});
    auto b = os->create_agent<ReActAgent>(AgentConfig{.name = "w1"});

    // Fan out to one existing and one non-existing agent
    auto results = os->delegation().fan_out(
        a->id(), {b->id(), 99999}, "mixed task");
    EXPECT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].has_value());
    EXPECT_FALSE(results[1].has_value());
}

TEST(DelegationTest, DelegateWithContext) {
    auto os = quickstart_mock();
    auto* mock = dynamic_cast<kernel::MockLLMBackend*>(&os->kernel().backend());
    ASSERT_NE(mock, nullptr);
    mock->register_rule("Context:", "context received");

    auto a = os->create_agent<ReActAgent>(AgentConfig{.name = "ctx_delegator"});
    auto b = os->create_agent<ReActAgent>(AgentConfig{.name = "ctx_worker"});

    DelegationRequest req;
    req.from = a->id();
    req.to = b->id();
    req.task = "do something";
    req.context = "important background info";

    auto r = os->delegation().delegate(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->success);
}
