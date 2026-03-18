#pragma once
// ============================================================
// AgentOS :: PlanningAgent
// 基于 Plan-Execute-Replan 范式的规划型 Agent
// ============================================================
#include <agentos/agent.hpp>
#include <sstream>

namespace agentos {

// ─────────────────────────────────────────────────────────────
// § Data Structures
// ─────────────────────────────────────────────────────────────

enum class StepStatus { Pending, Running, Completed, Failed, Skipped };

struct PlanStep {
    std::string id;
    std::string description;
    std::string tool_hint;
    StepStatus status{StepStatus::Pending};
    std::string result;
    std::vector<PlanStep> substeps;
};

struct Plan {
    std::string goal;
    std::vector<PlanStep> steps;
    int revision{0};
    static constexpr int MAX_REVISIONS = 3;
    static constexpr int MAX_DEPTH = 3;
    static constexpr int MAX_STEPS = 10;
};

// ─────────────────────────────────────────────────────────────
// § PlanningAgent
// ─────────────────────────────────────────────────────────────

class PlanningAgent : public AgentBase<PlanningAgent> {
public:
    using AgentBase<PlanningAgent>::AgentBase;
    Result<std::string> run(std::string user_input) override;

protected:
    Result<Plan> generate_plan(const std::string& goal);
    Result<std::string> execute_step(PlanStep& step, int depth = 0);
    Result<Plan> replan(Plan& current, const PlanStep& failed_step, const std::string& error);
    Result<std::string> synthesize(const Plan& plan);

private:
    static Plan parse_plan_response(const std::string& llm_output, const std::string& goal);
    static constexpr int STEP_MAX_ITERATIONS = 3;
};

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::run()
// ─────────────────────────────────────────────────────────────

inline Result<std::string> PlanningAgent::run(std::string user_input) {
    if (!this->os_) return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
    if (!this->alive_->load(std::memory_order_acquire))
        return make_error(ErrorCode::InvalidArgument, "Agent has been destroyed");

    // 1. Recall past plans as context
    std::string recall_context;
    auto recall_result = this->recall(user_input, 3);
    if (recall_result && !recall_result->empty()) {
        std::string mem_ctx = "相关记忆：\n";
        for (auto& sr : *recall_result) {
            mem_ctx += "- " + sr.entry.content + "\n";
            if (sr.entry.content.find("Plan for") != std::string::npos) {
                recall_context += sr.entry.content + "\n";
            }
        }
        this->os_->ctx().append(this->id_, kernel::Message::system(mem_ctx));
    }

    // 2. Generate plan
    auto plan_result = generate_plan(user_input);
    if (!plan_result) return make_unexpected(plan_result.error());
    auto plan = std::move(*plan_result);

    LOG_INFO(fmt::format("[PlanningAgent] Generated plan for '{}' with {} steps",
                         user_input, plan.steps.size()));

    // 3. Execute each step; replan on failure
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        auto& step = plan.steps[i];
        if (step.status != StepStatus::Pending) continue;

        LOG_INFO(fmt::format("[PlanningAgent] Executing step {}/{}: {}",
                             i + 1, plan.steps.size(), step.description));

        auto step_result = execute_step(step);
        if (!step_result) {
            // Step failed — try replanning
            std::string err_msg = step_result.error().message;
            LOG_WARN(fmt::format("[PlanningAgent] Step '{}' failed: {}", step.id, err_msg));

            if (plan.revision < Plan::MAX_REVISIONS) {
                auto replan_result = replan(plan, step, err_msg);
                if (replan_result) {
                    plan = std::move(*replan_result);
                    LOG_INFO(fmt::format("[PlanningAgent] Replanned (revision {}), {} steps remaining",
                                         plan.revision, plan.steps.size()));
                    // Restart execution from the beginning of remaining pending steps
                    i = static_cast<size_t>(-1); // will be incremented to 0
                    continue;
                }
            }
            // Cannot replan — record and continue
            step.status = StepStatus::Failed;
        }
    }

    // 4. Synthesize results
    auto synth_result = synthesize(plan);
    std::string outcome = synth_result ? *synth_result : "综合失败";

