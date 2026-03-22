#pragma once
// ============================================================
// AgentOS :: CoTAgent (Chain-of-Thought)
// Generates explicit step-by-step reasoning before answering
// ============================================================
#include <agentos/agent.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace agentos {

// ─────────────────────────────────────────────────────────────
// § CoTAgent — Chain-of-Thought Reasoning
// ─────────────────────────────────────────────────────────────

class CoTAgent : public AgentBase<CoTAgent> {
public:
    using AgentBase<CoTAgent>::AgentBase;

    Result<std::string> run(std::string user_input) override;

    // Configuration
    void set_cot_prompt(const std::string& prompt) { cot_prompt_ = prompt; }
    void set_max_reasoning_tokens(TokenCount tokens) { max_reasoning_tokens_ = tokens; }

    // Parse the LLM response into {reasoning, answer} parts.
    // Static for testability without creating an agent instance.
    static std::pair<std::string, std::string> parse_response(const std::string& content);

private:
    std::string cot_prompt_ =
        "Think step by step. After your reasoning, provide the final "
        "answer on a new line starting with 'ANSWER:'.";
    TokenCount max_reasoning_tokens_{1024};
};

// ─────────────────────────────────────────────────────────────
// § CoTAgent::parse_response()
// ─────────────────────────────────────────────────────────────

inline std::pair<std::string, std::string>
CoTAgent::parse_response(const std::string& content) {
    // Look for "ANSWER:" delimiter (case-insensitive first letter)
    auto pos = content.find("ANSWER:");
    if (pos == std::string::npos) {
        pos = content.find("Answer:");
    }
    if (pos == std::string::npos) {
        // No delimiter found; entire content is the answer
        return {content, ""};
    }

    std::string reasoning = content.substr(0, pos);
    std::string answer = content.substr(pos + 7); // skip "ANSWER:" or "Answer:"

    // Trim leading whitespace from answer
    auto start = answer.find_first_not_of(" \n\r\t");
    if (start != std::string::npos) {
        answer = answer.substr(start);
    } else {
        answer.clear();
    }

    return {reasoning, answer};
}

// ─────────────────────────────────────────────────────────────
// § CoTAgent::run()
// ─────────────────────────────────────────────────────────────

inline Result<std::string> CoTAgent::run(std::string user_input) {
    if (!this->os_)
        return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
    if (!this->alive_->load(std::memory_order_acquire))
        return make_error(ErrorCode::InvalidArgument, "Agent has been destroyed");

    auto& kernel = os_->kernel();
    auto& ctx_win = os_->ctx().get_window(id_, config_.context_limit);

    // Build LLM request with CoT instruction
    kernel::LLMRequest req;
    req.agent_id = id_;
    req.priority = config_.priority;

    // System prompt: role_prompt + CoT instruction
    std::string system_text;
    if (!config_.role_prompt.empty()) {
        system_text = config_.role_prompt + "\n\n" + cot_prompt_;
    } else {
        system_text = cot_prompt_;
    }
    req.messages.push_back(kernel::Message::system(system_text));

    // Append existing context messages
    for (const auto& msg : ctx_win.messages()) {
        if (msg.role != kernel::Role::System) {
            req.messages.push_back(msg);
        }
    }

    // Append user input
    req.messages.push_back(kernel::Message::user(user_input));
    req.max_tokens = max_reasoning_tokens_;

    auto result = kernel.infer(req);
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }

    auto [reasoning, answer] = parse_response(result->content);

    // Store in context for future turns
    os_->ctx().append(id_, kernel::Message::user(user_input));
    os_->ctx().append(id_, kernel::Message::assistant(result->content));

    // Remember the exchange
    (void)this->remember(
        fmt::format("CoT Q: {} -> A: {}", user_input,
                    answer.empty() ? result->content : answer),
        0.6f);

    return answer.empty() ? result->content : answer;
}

} // namespace agentos
