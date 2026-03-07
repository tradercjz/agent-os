#pragma once
// ============================================================
// AgentOS :: Lightweight Logger
// 零外部依赖的分级日志系统
// ============================================================
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace agentos {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

inline const char *log_level_str(LogLevel level) {
  switch (level) {
  case LogLevel::Debug: return "DEBUG";
  case LogLevel::Info:  return "INFO ";
  case LogLevel::Warn:  return "WARN ";
  case LogLevel::Error: return "ERROR";
  default:              return "?????";
  }
}

// Custom log sink function signature
using LogSink = std::function<void(LogLevel, std::string_view)>;

class Logger {
public:
  static Logger &instance() {
    static Logger logger;
    return logger;
  }

  void set_level(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }

  // Set a custom sink (e.g., to file or structured output)
  void set_sink(LogSink sink) {
    std::lock_guard lk(mu_);
    custom_sink_ = std::move(sink);
  }

  void log(LogLevel level, std::string_view msg,
           const char *file = __builtin_FILE(),
           int line = __builtin_LINE()) {
    if (level < level_) return;

    // Format timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    char time_buf[24];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S",
                  std::localtime(&time_t_now));

    // Extract filename from path
    std::string_view filepath(file);
    auto slash = filepath.rfind('/');
    if (slash != std::string_view::npos) filepath = filepath.substr(slash + 1);

    char buf[512];
    int len = std::snprintf(buf, sizeof(buf), "[%s.%03d] [%s] %s:%d  %.*s\n",
                            time_buf, static_cast<int>(ms.count()),
                            log_level_str(level),
                            filepath.data(), line,
                            static_cast<int>(msg.size()), msg.data());

    std::lock_guard lk(mu_);
    if (custom_sink_) {
      custom_sink_(level, std::string_view(buf, len > 0 ? len : 0));
    } else {
      std::fputs(buf, stderr);
    }
  }

private:
  Logger() = default;
  LogLevel level_{LogLevel::Warn}; // Default: only warnings and errors
  LogSink custom_sink_;
  std::mutex mu_;
};

// Convenience macros
#define AGENTOS_LOG(level, msg) \
  ::agentos::Logger::instance().log(level, msg, __builtin_FILE(), __builtin_LINE())

#define LOG_DEBUG(msg) AGENTOS_LOG(::agentos::LogLevel::Debug, msg)
#define LOG_INFO(msg)  AGENTOS_LOG(::agentos::LogLevel::Info, msg)
#define LOG_WARN(msg)  AGENTOS_LOG(::agentos::LogLevel::Warn, msg)
#define LOG_ERROR(msg) AGENTOS_LOG(::agentos::LogLevel::Error, msg)

} // namespace agentos
