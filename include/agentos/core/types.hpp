#pragma once
// ============================================================
// AgentOS :: Core Types
// 基础类型、错误体系、通用工具
// ============================================================
#include <agentos/core/compat.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace agentos {

// ── std::format 快捷别名（使用 agentos::fmt::format）──────────
// 在整个项目中写 std::format(...) 会冲突，改用 agentos 内部版本
// 为方便阅读，提供全局 using（仅在 agentos 命名空间内）
// using fmt::format;

// ── ID 类型 ──────────────────────────────────────────────────
using AgentId = std::uint64_t;
using TaskId = std::uint64_t;
using ToolId = std::string;
using SessionId = std::string;
using TokenCount = std::uint32_t;

// ── 优先级 ────────────────────────────────────────────────────
enum class Priority : int {
  Low = 0,
  Normal = 5,
  High = 10,
  Critical = 20,
};

// ── 错误体系 ──────────────────────────────────────────────────
enum class ErrorCode : int {
  Ok = 0,
  Unknown,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  Timeout,
  Cancelled,
  LLMBackendError,
  RateLimitExceeded,
  TokenBudgetExceeded,
  ContextWindowFull,
  DeadlockDetected,
  CircularDependency,
  PermissionDenied,
  TaintedInput,
  InjectionDetected,
  ToolNotFound,
  ToolExecutionFailed,
  SandboxViolation,
  MemoryReadFailed,
  MemoryWriteFailed,
};

struct Error {
  Error() = default;
  ErrorCode code;
  std::string message;
  SourceLocation loc;

  Error(ErrorCode c, std::string msg,
        SourceLocation l = SourceLocation::current())
      : code(c), message(std::move(msg)), loc(l) {}

  std::string to_string() const {
    return fmt::format("[Error {}] {} ({}:{})", static_cast<int>(code), message,
                       loc.file_name(), loc.line_number());
  }
};

template <typename T> using Result = Expected<T, Error>;

inline auto make_error(ErrorCode c, std::string msg,
                       SourceLocation loc = SourceLocation::current()) {
  return make_unexpected(Error{c, std::move(msg), loc});
}

// ── 时间工具 ──────────────────────────────────────────────────
using Clock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;
using Duration = std::chrono::milliseconds;

inline TimePoint now() { return Clock::now(); }

using Json = nlohmann::json;

// ── 不可拷贝基类 ──────────────────────────────────────────────
struct NonCopyable {
  NonCopyable() = default;
  NonCopyable(const NonCopyable &) = delete;
  NonCopyable &operator=(const NonCopyable &) = delete;
  NonCopyable(NonCopyable &&) = default;
  NonCopyable &operator=(NonCopyable &&) = default;
};

} // namespace agentos
