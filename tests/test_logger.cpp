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

  Logger::instance().set_level(LogLevel::Warn);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    captured.emplace_back(msg);
  });

  LOG_DEBUG("debug msg");
  LOG_INFO("info msg");
  LOG_WARN("warn msg");
  LOG_ERROR("error msg");

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

  Logger::instance().set_level(LogLevel::Debug);
  Logger::instance().set_sink([&](LogLevel, std::string_view msg) {
    captured.emplace_back(msg);
  });

  LOG_DEBUG("d");
  LOG_INFO("i");
  LOG_WARN("w");
  LOG_ERROR("e");

  EXPECT_EQ(captured.size(), 4u);

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

  EXPECT_EQ(captured.size(), 0u);

  Logger::instance().set_sink(nullptr);
  Logger::instance().set_level(LogLevel::Warn);
}

TEST(LoggerTest, CustomSinkReceivesMessages) {
  LogLevel received_level = LogLevel::Off;
  std::string received_msg;

  Logger::instance().set_level(LogLevel::Info);
  Logger::instance().set_sink([&](LogLevel level, std::string_view msg) {
    received_level = level;
    received_msg = std::string(msg);
  });

  LOG_INFO("test message");

  EXPECT_EQ(received_level, LogLevel::Info);
  EXPECT_NE(received_msg.find("test message"), std::string::npos);

  Logger::instance().set_sink(nullptr);
  Logger::instance().set_level(LogLevel::Warn);
}
