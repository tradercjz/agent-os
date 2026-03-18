// ============================================================
// AgentOS :: Tool Learner Implementation
// 工具调用失败分析、自动修正规则生成
// ============================================================
#include <agentos/tools/tool_learner.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <nlohmann/json.hpp>

namespace agentos::tools {

// ─────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────

ToolLearner::ToolLearner(kernel::LLMKernel& kernel, ToolLearnerConfig cfg)
    : kernel_(kernel), config_(std::move(cfg)) {}

// ─────────────────────────────────────────────────────────────
// Record a tool failure
// ─────────────────────────────────────────────────────────────

void ToolLearner::record_failure(const ToolFailureRecord& record) {
    std::lock_guard lk(mu_);
    failures_.push_back(record);
    evict_failures_if_needed();
}

// ─────────────────────────────────────────────────────────────
// Analyze failure via LLM and generate correction rule
// ─────────────────────────────────────────────────────────────

void ToolLearner::analyze_failure(const ToolFailureRecord& record) {
    if (!config_.enabled) return;

    std::string system_prompt =
        "你是一个工具调用专家。以下工具调用失败了：\n"
        "工具: " + record.tool_id + "\n"
        "参数: " + record.args_json + "\n"
        "错误: " + record.error + "\n"
        "\n"
        "请分析失败原因并给出修正建议。严格按以下格式回复（只回复一条规则）：\n"
        "TYPE: param_fix 或 prompt_hint\n"
        "PATTERN: 错误匹配模式描述\n"
        "PARAM: 参数名（TYPE=param_fix时填写，否则留空）\n"
        "FIX: 修正值或提醒文本";

    auto req = kernel::LLMRequest::builder()
        .system(system_prompt)
        .user(fmt::format("请分析工具 {} 的调用失败并给出修正规则。", record.tool_id))
        .temperature(0.3f)
        .max_tokens(512)
        .build();

    auto result = kernel_.infer(req);
    if (result) {
        parse_and_store_rule(record.tool_id, result->content);
    } else {
        LOG_WARN(fmt::format("ToolLearner: LLM analysis failed for tool '{}': {}",
                             record.tool_id, result.error().message));
    }
}

// ─────────────────────────────────────────────────────────────
// Parse LLM output and store correction rule
// ─────────────────────────────────────────────────────────────

void ToolLearner::parse_and_store_rule(const std::string& tool_id, const std::string& llm_output) {
    ToolCorrectionRule rule;
    rule.tool_id = tool_id;

    // Parse line by line looking for TYPE:, PATTERN:, PARAM:, FIX: prefixes
    std::istringstream stream(llm_output);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim leading whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.starts_with("TYPE:")) {
            auto val = line.substr(5);
            auto vstart = val.find_first_not_of(" \t");
            if (vstart != std::string::npos) {
                rule.fix_type = val.substr(vstart);
                // Normalize: trim trailing whitespace
                auto vend = rule.fix_type.find_last_not_of(" \t\r\n");
                if (vend != std::string::npos) rule.fix_type = rule.fix_type.substr(0, vend + 1);
            }
        } else if (line.starts_with("PATTERN:")) {
            auto val = line.substr(8);
            auto vstart = val.find_first_not_of(" \t");
            if (vstart != std::string::npos) {
                rule.pattern = val.substr(vstart);
                auto vend = rule.pattern.find_last_not_of(" \t\r\n");
                if (vend != std::string::npos) rule.pattern = rule.pattern.substr(0, vend + 1);
            }
        } else if (line.starts_with("PARAM:")) {
            auto val = line.substr(6);
            auto vstart = val.find_first_not_of(" \t");
            if (vstart != std::string::npos) {
                rule.param_name = val.substr(vstart);
                auto vend = rule.param_name.find_last_not_of(" \t\r\n");
                if (vend != std::string::npos) rule.param_name = rule.param_name.substr(0, vend + 1);
            }
        } else if (line.starts_with("FIX:")) {
            auto val = line.substr(4);
            auto vstart = val.find_first_not_of(" \t");
            if (vstart != std::string::npos) {
                std::string fix_val = val.substr(vstart);
                auto vend = fix_val.find_last_not_of(" \t\r\n");
                if (vend != std::string::npos) fix_val = fix_val.substr(0, vend + 1);

                if (rule.fix_type == "param_fix") {
                    rule.fix_replacement = fix_val;
                    // Use param_name as fix_regex if not otherwise set
                    if (rule.fix_regex.empty() && !rule.param_name.empty()) {
                        rule.fix_regex = rule.param_name;
                    }
                } else {
                    rule.prompt_hint = fix_val;
                }
            }
        }
    }

    // Validate: must have fix_type and at least some content
    if (rule.fix_type != "param_fix" && rule.fix_type != "prompt_hint") {
        LOG_WARN(fmt::format("ToolLearner: invalid fix_type '{}' from LLM output, discarding rule",
                             rule.fix_type));
        return;
    }

    std::lock_guard lk(mu_);
    auto& tool_rules = rules_[tool_id];
    if (tool_rules.size() >= config_.max_rules_per_tool) {
        // Evict the oldest rule (FIFO)
        tool_rules.erase(tool_rules.begin());
    }
    tool_rules.push_back(std::move(rule));
    LOG_INFO(fmt::format("ToolLearner: added {} rule for tool '{}'",
                         tool_rules.back().fix_type, tool_id));
}

