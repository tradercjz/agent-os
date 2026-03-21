#include <agentos/core/log_sink.hpp>
#include <gtest/gtest.h>
#include <string>

using namespace agentos;

TEST(ConsoleSinkTest, WritesHumanReadableFormat) {
  std::string captured;
  ConsoleSink sink([&](std::string_view s) { captured += s; });

  LogEvent event;
  event.level = LogLevel::Info;
  event.timestamp = "2026-03-21T12:00:00.000Z";
  event.filename = "main.cpp";
  event.line = 42;
  event.msg = "server started";

  sink.write(event);

  // Verify level tag present
  EXPECT_NE(captured.find("INFO"), std::string::npos);
  // Verify file:line present
  EXPECT_NE(captured.find("main.cpp:42"), std::string::npos);
  // Verify message present
  EXPECT_NE(captured.find("server started"), std::string::npos);
  // Verify timestamp present
  EXPECT_NE(captured.find("2026-03-21T12:00:00.000Z"), std::string::npos);
  // Verify trailing newline
  EXPECT_FALSE(captured.empty());
  EXPECT_EQ(captured.back(), '\n');
}

TEST(ConsoleSinkTest, WritesKVFields) {
  std::string captured;
  ConsoleSink sink([&](std::string_view s) { captured += s; });

  LogEvent event;
  event.level = LogLevel::Warn;
  event.timestamp = "2026-03-21T12:00:00.000Z";
  event.filename = "handler.cpp";
  event.line = 99;
  event.msg = "request slow";
  event.fields = {{"latency_ms", "350"}, {"path", "/api/v1"}};

  sink.write(event);

  // Verify key=value pairs appear
  EXPECT_NE(captured.find("latency_ms=350"), std::string::npos);
  EXPECT_NE(captured.find("path=/api/v1"), std::string::npos);
  // Verify the message is still present
  EXPECT_NE(captured.find("request slow"), std::string::npos);
  // Verify level
  EXPECT_NE(captured.find("WARN"), std::string::npos);
}
