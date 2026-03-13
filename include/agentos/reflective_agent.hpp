#pragma once
// ============================================================
// AgentOS :: ReflectiveReActAgent
// 在 ReAct 循环中加入“反思”步骤，提高规划质量
// ============================================================
#include <agentos/agent.hpp>

namespace agentos {

/**
 * @brief ReflectiveReActAgent
 * 
 * 相比基础的 ReActAgent，在 Act 之前增加了一个内部反思步骤：
 * 1. Think: 提出初始行动方案
 * 2. Reflect: 审视方案（是否存在逻辑错误、漏掉约束、错误的工具参数等）
 * 3. Act: 执行经过反思修订后的方案
 */
class ReflectiveReActAgent : public AgentBase<ReflectiveReActAgent> {
public:
    using AgentBase<ReflectiveReActAgent>::AgentBase;

    Result<std::string> run(std::string user_input) override {
        // 先从记忆中检索相关上下文
        auto recall_result = this->recall(user_input, 3);
        if (recall_result && !recall_result->empty()) {
            std::string mem_ctx = "相关记忆：\n";
            for (auto &sr : *recall_result) {
                mem_ctx += "- " + sr.entry.content + "\n";
            }
            this->os_->ctx().append(this->id_, kernel::Message::system(mem_ctx));
        }

        static constexpr int MAX_STEPS = 10;
        for (int step = 0; step < MAX_STEPS; ++step) {
            // ── 1. Think ──
            auto resp = this->think(step == 0 ? user_input : "[继续]");
            if (!resp) return make_unexpected(resp.error());

            // ── 完成 ──
            if (!resp->wants_tool_call()) {
                (void)this->remember(fmt::format("Q: {} → A: {}", user_input, resp->content), 0.6f);
                LOG_INFO(fmt::format("[ReflectiveAgent] Task completed: {}", resp->content));
                return resp->content;
            }

            LOG_INFO(fmt::format("[ReflectiveAgent] Turn {}: Model proposed action.", step));

            // ── 2. Reflect ──
            if (resp->wants_tool_call()) {
                std::string tool_plan = "计划执行工具调用: ";
                for (const auto& tc : resp->tool_calls) {
                    tool_plan += fmt::format("{}({}); ", tc.name, tc.args_json);
                }

                auto reflection = reflect(tool_plan);
                if (reflection) {
                    LOG_INFO(fmt::format("[ReflectiveAgent] Reflection caught issue."));
                    
                    const auto& history = this->os_->ctx().get_window(this->id_).messages();
                    bool already_reflected = std::ranges::any_of(history | std::views::reverse, 
                        [&](const auto& msg) {
                            return msg.role == kernel::Role::System && 
                                   msg.content.find(*reflection) != std::string::npos;
                        });

                    if (!already_reflected) {
                        this->os_->ctx().append(this->id_, kernel::Message::system(
                            fmt::format("【自我反思驱动的修正指令】:\n{}\n请根据上述反思，重写你的响应并决定最终行动。", *reflection)
                        ));
                        continue; 
                    }
                }
            }

            // ── 3. Act ──
            for (auto &tc : resp->tool_calls) {
                (void)this->act(tc).transform([&](tools::ToolResult res) {
                    std::string obs = res.success ? std::move(res.output)
                                                   : "工具执行失败: " + std::move(res.error);
                    
                    kernel::Message obs_msg;
                    obs_msg.role = kernel::Role::Tool;
                    obs_msg.content = std::move(obs);
                    obs_msg.tool_call_id = tc.id;
                    obs_msg.name = tc.name;
                    this->os_->ctx().append(this->id_, std::move(obs_msg));
                }).or_else([&, &tc = tc](Error err) {
                    kernel::Message obs_msg;
                    obs_msg.role = kernel::Role::Tool;
                    obs_msg.content = "工具调用被拒绝: " + err.message;
                    obs_msg.tool_call_id = tc.id;
                    obs_msg.name = tc.name;
                    this->os_->ctx().append(this->id_, std::move(obs_msg));
                    return Result<void>(make_unexpected(err));
                });
            }
        }

        return make_error(ErrorCode::Timeout, "ReflectiveReActAgent: exceeded max_steps");
    }

protected:
    std::optional<std::string> reflect(const std::string& plan) {
        kernel::LLMRequest req;
        req.agent_id = this->id_;
        req.priority = this->config_.priority;
        
        req.messages = { kernel::Message::user(fmt::format(
            "你是一个严谨的审查者。请审视以下 Agent 的执行计划，检查是否存在以下问题：\n"
            "1. 是否违反了用户的原始约束？\n"
            "2. 工具参数是否符合逻辑？\n"
            "3. 是否有更高效的方法？\n\n"
            "执行计划：\n\"{}\"\n\n"
            "如果计划很完善，请只回复 \"OK\"。如果发现问题，请详细列出并给出改进建议。",
            plan
        )) };
        
        return this->os_->kernel().infer(req).transform([](kernel::LLMResponse resp) -> std::optional<std::string> {
            if (resp.content.find("OK") == std::string::npos || resp.content.size() > 10) {
                return std::move(resp.content);
            }
            return std::nullopt;
        }).value_or(std::nullopt);
    }
};

} // namespace agentos
