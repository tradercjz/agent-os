#include <agentos/context/compressor.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::context;
using namespace agentos::kernel;

TEST(ContextCompressorTest, ShouldCompressAtThreshold) {
    ContextCompressor comp({.trigger_ratio = 0.8f});
    EXPECT_FALSE(comp.should_compress(700, 1000));
    EXPECT_TRUE(comp.should_compress(800, 1000));
    EXPECT_TRUE(comp.should_compress(900, 1000));
    EXPECT_FALSE(comp.should_compress(0, 0));
}

TEST(ContextCompressorTest, CompressWithSimpleSummarizer) {
    ContextCompressor comp({.min_messages_to_keep = 2, .min_messages_to_compress = 2});

    std::vector<Message> msgs;
    msgs.push_back(Message::system("You are helpful"));
    msgs.push_back(Message::user("What is 2+2?"));
    msgs.push_back(Message::assistant("4"));
    msgs.push_back(Message::user("What is 3+3?"));
    msgs.push_back(Message::assistant("6"));
    msgs.push_back(Message::user("Latest question"));
    msgs.push_back(Message::assistant("Latest answer"));

    auto summarizer = ContextCompressor::make_simple_summarizer(200);
    auto result = comp.compress(msgs, 4096, summarizer);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->compressed);
    EXPECT_LT(result->messages_after, result->messages_before);
    // Should keep system msg + summary + last 2 messages
    EXPECT_LE(msgs.size(), 5u);
}

TEST(ContextCompressorTest, TooFewMessagesSkipsCompression) {
    ContextCompressor comp({.min_messages_to_keep = 4, .min_messages_to_compress = 3});

    std::vector<Message> msgs;
    msgs.push_back(Message::user("Hello"));
    msgs.push_back(Message::assistant("Hi"));

    auto summarizer = ContextCompressor::make_simple_summarizer();
    auto result = comp.compress(msgs, 4096, summarizer);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->compressed);
    EXPECT_EQ(msgs.size(), 2u);
}

TEST(ContextCompressorTest, SystemMessagesPreserved) {
    ContextCompressor comp({.min_messages_to_keep = 1, .min_messages_to_compress = 2});

    std::vector<Message> msgs;
    msgs.push_back(Message::system("System prompt"));
    msgs.push_back(Message::user("Q1"));
    msgs.push_back(Message::assistant("A1"));
    msgs.push_back(Message::user("Q2"));
    msgs.push_back(Message::assistant("A2"));
    msgs.push_back(Message::user("Q3"));

    auto summarizer = ContextCompressor::make_simple_summarizer();
    auto result = comp.compress(msgs, 4096, summarizer);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->compressed);
    // First message should still be system
    EXPECT_EQ(msgs[0].role, Role::System);
    EXPECT_EQ(msgs[0].content, "System prompt");
}

TEST(ContextCompressorTest, SummarizerFailurePreservesMessages) {
    ContextCompressor comp({.min_messages_to_keep = 1, .min_messages_to_compress = 2});

    std::vector<Message> msgs;
    msgs.push_back(Message::user("Q1"));
    msgs.push_back(Message::assistant("A1"));
    msgs.push_back(Message::user("Q2"));
    msgs.push_back(Message::assistant("A2"));
    msgs.push_back(Message::user("Q3"));
    size_t original_count = msgs.size();

    auto failing_summarizer = [](const std::vector<Message>&) -> Result<std::string> {
        return make_error(ErrorCode::LLMBackendError, "summarizer failed");
    };

    auto result = comp.compress(msgs, 4096, failing_summarizer);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(msgs.size(), original_count); // messages restored
}

TEST(ContextCompressorTest, LLMSummarizerWithMock) {
    auto mock = std::make_unique<MockLLMBackend>();
    mock->register_rule("User:", "Summary: discussed math");
    LLMKernel kernel(std::move(mock));

    auto summarizer = ContextCompressor::make_llm_summarizer(kernel);

    std::vector<Message> msgs;
    msgs.push_back(Message::user("What is 2+2?"));
    msgs.push_back(Message::assistant("4"));

    auto result = summarizer(msgs);
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Summary"), std::string::npos);
}
