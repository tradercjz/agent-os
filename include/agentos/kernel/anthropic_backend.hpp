#pragma once
// ============================================================
// AgentOS :: Anthropic Claude Backend
// ============================================================
#include <agentos/kernel/http_client.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <memory>
#include <string>

namespace agentos::kernel {

class AnthropicBackend : public ILLMBackend {
public:
    explicit AnthropicBackend(std::string api_key,
                              std::string model = "claude-sonnet-4-20250514",
                              std::string base_url = "https://api.anthropic.com",
                              std::shared_ptr<HttpClient> http = nullptr);

    Result<LLMResponse> complete(const LLMRequest& req) override;
    Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    std::string name() const noexcept override { return "anthropic"; }

    // Exposed for testing
    Json build_request(const LLMRequest& req) const;
    Result<LLMResponse> parse_response(const std::string& body) const;

private:
    std::string api_key_;
    std::string model_;
    std::string base_url_;
    std::shared_ptr<HttpClient> http_;

    std::vector<std::string> build_headers() const;
};

} // namespace agentos::kernel
