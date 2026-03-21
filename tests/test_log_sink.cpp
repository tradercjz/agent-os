#include <agentos/core/log_sink.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
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

TEST(JsonFileSinkTest, WritesJsonLines) {
    auto tmp = std::filesystem::temp_directory_path() / "test_json_sink.jsonl";
    std::filesystem::remove(tmp);

    {
        agentos::JsonFileSink sink(tmp.string());
        agentos::LogEvent event{
            .level = agentos::LogLevel::Info,
            .timestamp = "2026-03-21T14:00:01.234Z",
            .filename = "test.cpp",
            .line = 42,
            .msg = "hello",
            .fields = {{"key1", "val1"}}
        };
        sink.write(event);
        sink.flush();
    }

    std::ifstream ifs(tmp);
    std::string line;
    ASSERT_TRUE(std::getline(ifs, line));
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["level"], "INFO");
    EXPECT_EQ(j["msg"], "hello");
    EXPECT_EQ(j["file"], "test.cpp");
    EXPECT_EQ(j["line"], 42);
    EXPECT_EQ(j["extra"]["key1"], "val1");

    std::filesystem::remove(tmp);
}

TEST(JsonFileSinkTest, RotatesOnSizeLimit) {
    auto tmp_dir = std::filesystem::temp_directory_path() / "test_rotation";
    std::filesystem::create_directories(tmp_dir);
    auto logfile = tmp_dir / "app.jsonl";

    {
        agentos::JsonFileSink sink(logfile.string(), agentos::JsonFileSink::RotationConfig{
            .max_bytes = 100
        });

        agentos::LogEvent event{
            .level = agentos::LogLevel::Info,
            .timestamp = "2026-03-21T14:00:01.234Z",
            .filename = "test.cpp",
            .line = 1,
            .msg = "a message that is long enough to exceed 100 bytes when serialized as JSON",
            .fields = {}
        };
        sink.write(event);
        sink.write(event);
        sink.flush();
    }

    bool found_rotated = false;
    for (auto& entry : std::filesystem::directory_iterator(tmp_dir)) {
        if (entry.path().string().find("app.") != std::string::npos &&
            entry.path() != logfile) {
            found_rotated = true;
        }
    }
    EXPECT_TRUE(found_rotated);

    std::filesystem::remove_all(tmp_dir);
}