    // 5. Remember the execution
    (void)this->remember(
        fmt::format("Plan for '{}': {} steps, outcome: {}",
                    user_input, plan.steps.size(), outcome),
        0.8f);

    if (synth_result) return *synth_result;
    return make_unexpected(synth_result.error());
}

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::generate_plan()
// ─────────────────────────────────────────────────────────────

inline Result<Plan> PlanningAgent::generate_plan(const std::string& goal) {
    kernel::LLMRequest req;
    req.agent_id = this->id_;
    req.priority = this->config_.priority;

    std::string system_prompt =
        "你是一个任务规划专家。请将以下任务分解为具体的执行步骤。\n"
        "格式要求：每行一个步骤，格式为: 数字. 步骤描述 | tool: 工具名(可选)\n"
        "最多10个步骤。";

    req.messages.push_back(kernel::Message::system(system_prompt));

    // Include past plan context if available
    auto recall_result = this->recall(goal, 3);
    if (recall_result && !recall_result->empty()) {
        std::string history;
        for (auto& sr : *recall_result) {
            if (sr.entry.content.find("Plan for") != std::string::npos) {
                history += sr.entry.content + "\n";
            }
        }
        if (!history.empty()) {
            req.messages.push_back(kernel::Message::system(
                "参考历史方案:\n" + history));
        }
    }

    req.messages.push_back(kernel::Message::user(goal));

    auto resp = this->os_->kernel().infer(req);
    if (!resp) return make_unexpected(resp.error());

    return parse_plan_response(resp->content, goal);
}

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::execute_step()
// ─────────────────────────────────────────────────────────────

inline Result<std::string> PlanningAgent::execute_step(PlanStep& step, int depth) {
    step.status = StepStatus::Running;

    std::string prompt = step.description;
    if (!step.tool_hint.empty()) {
        prompt += fmt::format(" (建议使用工具: {})", step.tool_hint);
    }

    for (int iter = 0; iter < STEP_MAX_ITERATIONS; ++iter) {
        auto resp = this->think(iter == 0 ? prompt : "[继续执行当前步骤]");
        if (!resp) {
            step.status = StepStatus::Failed;
            step.result = resp.error().message;
            (void)this->remember("Step failed: " + step.description + " Error: " + resp.error().message, 0.7f);
            return make_unexpected(resp.error());
        }

        // LLM wants tool call → execute tools
        if (resp->wants_tool_call()) {
            for (auto& tc : resp->tool_calls) {
                auto tool_result = this->act(tc);
                std::string obs = tool_result
                    ? (tool_result->success ? tool_result->output : "工具执行失败: " + tool_result->error)
                    : "工具调用被拒绝: " + tool_result.error().message;

                kernel::Message obs_msg;
                obs_msg.role = kernel::Role::Tool;
                obs_msg.content = std::move(obs);
                obs_msg.tool_call_id = tc.id;
                obs_msg.name = tc.name;
                this->os_->ctx().append(this->id_, std::move(obs_msg));
            }
            continue; // Next iteration to get LLM's interpretation of tool results
        }

        // LLM gave text response — check if decomposition is needed
        if (resp->content.size() > 500 && depth < Plan::MAX_DEPTH) {
            // Complex response suggests step needs decomposition
            kernel::LLMRequest decompose_req;
            decompose_req.agent_id = this->id_;
            decompose_req.priority = this->config_.priority;
            decompose_req.messages = {
                kernel::Message::system(
                    "请判断以下步骤是否需要进一步分解。如果需要，请按格式输出子步骤：\n"
                    "数字. 子步骤描述 | tool: 工具名(可选)\n"
                    "如果不需要分解，请只回复 \"OK\"。"),
                kernel::Message::user(step.description + "\n当前结果: " + resp->content)
            };

            auto decompose_resp = this->os_->kernel().infer(decompose_req);
            if (decompose_resp && decompose_resp->content.find("OK") == std::string::npos
                && decompose_resp->content.size() > 10) {
                // Parse substeps and recurse
                auto sub_plan = parse_plan_response(decompose_resp->content, step.description);
                step.substeps = std::move(sub_plan.steps);

                std::string combined_result;
                bool any_failed = false;
                for (auto& substep : step.substeps) {
                    auto sub_result = execute_step(substep, depth + 1);
                    if (sub_result) {
                        combined_result += *sub_result + "\n";
                    } else {
                        any_failed = true;
                        combined_result += fmt::format("[子步骤失败: {}]\n", substep.description);
                    }
                }

                if (any_failed) {
                    step.status = StepStatus::Failed;
                    step.result = combined_result;
                    (void)this->remember("Step failed: " + step.description + " Error: substeps failed", 0.7f);
                    return make_error(ErrorCode::ToolExecutionFailed,
                                      "Substeps partially failed: " + combined_result);
                }

                step.status = StepStatus::Completed;
                step.result = combined_result;
                return combined_result;
            }
        }

        // Simple text response — step is complete
        step.status = StepStatus::Completed;
        step.result = resp->content;
        return resp->content;
    }

    // Exhausted iterations
    step.status = StepStatus::Failed;
    std::string err = "Step exceeded max iterations";
    step.result = err;
    (void)this->remember("Step failed: " + step.description + " Error: " + err, 0.7f);
    return make_error(ErrorCode::Timeout, err);
}

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::replan()
// ─────────────────────────────────────────────────────────────

