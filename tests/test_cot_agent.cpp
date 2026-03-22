#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

// ── parse_response tests (static, no agent needed) ──────────

TEST(CoTAgentTest, ParseResponseWithAnswer) {
    auto [reasoning, answer] = CoTAgent::parse_response(
        "Let me think...\nFirst, 2+2=4\nANSWER: 4");
    EXPECT_NE(reasoning.find("think"), std::string::npos);
    EXPECT_EQ(answer, "4");
}

TEST(CoTAgentTest, ParseResponseNoDelimiter) {
    auto [reasoning, answer] = CoTAgent::parse_response("Just a direct answer");
    EXPECT_EQ(reasoning, "Just a direct answer");
    EXPECT_TRUE(answer.empty());
}

TEST(CoTAgentTest, ParseResponseLowercaseAnswer) {
    auto [reasoning, answer] = CoTAgent::parse_response(
        "Step 1: add them\nAnswer: 42");
    EXPECT_EQ(answer, "42");
    EXPECT_NE(reasoning.find("Step 1"), std::string::npos);
}

TEST(CoTAgentTest, ParseResponseAnswerWithLeadingWhitespace) {
    auto [reasoning, answer] = CoTAgent::parse_response(
        "Reasoning here\nANSWER:   trimmed   ");
    // Leading whitespace trimmed, trailing preserved
    EXPECT_FALSE(answer.empty());
    EXPECT_NE(answer[0], ' ');
}

TEST(CoTAgentTest, ParseResponseEmptyAfterDelimiter) {
    auto [reasoning, answer] = CoTAgent::parse_response("Thinking...\nANSWER:");
    EXPECT_TRUE(answer.empty());
    EXPECT_NE(reasoning.find("Thinking"), std::string::npos);
}

// ── Integration tests with mock backend ─────────────────────

TEST(CoTAgentTest, RunWithMockBackend) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "cot-test")
                     .prompt("You are helpful")
                     .create_cot();
    auto result = agent->run("What is 2+2?");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(CoTAgentTest, SetCotPromptDoesNotCrash) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "cot-custom")
                     .prompt("You are a math tutor")
                     .create_cot();
    agent->set_cot_prompt("Please reason carefully before answering. End with ANSWER:");
    auto result = agent->run("Solve 3+5");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

TEST(CoTAgentTest, SetMaxReasoningTokens) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "cot-tokens")
                     .prompt("helpful")
                     .create_cot();
    agent->set_max_reasoning_tokens(512);
    auto result = agent->run("test");
    ASSERT_TRUE(result.has_value());
}

TEST(CoTAgentTest, CreateViaTemplateMethod) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "cot-tmpl")
                     .prompt("helpful")
                     .create<CoTAgent>();
    auto result = agent->run("hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}
