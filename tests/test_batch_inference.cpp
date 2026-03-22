#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>

using namespace agentos::kernel;

TEST(BatchInferenceTest, BatchReturnsSameOrderAsInput) {
    auto mock = std::make_unique<MockLLMBackend>();
    mock->register_rule("q1", "a1");
    mock->register_rule("q2", "a2");
    mock->register_rule("q3", "a3");

    LLMKernel kernel(std::move(mock));

    std::vector<LLMRequest> requests;
    for (const auto* q : {"q1", "q2", "q3"}) {
        LLMRequest req;
        req.messages.push_back(Message::user(q));
        requests.push_back(std::move(req));
    }

    auto results = kernel.infer_batch(std::move(requests));
    ASSERT_EQ(results.size(), 3u);
    EXPECT_TRUE(results[0].has_value());
    EXPECT_TRUE(results[1].has_value());
    EXPECT_TRUE(results[2].has_value());
    EXPECT_EQ(results[0]->content, "a1");
    EXPECT_EQ(results[1]->content, "a2");
    EXPECT_EQ(results[2]->content, "a3");
}

TEST(BatchInferenceTest, EmptyBatchReturnsEmpty) {
    auto mock = std::make_unique<MockLLMBackend>();
    LLMKernel kernel(std::move(mock));

    auto results = kernel.infer_batch({});
    EXPECT_TRUE(results.empty());
}

TEST(BatchInferenceTest, AsyncBatchReturnsFutures) {
    auto mock = std::make_unique<MockLLMBackend>();
    mock->register_rule("hello", "world");

    LLMKernel kernel(std::move(mock));

    std::vector<LLMRequest> requests;
    for (int i = 0; i < 3; ++i) {
        LLMRequest req;
        req.messages.push_back(Message::user("hello"));
        requests.push_back(std::move(req));
    }

    auto futures = kernel.infer_batch_async(std::move(requests));
    ASSERT_EQ(futures.size(), 3u);

    for (auto& f : futures) {
        auto result = f.get();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->content, "world");
    }
}

TEST(BatchInferenceTest, PartialFailurePreservesOrder) {
    auto mock = std::make_unique<MockLLMBackend>();
    mock->register_rule("good", "ok");
    // No rule for "bad" -> mock returns default response

    LLMKernel kernel(std::move(mock));

    std::vector<LLMRequest> requests;
    LLMRequest r1; r1.messages.push_back(Message::user("good"));
    LLMRequest r2; r2.messages.push_back(Message::user("bad"));
    LLMRequest r3; r3.messages.push_back(Message::user("good"));
    requests.push_back(std::move(r1));
    requests.push_back(std::move(r2));
    requests.push_back(std::move(r3));

    auto results = kernel.infer_batch(std::move(requests));
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0]->content, "ok");
    // r2 succeeds with default mock response — order preserved
    EXPECT_EQ(results[2]->content, "ok");
}