inline Result<Plan> PlanningAgent::replan(Plan& current, const PlanStep& failed_step, const std::string& error) {
    kernel::LLMRequest req;
    req.agent_id = this->id_;
    req.priority = this->config_.priority;

    // Build context: original goal, completed steps, failed step
    std::string context = fmt::format("原始目标: {}\n\n已完成的步骤:\n", current.goal);
    for (const auto& step : current.steps) {
        if (step.status == StepStatus::Completed) {
            context += fmt::format("- [完成] {}: {}\n", step.id, step.description);
            if (!step.result.empty()) {
                context += fmt::format("  结果: {}\n", step.result.substr(0, 200));
            }
        }
    }
    context += fmt::format("\n失败的步骤: {} - {}\n失败原因: {}\n",
                           failed_step.id, failed_step.description, error);

    req.messages = {
        kernel::Message::system(
            "步骤失败了，请根据已完成的步骤和失败原因，重新规划剩余步骤。\n"
            "格式要求：每行一个步骤，格式为: 数字. 步骤描述 | tool: 工具名(可选)\n"
            "最多10个步骤。"),
        kernel::Message::user(context)
    };

    auto resp = this->os_->kernel().infer(req);
    if (!resp) return make_unexpected(resp.error());

    auto new_plan = parse_plan_response(resp->content, current.goal);
    new_plan.revision = current.revision + 1;

    // Preserve completed steps from current plan
    std::vector<PlanStep> merged;
    for (auto& step : current.steps) {
        if (step.status == StepStatus::Completed) {
            merged.push_back(std::move(step));
        }
    }
    for (auto& step : new_plan.steps) {
        merged.push_back(std::move(step));
    }
    // Cap total steps
    if (merged.size() > static_cast<size_t>(Plan::MAX_STEPS)) {
        merged.resize(Plan::MAX_STEPS);
    }
    new_plan.steps = std::move(merged);

    LOG_INFO(fmt::format("[PlanningAgent] Replan revision {} with {} steps",
                         new_plan.revision, new_plan.steps.size()));

    return new_plan;
}

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::synthesize()
// ─────────────────────────────────────────────────────────────

