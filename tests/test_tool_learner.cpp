#include <agentos/tools/tool_learner.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/agent.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::tools;
using namespace agentos::kernel;

// ─────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────

class ToolLearnerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mock = std::make_unique<MockLLMBackend>();
        mock_ = mock.get(); // keep raw pointer for rule registration

        // Default rule: when analyze_failure sends user msg containing "请分析工具",
        // return a param_fix structured response
        mock_->register_rule(
            "请分析工具",
            "TYPE: param_fix\nPATTERN: url missing protocol\nPARAM: url\nFIX: https://example.com",
            10);

        kernel_ = std::make_unique<LLMKernel>(std::move(mock));
        learner_ = std::make_unique<ToolLearner>(*kernel_);
    }

    ToolFailureRecord make_failure(const std::string& tool_id,
                                    const std::string& args = R"({"url":"example.com"})",
                                    const std::string& error = "url missing protocol") {
        return ToolFailureRecord{
            .tool_id = tool_id,
            .args_json = args,
            .error = error,
            .timestamp = Clock::now(),
            .agent_id = 1,
        };
    }

    MockLLMBackend* mock_{nullptr};
    std::unique_ptr<LLMKernel> kernel_;
    std::unique_ptr<ToolLearner> learner_;
};

// ─────────────────────────────────────────────────────────────
// 1. RecordFailure
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, RecordFailure) {
    auto rec = make_failure("http_fetch");
    learner_->record_failure(rec);

    EXPECT_EQ(learner_->failure_count(), 1u);
    auto failures = learner_->get_failures("http_fetch");
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].tool_id, "http_fetch");
    EXPECT_EQ(failures[0].error, "url missing protocol");
}

// ─────────────────────────────────────────────────────────────
// 2. FailureEviction
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, FailureEviction) {
    // Use a small max to trigger eviction quickly
    ToolLearnerConfig cfg{.enabled = true, .max_failures_stored = 5, .max_rules_per_tool = 10};
    ToolLearner small_learner(*kernel_, cfg);

    for (int i = 0; i < 7; ++i) {
        small_learner.record_failure(make_failure("tool_" + std::to_string(i)));
    }

    // After exceeding max_failures_stored (5), eviction removes half.
    // 6 records triggers eviction (removes 3), leaving 3. Then 7th is added -> 4.
    // Actually: records are added one at a time, eviction happens when size > 5.
    // At i=5: size becomes 6 > 5, evict half(3), left 3.
    // At i=6: size becomes 4, no eviction.
    EXPECT_LE(small_learner.failure_count(), 5u);

    // The earliest records should have been evicted
    auto early = small_learner.get_failures("tool_0");
    // tool_0 was among the earliest, should be evicted
    EXPECT_TRUE(early.empty());
}

// ─────────────────────────────────────────────────────────────
// 3. AnalyzeFailure_ParamFix
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, AnalyzeFailure_ParamFix) {
    auto rec = make_failure("http_fetch");
    learner_->analyze_failure(rec);

    auto rules = learner_->get_rules("http_fetch");
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].fix_type, "param_fix");
    EXPECT_EQ(rules[0].pattern, "url missing protocol");
    EXPECT_EQ(rules[0].param_name, "url");
    EXPECT_EQ(rules[0].fix_replacement, "https://example.com");
    EXPECT_EQ(rules[0].tool_id, "http_fetch");
}

// ─────────────────────────────────────────────────────────────
// 4. AnalyzeFailure_PromptHint
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, AnalyzeFailure_PromptHint) {
    // Register a higher-priority prompt_hint rule for a different tool
    mock_->register_rule(
        "slow_api",
        "TYPE: prompt_hint\nPATTERN: timeout\nPARAM: \nFIX: Consider using shorter timeout",
        20);

    // The user message from analyze_failure contains the tool_id, so we trigger
    // via the tool_id text. But MockLLMBackend matches on the last message which
    // is: "请分析工具 slow_api 的调用失败并给出修正规则。"
    // The trigger "slow_api" appears in that message.
    auto rec = make_failure("slow_api", R"({"timeout":30})", "request timed out");
    learner_->analyze_failure(rec);

    auto rules = learner_->get_rules("slow_api");
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].fix_type, "prompt_hint");
    EXPECT_EQ(rules[0].pattern, "timeout");
    EXPECT_EQ(rules[0].prompt_hint, "Consider using shorter timeout");
}

