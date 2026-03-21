#pragma once
// ============================================================
// AgentOS :: Log Common Types
// Shared enums and structs for the logging subsystem
// ============================================================
#include <string>

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

struct LogField {
  std::string key;
  std::string value;
};

} // namespace agentos
