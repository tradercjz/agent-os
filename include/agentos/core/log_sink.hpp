#pragma once
// ============================================================
// AgentOS :: Log Sink Interface
// Pluggable output destinations for the logging subsystem
// ============================================================
#include <agentos/core/log_common.hpp>
#include <agentos/core/compat.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

/// Structured log event passed to sinks.
struct LogEvent {
  LogLevel level;
  std::string timestamp;   // UTC ISO-8601
  std::string filename;
  int line;
  std::string msg;
  std::vector<LogField> fields;
};

/// Abstract sink interface — implementations decide where logs go.
class ILogSink {
public:
  virtual ~ILogSink() = default;
  virtual void write(const LogEvent &event) = 0;
  virtual void flush() {}
};

/// Console sink — writes human-readable log lines to stderr (or a custom writer).
class ConsoleSink : public ILogSink {
public:
  /// Default: writes to stderr via std::fputs.
  ConsoleSink() : writer_([](std::string_view s) {
    std::fputs(std::string(s).c_str(), stderr);
  }) {}

  /// Testable: caller supplies a writer function.
  explicit ConsoleSink(std::function<void(std::string_view)> writer)
      : writer_(std::move(writer)) {}

  void write(const LogEvent &event) override {
    std::string formatted;
    if (event.fields.empty()) {
      formatted = ::agentos::fmt::format("[{}] [{}] {}:{}  {}\n",
          event.timestamp, log_level_str(event.level),
          event.filename, event.line, event.msg);
    } else {
      std::string kv_part;
      for (const auto &f : event.fields) {
        kv_part += ::agentos::fmt::format(" {}={}", f.key, f.value);
      }
      formatted = ::agentos::fmt::format("[{}] [{}] {}:{}  {}{}\n",
          event.timestamp, log_level_str(event.level),
          event.filename, event.line, event.msg, kv_part);
    }
    writer_(formatted);
  }

  void flush() override {
    // stderr is unbuffered by default; nothing to do.
  }

private:
  std::function<void(std::string_view)> writer_;
};

// ── JsonFileSink: JSON Lines file output with rotation ──
class JsonFileSink : public ILogSink {
public:
    struct RotationConfig {
        size_t max_bytes = 50 * 1024 * 1024;  // 50MB default
    };

    explicit JsonFileSink(std::string path)
        : path_(std::move(path)), rot_(), bytes_written_(0) {
        ofs_.open(path_, std::ios::app);
    }

    JsonFileSink(std::string path, RotationConfig rot)
        : path_(std::move(path)), rot_(rot), bytes_written_(0) {
        ofs_.open(path_, std::ios::app);
    }

    void write(const LogEvent& event) override {
        nlohmann::json j;
        j["ts"] = event.timestamp;
        // log_level_str returns "INFO " with trailing space — trim it
        std::string level_str = log_level_str(event.level);
        while (!level_str.empty() && level_str.back() == ' ') level_str.pop_back();
        j["level"] = level_str;
        j["file"] = event.filename;
        j["line"] = event.line;
        j["msg"] = event.msg;

        if (!event.fields.empty()) {
            nlohmann::json extra;
            for (const auto& f : event.fields) {
                extra[f.key] = f.value;
            }
            j["extra"] = extra;
        }

        std::string line = j.dump() + "\n";
        bytes_written_ += line.size();
        ofs_ << line;

        if (rot_.max_bytes > 0 && bytes_written_ >= rot_.max_bytes) {
            rotate();
        }
    }

    void flush() override {
        if (ofs_.is_open()) ofs_.flush();
    }

private:
    void rotate() {
        ofs_.close();

        auto now_sys = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now_sys);
        struct tm utc_tm;
#ifdef _WIN32
        gmtime_s(&utc_tm, &time_t_now);
#else
        gmtime_r(&time_t_now, &utc_tm);
#endif
        char time_buf[20];
        std::strftime(time_buf, sizeof(time_buf), "%Y%m%d%H%M%S", &utc_tm);

        std::string rotated = path_ + "." + time_buf + ".jsonl";

        if (std::filesystem::exists(rotated)) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now_sys.time_since_epoch()) % 1000;
            rotated = path_ + "." + time_buf + "." +
                      std::to_string(ms.count()) + ".jsonl";
        }

        std::error_code ec;
        std::filesystem::rename(path_, rotated, ec);
        // If rename fails, just reopen (data stays in old file)

        ofs_.open(path_, std::ios::app);
        bytes_written_ = 0;
    }

    std::string path_;
    RotationConfig rot_;
    std::ofstream ofs_;
    size_t bytes_written_;
};

} // namespace agentos
