#include <gtest/gtest.h>

#ifdef AGENTOS_ENABLE_LLAMACPP

#include <agentos/kernel/llamacpp_backend.hpp>

using namespace agentos::kernel;

TEST(LlamaCppBackendTest, NameReturnsLlamacpp) {
    LlamaCppBackend::Config cfg;
    LlamaCppBackend backend(cfg);
    EXPECT_EQ(backend.name(), "llamacpp");
}

TEST(LlamaCppBackendTest, StubReturnsNotImplemented) {
    LlamaCppBackend::Config cfg;
    LlamaCppBackend backend(cfg);
    LLMRequest req;
    req.messages.push_back(Message::user("hello"));
    auto r = backend.complete(req);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotImplemented);
}

#endif // AGENTOS_ENABLE_LLAMACPP