// ─────────────────────────────────────────────────────────────
// Apply parameter fixes before tool execution
// ─────────────────────────────────────────────────────────────

std::string ToolLearner::apply_param_fixes(const std::string& tool_id, const std::string& args_json) {
    std::lock_guard lk(mu_);

    auto it = rules_.find(tool_id);
    if (it == rules_.end()) return args_json;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(args_json);
    } catch (const std::exception&) {
        return args_json; // Cannot parse, return unchanged
    }

    if (!j.is_object()) return args_json;

    bool modified = false;
    for (auto& rule : it->second) {
        if (rule.fix_type != "param_fix") continue;
        if (rule.param_name.empty()) continue;

        auto param_it = j.find(rule.param_name);
        if (param_it == j.end()) continue;

        // Get the current value as string for matching
        std::string current_val;
        if (param_it->is_string()) {
            current_val = param_it->get<std::string>();
        } else {
            current_val = param_it->dump();
        }

        // Use simple string find for safety (not actual regex)
        if (!rule.fix_regex.empty() && current_val.find(rule.fix_regex) == std::string::npos) {
            continue; // Pattern does not match
        }

        // Apply fix
        j[rule.param_name] = rule.fix_replacement;
        rule.applied_count++;
        modified = true;
    }

    last_applied_tool_ = tool_id;
    return modified ? j.dump() : args_json;
}

// ─────────────────────────────────────────────────────────────
// Get prompt hints for a tool
// ─────────────────────────────────────────────────────────────

std::string ToolLearner::get_prompt_hints(const std::string& tool_id) {
    std::lock_guard lk(mu_);

    auto it = rules_.find(tool_id);
    if (it == rules_.end()) return {};

    std::string result;
    for (const auto& rule : it->second) {
        if (rule.fix_type != "prompt_hint") continue;
        if (rule.prompt_hint.empty()) continue;

        if (result.empty()) {
            result = fmt::format("[工具使用提醒 - {}]\n", tool_id);
        }
        result += fmt::format("- {}\n", rule.prompt_hint);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────
// Record fix outcome
// ─────────────────────────────────────────────────────────────

void ToolLearner::record_fix_outcome(const std::string& tool_id, bool success) {
    std::lock_guard lk(mu_);

    auto it = rules_.find(tool_id);
    if (it == rules_.end()) return;

    // Increment success_after_apply for rules that were recently applied
    for (auto& rule : it->second) {
        if (rule.applied_count > 0 && success) {
            rule.success_after_apply++;
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Query methods
// ─────────────────────────────────────────────────────────────

std::vector<ToolFailureRecord> ToolLearner::get_failures(const std::string& tool_id) const {
    std::lock_guard lk(mu_);
    std::vector<ToolFailureRecord> result;
    for (const auto& f : failures_) {
        if (f.tool_id == tool_id) {
            result.push_back(f);
        }
    }
    return result;
}

std::vector<ToolCorrectionRule> ToolLearner::get_rules(const std::string& tool_id) const {
    std::lock_guard lk(mu_);
    auto it = rules_.find(tool_id);
    if (it != rules_.end()) return it->second;
    return {};
}

size_t ToolLearner::failure_count() const {
    std::lock_guard lk(mu_);
    return failures_.size();
}

size_t ToolLearner::rule_count() const {
    std::lock_guard lk(mu_);
    size_t total = 0;
    for (const auto& [id, rules] : rules_) {
        total += rules.size();
    }
    return total;
}

// ─────────────────────────────────────────────────────────────
// Management
// ─────────────────────────────────────────────────────────────

void ToolLearner::clear_rules(const std::string& tool_id) {
    std::lock_guard lk(mu_);
    rules_.erase(tool_id);
}

void ToolLearner::clear_all() {
    std::lock_guard lk(mu_);
    failures_.clear();
    rules_.clear();
    last_applied_tool_.clear();
}

// ─────────────────────────────────────────────────────────────
// Eviction
// ─────────────────────────────────────────────────────────────

void ToolLearner::evict_failures_if_needed() {
    // Caller must hold mu_
    if (failures_.size() > config_.max_failures_stored) {
        size_t half = failures_.size() / 2;
        failures_.erase(failures_.begin(), failures_.begin() + static_cast<ptrdiff_t>(half));
        LOG_INFO(fmt::format("ToolLearner: evicted {} oldest failure records", half));
    }
}

} // namespace agentos::tools
