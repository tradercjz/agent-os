#pragma once
// ============================================================
// AgentOS :: Log Sink Interface
// Pluggable output destinations for the logging subsystem
// ============================================================
#include <agentos/core/log_common.hpp>
#include <agentos/core/compat.hpp>
#include <cstdio>
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

} // namespace agentos
