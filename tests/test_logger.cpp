// ============================================================
// AgentOS Logger Tests
// ============================================================
#include <agentos/core/logger.hpp>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace agentos;

TEST(LoggerTest, DefaultLevelIsWarn) {
  EXPECT_EQ(Logger::instance().level(), LogLevel::Warn);
}

TEST(LoggerTest, LevelFilteringWorks) {
  std::vector<std::string> captured;
  std::mutex cap_mu;

  Logger::instance().set_level(LogLevel::Warn);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    std::lock_guard lk(cap_mu);
    captured.emplace_back(msg);
  });

  LOG_DEBUG("debug msg");
  LOG_INFO("info msg");
  LOG_WARN("warn msg");
  LOG_ERROR("error msg");

  Logger::instance().flush();

  std::lock_guard lk(cap_mu);
  // Only Warn and Error should pass
  EXPECT_EQ(captured.size(), 2u);
  EXPECT_NE(captured[0].find("WARN"), std::string::npos);
  EXPECT_NE(captured[1].find("ERROR"), std::string::npos);

  // Cleanup
  Logger::instance().set_sink(nullptr);
  Logger::instance().set_level(LogLevel::Warn);
}

TEST(LoggerTest, DebugLevelCapturesAll) {
  std::vector<std::string> captured;
  std::mutex cap_mu;

  Logger::instance().set_level(LogLevel::Debug);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    std::lock_guard lk(cap_mu);
    captured.emplace_back(msg);
  });

  LOG_DEBUG("d");
  LOG_INFO("i");
  LOG_WARN("w");
  LOG_ERROR("e");

  Logger::instance().flush();

  std::lock_guard lk(cap_mu);
#ifdef NDEBUG
  EXPECT_EQ(captured.size(), 3u);
#else
  EXPECT_EQ(captured.size(), 4u);
#endif

  Logger::instance().set_sink(nullptr);
  Logger::instance().set_level(LogLevel::Warn);
}

TEST(LoggerTest, OffLevelSilencesAll) {
  std::vector<std::string> captured;

  Logger::instance().set_level(LogLevel::Off);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    captured.emplace_back(msg);
  });

  LOG_DEBUG("d");
  LOG_INFO("i");
  LOG_WARN("w");
  LOG_ERROR("e");

  Logger::instance().flush();

  EXPECT_EQ(captured.size(), 0u);

  Logger::instance().set_sink(nullptr);
  Logger::instance().set_level(LogLevel::Warn);
}

TEST(LoggerTest, StructuredLoggingKV) {
  std::string captured;
  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    captured = std::string(msg);
  });

  LOG_INFO_KV("Task started", KV("id", 123), KV("user", "alice"));
  Logger::instance().flush();

  EXPECT_NE(captured.find("Task started"), std::string::npos);
  EXPECT_NE(captured.find("id=123"), std::string::npos);
  EXPECT_NE(captured.find("user=alice"), std::string::npos);

  Logger::instance().set_sink(nullptr);
}
