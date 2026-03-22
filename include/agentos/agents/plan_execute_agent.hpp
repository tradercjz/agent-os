#pragma once
// ============================================================
// AgentOS :: PlanExecuteAgent
// Creates a plan (list of steps), then executes each step
// ============================================================
#include <agentos/agent.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

// ─────────────────────────────────────────────────────────────
// § Data Structures
// ─────────────────────────────────────────────────────────────

struct PlanExecStep {
    int step_number{0};
    std::string description;
    std::string status{"pending"};  // "pending", "running", "done", "failed"
    std::string result;
};

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent
// ─────────────────────────────────────────────────────────────

class PlanExecuteAgent : public AgentBase<PlanExecuteAgent> {
public:
    using AgentBase<PlanExecuteAgent>::AgentBase;

    Result<std::string> run(std::string user_input) override;

    // Access the current plan (for inspection/testing)
    const std::vector<PlanExecStep>& current_plan() const { return plan_; }

    void set_max_steps(int max) { max_steps_ = max; }
    void set_max_replan_attempts(int max) { max_replans_ = max; }

private:
    std::vector<PlanExecStep> plan_;
    int max_steps_{10};
    int max_replans_{2};

    // Phase 1: Generate a plan from the input
    Result<std::vector<PlanExecStep>> generate_plan(const std::string& input);

    // Phase 2: Execute a single step (may use tools)
    Result<std::string> execute_step(PlanExecStep& step);

    // Phase 3: Synthesize final answer from step results
    Result<std::string> synthesize(const std::string& input);

    // Parse "Step N: description" lines from LLM output
    static std::vector<PlanExecStep> parse_steps(const std::string& content,
                                                  int max_steps);
};

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent::parse_steps()
// ─────────────────────────────────────────────────────────────

inline std::vector<PlanExecStep>
PlanExecuteAgent::parse_steps(const std::string& content, int max_steps) {
    std::vector<PlanExecStep> steps;
    std::istringstream iss(content);
    std::string line;

    while (std::getline(iss, line) &&
           static_cast<int>(steps.size()) < max_steps) {
        // Match "Step N:" pattern (case-insensitive first letter)
        bool matches = false;
        if (line.find("Step ") == 0 || line.find("step ") == 0) {
            matches = true;
        }
        if (!matches) continue;

        PlanExecStep step;
        step.step_number = static_cast<int>(steps.size()) + 1;
        step.status = "pending";

        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string desc = line.substr(colon + 1);
            auto start = desc.find_first_not_of(" \t");
            if (start != std::string::npos) {
                desc = desc.substr(start);
            }
            // Trim trailing whitespace
            auto end = desc.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) {
                desc = desc.substr(0, end + 1);
            }
            step.description = std::move(desc);
        } else {
            step.description = line;
        }

        steps.push_back(std::move(step));
    }

    // Fallback: if no steps parsed, treat entire response as a single step
    if (steps.empty()) {
        PlanExecStep fallback;
        fallback.step_number = 1;
        fallback.status = "pending";
        // Trim for description
        std::string desc = content;
        auto end = desc.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            desc = desc.substr(0, end + 1);
        }
        fallback.description = std::move(desc);
        steps.push_back(std::move(fallback));
    }

    return steps;
}

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent::run()
// ─────────────────────────────────────────────────────────────

inline Result<std::string> PlanExecuteAgent::run(std::string user_input) {
    if (!this->os_)
        return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
    if (!this->alive_->load(std::memory_order_acquire))
        return make_error(ErrorCode::InvalidArgument, "Agent has been destroyed");

    // Phase 1: Plan
    auto plan_result = generate_plan(user_input);
    if (!plan_result.has_value()) {
        return make_unexpected(plan_result.error());
    }
    plan_ = std::move(*plan_result);

    LOG_INFO(fmt::format("[PlanExecuteAgent] Generated plan with {} steps for '{}'",
                         plan_.size(), user_input));

    // Phase 2: Execute each step
    for (auto& step : plan_) {
        step.status = "running";
        auto exec_result = execute_step(step);
        if (exec_result.has_value()) {
            step.result = *exec_result;
            step.status = "done";
        } else {
            step.result = exec_result.error().message;
            step.status = "failed";
        }
    }

    // Phase 3: Synthesize
    auto synth = synthesize(user_input);
    if (!synth.has_value()) {
        return make_unexpected(synth.error());
    }

    // Remember the execution
    (void)this->remember(
        fmt::format("PlanExecute for '{}': {} steps", user_input, plan_.size()),
        0.7f);

    return *synth;
}

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent::generate_plan()
// ─────────────────────────────────────────────────────────────

inline Result<std::vector<PlanExecStep>>
PlanExecuteAgent::generate_plan(const std::string& input) {
    auto& kernel = os_->kernel();

    kernel::LLMRequest req;
    req.agent_id = id_;
    req.priority = config_.priority;

    std::string plan_prompt;
    if (!config_.role_prompt.empty()) {
        plan_prompt = config_.role_prompt + "\n\n";
    }
    plan_prompt += "Create a step-by-step plan to accomplish the following task. "
                   "Output each step on a new line as: 'Step N: description'. "
                   "Keep the plan to at most " +
                   std::to_string(max_steps_) + " steps.";

    req.messages.push_back(kernel::Message::system(plan_prompt));
    req.messages.push_back(kernel::Message::user(input));

    auto result = kernel.infer(req);
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }

    return parse_steps(result->content, max_steps_);
}

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent::execute_step()
// ─────────────────────────────────────────────────────────────

inline Result<std::string>
PlanExecuteAgent::execute_step(PlanExecStep& step) {
    auto& kernel = os_->kernel();

    kernel::LLMRequest req;
    req.agent_id = id_;
    req.priority = config_.priority;

    req.messages.push_back(kernel::Message::system(
        "Execute the following step and provide the result. Be concise."));
    req.messages.push_back(kernel::Message::user(
        "Step " + std::to_string(step.step_number) + ": " + step.description));

    // Include tools if any are available
    std::string tj = os_->tools().tools_json(config_.allowed_tools);
    if (!tj.empty() && tj != "[]") {
        req.tools_json = std::move(tj);
    }

    auto result = kernel.infer(req);
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }

    // Handle tool calls if any
    if (result->wants_tool_call()) {
        for (const auto& tc : result->tool_calls) {
            auto tool_result = this->act(tc);
            if (tool_result.has_value() && tool_result->success) {
                return tool_result->output;
            }
        }
    }

    return result->content;
}

// ─────────────────────────────────────────────────────────────
// § PlanExecuteAgent::synthesize()
// ─────────────────────────────────────────────────────────────

inline Result<std::string>
PlanExecuteAgent::synthesize(const std::string& input) {
    auto& kernel = os_->kernel();

    kernel::LLMRequest req;
    req.agent_id = id_;
    req.priority = config_.priority;

    std::string summary = "Task: " + input + "\n\nResults:\n";
    for (const auto& step : plan_) {
        summary += "Step " + std::to_string(step.step_number) +
                   " (" + step.status + "): " + step.description +
                   "\n  Result: " + step.result + "\n";
    }

    req.messages.push_back(kernel::Message::system(
        "Synthesize a final answer from the completed plan steps. "
        "Be concise and direct."));
    req.messages.push_back(kernel::Message::user(summary));

    auto result = kernel.infer(req);
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }
    return result->content;
}

} // namespace agentos
