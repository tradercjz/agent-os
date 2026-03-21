#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(AgentCloneTest, CloneCreatesNewAgent) {
    auto os = quickstart_mock();
    auto original = make_agent(*os, "original")
        .prompt("You are the original")
        .context(4096)
        .create();

    auto clone_result = os->clone_agent(original->id(), "clone-1");
    ASSERT_TRUE(clone_result.has_value());

    auto& clone = *clone_result;
    EXPECT_NE(clone->id(), original->id());  // different ID
    EXPECT_EQ(clone->config().name, "clone-1");
    EXPECT_EQ(clone->config().role_prompt, "You are the original");  // same prompt
}

TEST(AgentCloneTest, CloneDefaultName) {
    auto os = quickstart_mock();
    auto original = make_agent(*os, "mybot").prompt("test").create();

    auto clone_result = os->clone_agent(original->id());
    ASSERT_TRUE(clone_result.has_value());
    EXPECT_EQ((*clone_result)->config().name, "mybot-clone");
}

TEST(AgentCloneTest, CloneNonexistentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->clone_agent(99999);
    EXPECT_FALSE(r.has_value());
}

TEST(AgentCloneTest, ClonePreservesAgentCount) {
    auto os = quickstart_mock();
    auto original = make_agent(*os, "src").prompt("p").create();
    size_t before = os->agent_count();

    (void)os->clone_agent(original->id());
    EXPECT_EQ(os->agent_count(), before + 1);
}

TEST(AgentCloneTest, ClonePreservesContextLimit) {
    auto os = quickstart_mock();
    auto original = make_agent(*os, "ctx-test")
        .prompt("test prompt")
        .context(2048)
        .create();

    auto clone_result = os->clone_agent(original->id(), "ctx-clone");
    ASSERT_TRUE(clone_result.has_value());
    EXPECT_EQ((*clone_result)->config().context_limit, 2048u);
}