// ─────────────────────────────────────────────────────────────
// 5. ApplyParamFix
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, ApplyParamFix) {
    // First generate a param_fix rule via analyze_failure
    auto rec = make_failure("http_fetch");
    learner_->analyze_failure(rec);

    // The rule: param_name="url", fix_regex="url" (defaults to param_name),
    // fix_replacement="https://example.com"
    // apply_param_fixes checks if current value of "url" param contains fix_regex ("url")
    std::string input = R"({"url":"url_without_scheme"})";
    std::string result = learner_->apply_param_fixes("http_fetch", input);

    auto j = nlohmann::json::parse(result);
    EXPECT_EQ(j["url"].get<std::string>(), "https://example.com");
}

// ─────────────────────────────────────────────────────────────
// 6. ApplyParamFix_NoMatch
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, ApplyParamFix_NoMatch) {
    // Generate rule for http_fetch
    learner_->analyze_failure(make_failure("http_fetch"));

    // Call apply_param_fixes for a different tool — no rules match
    std::string input = R"({"query":"hello"})";
    std::string result = learner_->apply_param_fixes("search_tool", input);
    EXPECT_EQ(result, input);
}

// ─────────────────────────────────────────────────────────────
// 7. GetPromptHints
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, GetPromptHints) {
    // Register prompt_hint rule
    mock_->register_rule(
        "hint_tool",
        "TYPE: prompt_hint\nPATTERN: slow\nPARAM: \nFIX: Use batch mode for large inputs",
        20);

    auto rec = make_failure("hint_tool", "{}", "too slow");
    learner_->analyze_failure(rec);

    std::string hints = learner_->get_prompt_hints("hint_tool");
    EXPECT_FALSE(hints.empty());
    EXPECT_NE(hints.find("hint_tool"), std::string::npos);
    EXPECT_NE(hints.find("Use batch mode for large inputs"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// 8. GetPromptHints_NoRules
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, GetPromptHints_NoRules) {
    std::string hints = learner_->get_prompt_hints("nonexistent_tool");
    EXPECT_TRUE(hints.empty());
}

// ─────────────────────────────────────────────────────────────
// 9. MaxRulesPerTool
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, MaxRulesPerTool) {
    ToolLearnerConfig cfg{.enabled = true, .max_failures_stored = 500, .max_rules_per_tool = 3};
    ToolLearner capped_learner(*kernel_, cfg);

    // Analyze 5 failures for the same tool — should cap at 3 rules
    for (int i = 0; i < 5; ++i) {
        auto rec = make_failure("http_fetch", R"({"url":"bad"})", "error " + std::to_string(i));
        capped_learner.analyze_failure(rec);
    }

    auto rules = capped_learner.get_rules("http_fetch");
    EXPECT_LE(rules.size(), 3u);
}

// ─────────────────────────────────────────────────────────────
// 10. ClearRules
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, ClearRules) {
    learner_->analyze_failure(make_failure("http_fetch"));
    ASSERT_GT(learner_->rule_count(), 0u);

    learner_->clear_rules("http_fetch");
    EXPECT_EQ(learner_->get_rules("http_fetch").size(), 0u);

    // clear_all should also work
    learner_->analyze_failure(make_failure("http_fetch"));
    learner_->record_failure(make_failure("http_fetch"));
    learner_->clear_all();
    EXPECT_EQ(learner_->rule_count(), 0u);
    EXPECT_EQ(learner_->failure_count(), 0u);
}

// ─────────────────────────────────────────────────────────────
// 11. Integration — full AgentOS with a failing tool
// ─────────────────────────────────────────────────────────────

TEST_F(ToolLearnerTest, Integration) {
    // Build AgentOS with mock backend
    auto os = AgentOSBuilder()
        .mock()
        .threads(1)
        .tpm(100000)
        .build();

    // Register a tool that always fails
    os->register_tool(
        ToolSchema{
            .id = "always_fail",
            .description = "A tool that always fails",
            .params = {{.name = "input",
                        .type = ParamType::String,
                        .description = "input data",
                        .required = true}},
        },
        [](const ParsedArgs&) -> ToolResult {
            return ToolResult::fail("intentional failure for testing");
        });

    // Record a failure manually via the OS tool_learner
    auto& learner = os->tool_learner();
    ToolFailureRecord rec{
        .tool_id = "always_fail",
        .args_json = R"({"input":"test"})",
        .error = "intentional failure for testing",
        .timestamp = Clock::now(),
        .agent_id = 0,
    };
    learner.record_failure(rec);

    EXPECT_EQ(learner.failure_count(), 1u);
    auto failures = learner.get_failures("always_fail");
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].error, "intentional failure for testing");

    os->graceful_shutdown(std::chrono::milliseconds(500));
}
