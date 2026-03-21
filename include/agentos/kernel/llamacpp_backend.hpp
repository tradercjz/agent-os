#pragma once

#ifdef AGENTOS_ENABLE_LLAMACPP

#include <agentos/kernel/llm_kernel.hpp>
#include <memory>
#include <string>

namespace agentos::kernel {

class LlamaCppBackend : public ILLMBackend {
public:
    struct Config {
        std::string model_path;
        int n_ctx{4096};
        int n_gpu_layers{0};
        int n_threads{4};
    };

    explicit LlamaCppBackend(Config config);
    ~LlamaCppBackend() override;

    LlamaCppBackend(const LlamaCppBackend&) = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    Result<LLMResponse> complete(const LLMRequest& req) override;
    Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    Result<EmbeddingResponse> embed(const EmbeddingRequest& req) override;
    std::string name() const override { return "llamacpp"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentos::kernel

#endif // AGENTOS_ENABLE_LLAMACPP
