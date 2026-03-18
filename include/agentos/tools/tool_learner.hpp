#pragma once
// ============================================================
// AgentOS :: Module 5b — Tool Learner
// 工具调用失败分析、自动修正规则生成
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/core/logger.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::kernel { class LLMKernel; } // forward declare

namespace agentos::tools {

struct ToolFailureRecord {
    std::string tool_id;
    std::string args_json;
    std::string error;
    TimePoint timestamp;
    AgentId agent_id;
};

struct ToolCorrectionRule {
    std::string tool_id;
    std::string pattern;         // Human-readable error pattern description
    std::string fix_type;        // "param_fix" or "prompt_hint"
    std::string param_name;      // For param_fix: which parameter to fix
    std::string fix_regex;       // For param_fix: regex to match in value
    std::string fix_replacement; // For param_fix: replacement value
    std::string prompt_hint;     // For prompt_hint: text to inject
    uint32_t applied_count{0};
    uint32_t success_after_apply{0};
};

struct ToolLearnerConfig {
    bool enabled{true};
    size_t max_failures_stored{500};
    size_t max_rules_per_tool{10};
};

class ToolLearner : private NonCopyable {
public:
    explicit ToolLearner(kernel::LLMKernel& kernel, ToolLearnerConfig cfg = {});

    // Record a tool failure
    void record_failure(const ToolFailureRecord& record);

    // Analyze failure using LLM and generate correction rule
    void analyze_failure(const ToolFailureRecord& record);

    // Apply parameter fixes before tool execution (returns corrected args_json)
    std::string apply_param_fixes(const std::string& tool_id, const std::string& args_json);

    // Get prompt hints for a tool (inject as system message before act)
    std::string get_prompt_hints(const std::string& tool_id);

    // Record that a fix was applied and whether it succeeded
    void record_fix_outcome(const std::string& tool_id, bool success);

    // Query
    std::vector<ToolFailureRecord> get_failures(const std::string& tool_id) const;
    std::vector<ToolCorrectionRule> get_rules(const std::string& tool_id) const;
    size_t failure_count() const;
    size_t rule_count() const;

    // Management
    void clear_rules(const std::string& tool_id);
    void clear_all();

    bool enabled() const noexcept { return config_.enabled; }
    const ToolLearnerConfig& config() const noexcept { return config_; }

private:
    void parse_and_store_rule(const std::string& tool_id, const std::string& llm_output);
    void evict_failures_if_needed();

    kernel::LLMKernel& kernel_;
    ToolLearnerConfig config_;
    mutable std::mutex mu_;
    std::vector<ToolFailureRecord> failures_;
    std::unordered_map<std::string, std::vector<ToolCorrectionRule>> rules_; // tool_id -> rules
    std::string last_applied_tool_; // Track for record_fix_outcome
};

} // namespace agentos::tools
