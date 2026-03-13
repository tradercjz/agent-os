#pragma once
// ============================================================
// AgentOS :: ReflectiveReActAgent
// 在 ReAct 循环中加入“反思”步骤，提高规划质量
// ============================================================
#include <agentos/agent.hpp>
#include <iostream>

namespace agentos {

/**
 * @brief ReflectiveReActAgent
 * 
 * 相比基础的 ReActAgent，在 Act 之前增加了一个内部反思步骤：
 * 1. Think: 提出初始行动方案
 * 2. Reflect: 审视方案（是否存在逻辑错误、漏掉约束、错误的工具参数等）
 * 3. Act: 执行经过反思修订后的方案
 */
class ReflectiveReActAgent : public ReActAgent {
public:
    using ReActAgent::ReActAgent;

    Result<std::string> run(std::string user_input) override {
        // 先从记忆中检索相关上下文
        auto recall_result = recall(user_input, 3);
        if (recall_result && !recall_result->empty()) {
            std::string mem_ctx = "相关记忆：\n";
            for (auto &sr : *recall_result) {
                mem_ctx += "- " + sr.entry.content + "\n";
            }
            os_->ctx().append(id_, kernel::Message::system(mem_ctx));
        }

        for (int step = 0; step < MAX_STEPS; ++step) {
            // ── 1. Think ──
            auto resp = think(step == 0 ? user_input : "[继续]");
            if (!resp) return make_unexpected(resp.error());

            // ── 完成 ──
            if (!resp->wants_tool_call()) {
                (void)remember(fmt::format("Q: {} → A: {}", user_input, resp->content), 0.6f);
                LOG_INFO(fmt::format("[ReflectiveAgent] Task completed with final answer: {}", resp->content));
                return resp->content;
            }

            LOG_INFO(fmt::format("[ReflectiveAgent] Turn {}: Model proposed content: '{}', tool_calls: {}", 
                                 step, resp->content, resp->tool_calls.size()));

            // ── 2. Reflect ──
            if (resp->wants_tool_call()) {
                std::string tool_plan = "计划执行工具调用: ";
                for (const auto& tc : resp->tool_calls) {
                    tool_plan += fmt::format("{}({}); ", tc.name, tc.args_json);
                }

                LOG_INFO(fmt::format("[ReflectiveAgent] Reflecting on tool plan: {}", tool_plan));
                auto reflection = reflect(tool_plan);
                if (reflection) {
                    LOG_INFO(fmt::format("[ReflectiveAgent] Reflection caught issue: {}", *reflection));
                    
                    // 简单循环检测：检查是否已经在上下文中追加过相同的反思
                    const auto& history = os_->ctx().get_window(id_).messages();
                    bool already_reflected = false;
                    for (auto it = history.rbegin(); it != history.rend(); ++it) {
                        if (it->role == kernel::Role::System && it->content.find(*reflection) != std::string::npos) {
                            already_reflected = true;
                            break;
                        }
                    }

                    if (!already_reflected) {
                        os_->ctx().append(id_, kernel::Message::system(
                            "【自我反思驱动的修正指令】:\n" + *reflection + "\n请根据上述反思，重写你的响应并决定最终行动。"
                        ));
                        continue; 
                    } else {
                        LOG_WARN("[ReflectiveAgent] Already reflected on this issue, proceeding to act to avoid loop.");
                    }
                }
            }

            // ── 3. Act ──
            for (auto &tc : resp->tool_calls) {
                auto tool_result = act(tc);
                std::string obs;
                if (tool_result) {
                    obs = tool_result->success ? tool_result->output
                                               : "工具执行失败: " + tool_result->error;
                } else {
                    obs = "工具调用被拒绝: " + tool_result.error().message;
                }

                kernel::Message obs_msg;
                obs_msg.role = kernel::Role::Tool;
                obs_msg.content = obs;
                obs_msg.tool_call_id = tc.id;
                obs_msg.name = tc.name;
                os_->ctx().append(id_, obs_msg);
            }
        }

        return make_error(ErrorCode::Timeout, "ReflectiveReActAgent: exceeded max_steps");
    }

protected:
    std::optional<std::string> reflect(const std::string& plan) {
        // 创建一个辅助请求来进行反思
        kernel::LLMRequest req;
        req.agent_id = id_;
        req.priority = config_.priority;
        
        // 构建反思指令
        std::string prompt = fmt::format(
            "你是一个严谨的审查者。请审视以下 Agent 的执行计划，检查是否存在以下问题：\n"
            "1. 是否违反了用户的原始约束？\n"
            "2. 工具参数是否符合逻辑？\n"
            "3. 是否有更高效的方法？\n\n"
            "执行计划：\n\"{}\"\n\n"
            "如果计划很完善，请只回复 \"OK\"。如果发现问题，请详细列出并给出改进建议。",
            plan
        );
        
        req.messages = { kernel::Message::user(prompt) };
        
        auto resp = os_->kernel().infer(req);
        if (!resp) return std::nullopt; // 考虑降级，反思失败则认为计划 OK 或返回错误？这里简单处理为 OK
        
        // 如果回复不是 OK，则返回反思内容
        if (resp->content.find("OK") == std::string::npos || resp->content.size() > 10) {
            return resp->content;
        }
        
        return std::nullopt; // 代表计划 OK
    }
};

} // namespace agentos
