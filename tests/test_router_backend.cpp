#include <agentos/kernel/router_backend.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::kernel;

class RouterBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        simple_ = std::make_shared<MockLLMBackend>();
        simple_->register_rule("", "simple response");

        complex_ = std::make_shared<MockLLMBackend>();
        complex_->register_rule("", "complex response");
    }

    std::shared_ptr<MockLLMBackend> simple_;
    std::shared_ptr<MockLLMBackend> complex_;
};

TEST_F(RouterBackendTest, NameReturnsRouter) {
    RouterBackend router;
    EXPECT_EQ(router.name(), "router");
}

TEST_F(RouterBackendTest, DefaultBackendUsedWhenNoRules) {
    RouterBackend router;
    router.set_default(simple_);

    LLMRequest req;
    req.messages.push_back(Message::user("hello"));
    auto r = router.complete(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content, "simple response");
}

TEST_F(RouterBackendTest, RuleMatchesFirst) {
    RouterBackend router;
    router.set_default(simple_);
    router.add_rule({
        .name = "complex",
        .predicate = routing::by_complexity(10),
        .backend = complex_
    });

    // Short message -> default (simple)
    LLMRequest short_req;
    short_req.messages.push_back(Message::user("hi"));
    auto r1 = router.complete(short_req);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->content, "simple response");

    // Long message -> complex
    LLMRequest long_req;
    long_req.messages.push_back(Message::user(std::string(200, 'x')));
    auto r2 = router.complete(long_req);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->content, "complex response");
}

TEST_F(RouterBackendTest, PriorityRouting) {
    RouterBackend router;
    router.set_default(simple_);
    router.add_rule({
        .name = "high-priority",
        .predicate = routing::by_priority(Priority::High),
        .backend = complex_
    });

    LLMRequest req;
    req.messages.push_back(Message::user("task"));
    req.priority = Priority::Critical;

    auto* resolved = router.resolve(req);
    EXPECT_EQ(resolved, complex_.get());
}

TEST_F(RouterBackendTest, ToolRoutingPredicate) {
    RouterBackend router;
    router.set_default(simple_);
    router.add_rule({
        .name = "tool-capable",
        .predicate = routing::has_tools(),
        .backend = complex_
    });

    LLMRequest req_no_tools;
    req_no_tools.messages.push_back(Message::user("hello"));
    EXPECT_EQ(router.resolve(req_no_tools), simple_.get());

    LLMRequest req_with_tools;
    req_with_tools.messages.push_back(Message::user("hello"));
    req_with_tools.tools_json = R"([{"type":"function"}])";
    EXPECT_EQ(router.resolve(req_with_tools), complex_.get());
}

TEST_F(RouterBackendTest, NoBackendReturnsError) {
    RouterBackend router; // no default, no rules
    LLMRequest req;
    req.messages.push_back(Message::user("hello"));
    auto r = router.complete(req);
    EXPECT_FALSE(r.has_value());
}

TEST_F(RouterBackendTest, RuleCount) {
    RouterBackend router;
    EXPECT_EQ(router.rule_count(), 0u);
    router.add_rule({.name = "r1",
                     .predicate = [](const LLMRequest&) { return false; },
                     .backend = simple_});
    router.add_rule({.name = "r2",
                     .predicate = [](const LLMRequest&) { return false; },
                     .backend = complex_});
    EXPECT_EQ(router.rule_count(), 2u);
}

TEST_F(RouterBackendTest, ModelPrefixRouting) {
    RouterBackend router;
    router.set_default(simple_);
    router.add_rule({
        .name = "claude",
        .predicate = routing::by_model_prefix("claude"),
        .backend = complex_
    });

    LLMRequest req;
    req.messages.push_back(Message::user("hi"));
    req.model = "claude-sonnet-4-20250514";
    EXPECT_EQ(router.resolve(req), complex_.get());

    req.model = "gpt-4o";
    EXPECT_EQ(router.resolve(req), simple_.get());
}

TEST_F(RouterBackendTest, StreamDelegatesToResolvedBackend) {
    RouterBackend router;
    router.set_default(simple_);

    LLMRequest req;
    req.messages.push_back(Message::user("hello"));
    auto r = router.stream(req, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->content, "simple response");
}
