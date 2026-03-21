#include <agentos/kernel/router_backend.hpp>

namespace agentos::kernel {

void RouterBackend::add_rule(RoutingRule rule) {
    std::lock_guard lk(mu_);
    rules_.push_back(std::move(rule));
}

void RouterBackend::set_default(std::shared_ptr<ILLMBackend> backend) {
    std::lock_guard lk(mu_);
    default_backend_ = std::move(backend);
}

ILLMBackend* RouterBackend::resolve(const LLMRequest& req) const {
    std::lock_guard lk(mu_);
    for (const auto& rule : rules_) {
        if (rule.predicate(req)) {
            return rule.backend.get();
        }
    }
    return default_backend_.get();
}

Result<LLMResponse> RouterBackend::complete(const LLMRequest& req) {
    auto* backend = resolve(req);
    if (!backend) {
        return make_error(ErrorCode::NotFound,
                          "RouterBackend: no matching backend for request");
    }
    return backend->complete(req);
}

Result<LLMResponse> RouterBackend::stream(const LLMRequest& req,
                                           TokenCallback cb) {
    auto* backend = resolve(req);
    if (!backend) {
        return make_error(ErrorCode::NotFound,
                          "RouterBackend: no matching backend for request");
    }
    return backend->stream(req, std::move(cb));
}

Result<EmbeddingResponse> RouterBackend::embed(const EmbeddingRequest& req) {
    // Use default backend for embeddings (most specific backends don't support it)
    std::lock_guard lk(mu_);
    if (default_backend_) return default_backend_->embed(req);
    // Try each backend's embed
    for (const auto& rule : rules_) {
        auto r = rule.backend->embed(req);
        if (r.has_value()) return r;
    }
    return make_error(ErrorCode::NotImplemented,
                      "RouterBackend: no backend supports embeddings");
}

size_t RouterBackend::rule_count() const {
    std::lock_guard lk(mu_);
    return rules_.size();
}

} // namespace agentos::kernel
