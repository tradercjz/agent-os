#ifdef AGENTOS_ENABLE_LLAMACPP

#include <agentos/kernel/llamacpp_backend.hpp>
#include <mutex>

namespace agentos::kernel {

struct LlamaCppBackend::Impl {
    Config config;
    std::mutex mu;
};

LlamaCppBackend::LlamaCppBackend(Config config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

LlamaCppBackend::~LlamaCppBackend() = default;

Result<LLMResponse> LlamaCppBackend::complete(const LLMRequest& /*req*/) {
    std::lock_guard lk(impl_->mu);
    return make_error(ErrorCode::NotImplemented,
                     "llama.cpp backend: model not loaded (stub implementation)");
}

Result<LLMResponse> LlamaCppBackend::stream(const LLMRequest& req, TokenCallback /*cb*/) {
    return complete(req);
}

Result<EmbeddingResponse> LlamaCppBackend::embed(const EmbeddingRequest& /*req*/) {
    std::lock_guard lk(impl_->mu);
    return make_error(ErrorCode::NotImplemented,
                     "llama.cpp embedding: not available (stub implementation)");
}

} // namespace agentos::kernel

#endif // AGENTOS_ENABLE_LLAMACPP
