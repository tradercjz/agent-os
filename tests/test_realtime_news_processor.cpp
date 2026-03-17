#include <gtest/gtest.h>
#include <agentos/news/realtime_news_processor.hpp>
#include <chrono>
#include <thread>

using namespace agentos::news;

TEST(RealtimeNewsProcessorTest, ProcessingStatsInstantiation) {
    ProcessingStats stats;
    EXPECT_EQ(stats.total_articles_processed.load(), 0);
    EXPECT_EQ(stats.articles_per_second.load(), 0);
    EXPECT_EQ(stats.entities_extracted.load(), 0);

    stats.total_articles_processed = 10;
    stats.reset();
    EXPECT_EQ(stats.total_articles_processed.load(), 0);
}

TEST(RealtimeNewsProcessorTest, ProcessingStatsUpdateArticlesPerSecond) {
    ProcessingStats stats;
    stats.total_articles_processed = 10;
    // Sleep to ensure duration > 0
    std::this_thread::sleep_for(std::chrono::seconds(1));
    stats.update_articles_per_second();
    EXPECT_GT(stats.articles_per_second.load(), 0);
    EXPECT_LE(stats.articles_per_second.load(), 10);
}

TEST(RealtimeNewsProcessorTest, RealtimeConfigDefaults) {
    RealtimeConfig config;
    EXPECT_EQ(config.parser_threads, 2);
    EXPECT_EQ(config.max_queue_size, 10000);
    EXPECT_TRUE(config.enable_batch_processing);
}

TEST(RealtimeNewsProcessorTest, NewsEventInstantiation) {
    NewsEvent event(NewsEventType::ARTICLE_RECEIVED, "test_data", "source_1");
    EXPECT_EQ(event.type, NewsEventType::ARTICLE_RECEIVED);
    EXPECT_EQ(event.data, "test_data");
    EXPECT_EQ(event.source_id, "source_1");
}
