#pragma once
// ============================================================
// AgentOS :: Lightweight Logger
// 零外部依赖的分级日志系统
// ============================================================
#include <agentos/core/log_common.hpp>
#include <agentos/core/types.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentos {

// Custom log sink function signature
using LogSink = std::function<void(LogLevel, std::string_view)>;

class Logger : private NonCopyable {
public:
  static Logger &instance() {
    static Logger logger;
    return logger;
  }

  static constexpr LogLevel kDefaultLogLevel = LogLevel::Warn;

  void set_level(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }

  void set_sink(LogSink sink) {
    std::lock_guard lk(mu_);
    custom_sink_ = std::move(sink);
  }

  // Synchronous log (for emergency or early startup)
  void log_sync(LogLevel level, std::string_view msg,
                const char *file = __builtin_FILE(),
                int line = __builtin_LINE()) {
    if (level < level_) return;
    auto event = prepare_event(level, msg, {}, file, line);
    process_event(event);
  }

  // Asynchronous log (standard)
  void log(LogLevel level, std::string_view msg,
           const char *file = __builtin_FILE(),
           int line = __builtin_LINE()) {
    if (level < level_) return;
    push_event(prepare_event(level, msg, {}, file, line));
  }

  // Structured asynchronous log
  void log_kv(LogLevel level, std::string_view msg,
              std::vector<LogField> fields,
              const char *file = __builtin_FILE(),
              int line = __builtin_LINE()) {
    if (level < level_) return;
    push_event(prepare_event(level, msg, std::move(fields), file, line));
  }

  void flush() {
    std::unique_lock lk(mu_);
    cv_flush_.wait(lk, [this] { return queue_.empty(); });
  }

private:
  struct LogEvent {
    LogLevel level;
    std::string timestamp;
    std::string filename;
    int line;
    std::string msg;
    std::vector<LogField> fields;
  };

  Logger() {
    worker_ = std::thread(&Logger::worker_loop, this);
  }

  ~Logger() {
    {
      std::lock_guard lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  LogEvent prepare_event(LogLevel level, std::string_view msg,
                         std::vector<LogField> fields,
                         const char *file, int line) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    char time_buf[24];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_t_now));
    
    std::string ts = ::agentos::fmt::format("{}.{:03d}", time_buf, static_cast<int>(ms.count()));

    std::string_view filepath(file);
    auto slash = filepath.rfind('/');
    if (slash != std::string_view::npos) filepath = filepath.substr(slash + 1);

    return {level, std::move(ts), std::string(filepath), line, std::string(msg), std::move(fields)};
  }

  void push_event(LogEvent event) {
    {
      std::lock_guard lk(mu_);
      if (queue_.size() > kMaxQueueSize) {
        // Simple drop strategy if queue is full to prevent OOM
        return;
      }
      queue_.push_back(std::move(event));
    }
    cv_.notify_one();
  }

  void worker_loop() {
    while (true) {
      std::vector<LogEvent> batch;
      {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] { return !queue_.empty() || stop_; });
        if (stop_ && queue_.empty()) break;
        
        // Grab a batch to minimize lock contention
        batch.reserve(std::min(queue_.size(), size_t(100)));
        while (!queue_.empty() && batch.size() < 100) {
          batch.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
      }
      
      for (const auto &event : batch) {
        process_event(event);
      }
      cv_flush_.notify_all();
    }
  }

  void process_event(const LogEvent &event) {
    std::string formatted;
    if (event.fields.empty()) {
      formatted = ::agentos::fmt::format("[{}] [{}] {}:{}  {}\n", 
                              event.timestamp, log_level_str(event.level),
                              event.filename, event.line, event.msg);
    } else {
      // JSON-like structured output
      std::string kv_part;
      for (const auto &f : event.fields) {
        kv_part += ::agentos::fmt::format(" {}={}", f.key, f.value);
      }
      formatted = ::agentos::fmt::format("[{}] [{}] {}:{}  {}{}\n", 
                              event.timestamp, log_level_str(event.level),
                              event.filename, event.line, event.msg, kv_part);
    }

    LogSink sink_copy;
    {
      std::lock_guard lk(mu_);
      sink_copy = custom_sink_;
    }

    if (sink_copy) {
      sink_copy(event.level, formatted);
    } else {
      std::fputs(formatted.c_str(), stderr);
    }
  }

  LogLevel level_{kDefaultLogLevel};
  LogSink custom_sink_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable cv_flush_;
  std::deque<LogEvent> queue_;
  std::thread worker_;
  bool stop_{false};
  static constexpr size_t kMaxQueueSize = 10000;
};

// Convenience macros
#define AGENTOS_LOG(level, msg) \
  ::agentos::Logger::instance().log(level, msg, __builtin_FILE(), __builtin_LINE())

#ifdef NDEBUG
#define LOG_DEBUG(msg) (void)0
#else
#define LOG_DEBUG(msg) AGENTOS_LOG(::agentos::LogLevel::Debug, msg)
#endif

#define LOG_INFO(msg)  AGENTOS_LOG(::agentos::LogLevel::Info, msg)
#define LOG_WARN(msg)  AGENTOS_LOG(::agentos::LogLevel::Warn, msg)
#define LOG_ERROR(msg) AGENTOS_LOG(::agentos::LogLevel::Error, msg)

#define LOG_INFO_KV(msg, ...) \
  ::agentos::Logger::instance().log_kv(::agentos::LogLevel::Info, msg, { __VA_ARGS__ }, __builtin_FILE(), __builtin_LINE())

#define KV(k, v) ::agentos::LogField{k, ::agentos::fmt::detail::to_str(v)}

} // namespace agentos
