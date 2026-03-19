#include <gtest/gtest.h>
#include <agentos/news/news_parser.hpp>

using namespace agentos;
using namespace agentos::news;

// Test NewsArticle initialization and defaults
TEST(NewsParserTest, NewsArticleInitialization) {
    NewsArticle article;
    EXPECT_EQ(article.id, "");
    EXPECT_EQ(article.title, "");
    EXPECT_EQ(article.content, "");
    EXPECT_EQ(article.summary, "");
    EXPECT_EQ(article.source, "");
    EXPECT_EQ(article.author, "");
    EXPECT_EQ(article.url, "");
    EXPECT_EQ(article.category, "");
    EXPECT_TRUE(article.tags.empty());
    EXPECT_DOUBLE_EQ(article.sentiment_score, 0.0);
    EXPECT_DOUBLE_EQ(article.importance_score, 0.5);
    EXPECT_TRUE(article.metadata.empty());
}

// Test NewsArticle assignment
TEST(NewsParserTest, NewsArticleAssignment) {
    NewsArticle article;
    article.id = "123";
    article.title = "Test Title";
    article.sentiment_score = 0.8;
    article.importance_score = 0.9;

    EXPECT_EQ(article.id, "123");
    EXPECT_EQ(article.title, "Test Title");
    EXPECT_DOUBLE_EQ(article.sentiment_score, 0.8);
    EXPECT_DOUBLE_EQ(article.importance_score, 0.9);
}

// Test NewsSourceConfig initialization and defaults
TEST(NewsParserTest, NewsSourceConfigInitialization) {
    NewsSourceConfig config;
    EXPECT_EQ(config.name, "");
    EXPECT_EQ(config.type, "");
    EXPECT_EQ(config.url, "");
    EXPECT_EQ(config.api_key, "");
    EXPECT_TRUE(config.categories.empty());
    EXPECT_EQ(config.update_interval_seconds, 300);
    EXPECT_TRUE(config.headers.empty());
    EXPECT_TRUE(config.params.empty());
}

// Test NewsSourceConfig assignment
TEST(NewsParserTest, NewsSourceConfigAssignment) {
    NewsSourceConfig config;
    config.name = "Test Source";
    config.type = "rss";
    config.update_interval_seconds = 600;

    EXPECT_EQ(config.name, "Test Source");
    EXPECT_EQ(config.type, "rss");
    EXPECT_EQ(config.update_interval_seconds, 600);
}

// Mock INewsParser for testing the interface
class MockNewsParser : public INewsParser {
public:
    [[nodiscard]] Result<std::vector<NewsArticle>> parse(const std::string& raw_data) override {
        if (raw_data == "valid") {
            NewsArticle article;
            article.id = "mock_id";
            return std::vector<NewsArticle>{article};
        }
        return make_error(ErrorCode::InvalidArgument, "Invalid data");
    }

    std::string get_type() const noexcept override { return "mock"; }

    bool validate_format(const std::string& raw_data) const noexcept override {
        return raw_data == "valid";
    }
};

// Test INewsParser interface via mock
TEST(NewsParserTest, INewsParserInterface) {
    MockNewsParser parser;

    EXPECT_EQ(parser.get_type(), "mock");
    EXPECT_TRUE(parser.validate_format("valid"));
    EXPECT_FALSE(parser.validate_format("invalid"));

    auto result_valid = parser.parse("valid");
    ASSERT_TRUE(result_valid.has_value());
    EXPECT_EQ(result_valid.value().size(), 1u);
    EXPECT_EQ(result_valid.value()[0].id, "mock_id");

    auto result_invalid = parser.parse("invalid");
    EXPECT_FALSE(result_invalid.has_value());
    EXPECT_EQ(result_invalid.error().code, ErrorCode::InvalidArgument);
}
