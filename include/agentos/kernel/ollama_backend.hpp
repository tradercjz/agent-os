#pragma once
// ============================================================
// AgentOS :: Kernel — Ollama Backend
// Local LLM inference via Ollama REST API
// ============================================================
#include <agentos/kernel/http_client.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <memory>
#include <string>

namespace agentos::kernel {

/// LLM backend targeting the Ollama local inference server.
///
/// Endpoints used:
///   POST /api/chat   — chat completion (streaming & non-streaming)
///   POST /api/embed  — text embeddings
///
/// Constructor accepts an optional shared_ptr<HttpClient> for test injection.
class OllamaBackend : public ILLMBackend {
public:
    explicit OllamaBackend(std::string model = "llama3",
                           std::string base_url = "http://localhost:11434",
                           std::shared_ptr<HttpClient> http = nullptr);

    [[nodiscard]] Result<LLMResponse> complete(const LLMRequest& req) override;
    [[nodiscard]] Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    [[nodiscard]] Result<EmbeddingResponse> embed(const EmbeddingRequest& req) override;
    std::string name() const noexcept override { return "ollama/" + model_; }

    // Exposed for testability
    Json build_chat_request(const LLMRequest& req, bool streaming) const;
    Result<LLMResponse> parse_chat_response(const std::string& body) const;

private:
    std::string model_;
    std::string base_url_;
    std::shared_ptr<HttpClient> http_;
};

} // namespace agentos::kernel