inline Result<std::string> PlanningAgent::synthesize(const Plan& plan) {
    kernel::LLMRequest req;
    req.agent_id = this->id_;
    req.priority = this->config_.priority;

    std::string results_summary = fmt::format("目标: {}\n\n执行结果:\n", plan.goal);
    for (const auto& step : plan.steps) {
        std::string status_str;
        switch (step.status) {
            case StepStatus::Completed: status_str = "完成"; break;
            case StepStatus::Failed:    status_str = "失败"; break;
            case StepStatus::Skipped:   status_str = "跳过"; break;
            case StepStatus::Running:   status_str = "进行中"; break;
            case StepStatus::Pending:   status_str = "待执行"; break;
        }
        results_summary += fmt::format("- [{}] {}: {}\n",
                                       status_str, step.description,
                                       step.result.substr(0, 300));
    }

    req.messages = {
        kernel::Message::system("基于以下步骤的执行结果，请给出最终总结回答。"),
        kernel::Message::user(results_summary)
    };

    auto resp = this->os_->kernel().infer(req);
    if (!resp) return make_unexpected(resp.error());
    return resp->content;
}

// ─────────────────────────────────────────────────────────────
// § PlanningAgent::parse_plan_response()
// ─────────────────────────────────────────────────────────────

inline Plan PlanningAgent::parse_plan_response(const std::string& llm_output, const std::string& goal) {
    Plan plan;
    plan.goal = goal;

    std::istringstream stream(llm_output);
    std::string line;
    int step_num = 0;

    while (std::getline(stream, line) && step_num < Plan::MAX_STEPS) {
        // Trim leading/trailing whitespace
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Look for pattern: N. description or N. description | tool: toolname
        auto dot_pos = line.find('.');
        if (dot_pos == std::string::npos || dot_pos == 0) continue;

        // Verify that everything before the dot is a number
        bool is_numbered = true;
        for (size_t i = 0; i < dot_pos; ++i) {
            if (line[i] < '0' || line[i] > '9') {
                is_numbered = false;
                break;
            }
        }
        if (!is_numbered) continue;

        std::string rest = line.substr(dot_pos + 1);
        // Trim leading space after dot
        auto rest_start = rest.find_first_not_of(" \t");
        if (rest_start != std::string::npos) {
            rest = rest.substr(rest_start);
        }
        if (rest.empty()) continue;

        PlanStep step;
        step_num++;
        step.id = fmt::format("step_{}", step_num);

        // Check for "| tool: toolname" suffix
        auto pipe_pos = rest.find('|');
        if (pipe_pos != std::string::npos) {
            std::string after_pipe = rest.substr(pipe_pos + 1);
            // Trim
            auto ap_start = after_pipe.find_first_not_of(" \t");
            if (ap_start != std::string::npos) {
                after_pipe = after_pipe.substr(ap_start);
            }
            // Check for "tool:" prefix
            if (after_pipe.starts_with("tool:")) {
                std::string tool_name = after_pipe.substr(5);
                auto tn_start = tool_name.find_first_not_of(" \t");
                if (tn_start != std::string::npos) {
                    tool_name = tool_name.substr(tn_start);
                }
                // Trim trailing whitespace
                auto tn_end = tool_name.find_last_not_of(" \t\r\n");
                if (tn_end != std::string::npos) {
                    tool_name = tool_name.substr(0, tn_end + 1);
                }
                step.tool_hint = std::move(tool_name);
            }
            // Description is before the pipe
            std::string desc = rest.substr(0, pipe_pos);
            auto desc_end = desc.find_last_not_of(" \t");
            if (desc_end != std::string::npos) {
                desc = desc.substr(0, desc_end + 1);
            }
            step.description = std::move(desc);
        } else {
            // Trim trailing whitespace from description
            auto desc_end = rest.find_last_not_of(" \t\r\n");
            if (desc_end != std::string::npos) {
                rest = rest.substr(0, desc_end + 1);
            }
            step.description = std::move(rest);
        }

        plan.steps.push_back(std::move(step));
    }

    // Fallback: if no steps parsed, create a single step with the entire output
    if (plan.steps.empty()) {
        PlanStep fallback;
        fallback.id = "step_1";
        // Trim the output for the description
        std::string desc = llm_output;
        auto desc_end = desc.find_last_not_of(" \t\r\n");
        if (desc_end != std::string::npos) {
            desc = desc.substr(0, desc_end + 1);
        }
        fallback.description = std::move(desc);
        plan.steps.push_back(std::move(fallback));
    }

    return plan;
}

} // namespace agentos
