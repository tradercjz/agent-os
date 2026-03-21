#pragma once
// ============================================================
// AgentOS :: RouterBackend — Multi-model request routing
// Routes LLM requests to different backends based on configurable rules
// ============================================================
#include <agentos/kernel/llm_kernel.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentos::kernel {

// Routing rule: decides which backend handles a request
struct RoutingRule {
    std::string name;  // rule name for logging
    std::function<bool(const LLMRequest&)> predicate;
    std::shared_ptr<ILLMBackend> backend;
};

// Routes LLM requests to different backends based on configurable rules.
// First matching rule wins; falls back to default backend if none match.
// RouterBackend itself IS an ILLMBackend, so it can be passed to LLMKernel.
class RouterBackend : public ILLMBackend {
public:
    RouterBackend() = default;

    // Add a routing rule (evaluated in order, first match wins)
    void add_rule(RoutingRule rule);

    // Set the default backend (used when no rule matches)
    void set_default(std::shared_ptr<ILLMBackend> backend);

    [[nodiscard]] Result<LLMResponse> complete(const LLMRequest& req) override;
    [[nodiscard]] Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    [[nodiscard]] Result<EmbeddingResponse> embed(const EmbeddingRequest& req) override;
    std::string name() const noexcept override { return "router"; }

    // Get the backend that would handle a given request (for testing)
    ILLMBackend* resolve(const LLMRequest& req) const;

    size_t rule_count() const;

private:
    mutable std::mutex mu_;
    std::vector<RoutingRule> rules_;
    std::shared_ptr<ILLMBackend> default_backend_;
};

// ── Pre-built routing predicates ──

namespace routing {

// Route by estimated token count (simple vs complex)
inline auto by_complexity(TokenCount threshold) {
    return [threshold](const LLMRequest& req) -> bool {
        TokenCount total = 0;
        for (const auto& m : req.messages)
            total += ILLMBackend::estimate_tokens(m.content);
        return total > threshold;
    };
}

// Route by priority
inline auto by_priority(Priority min_priority) {
    return [min_priority](const LLMRequest& req) -> bool {
        return req.priority >= min_priority;
    };
}

// Route by model name prefix
inline auto by_model_prefix(std::string prefix) {
    return [p = std::move(prefix)](const LLMRequest& req) -> bool {
        return req.model.starts_with(p);
    };
}

// Route if request has tools (function calling)
inline auto has_tools() {
    return [](const LLMRequest& req) -> bool {
        return req.tools_json.has_value() && !req.tools_json->empty();
    };
}

} // namespace routing

} // namespace agentos::kernel
