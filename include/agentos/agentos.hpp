#pragma once
// ============================================================
// AgentOS SDK — Umbrella Header + Builder API
//
// 用法：
//   #include <agentos/agentos.hpp>
//   auto os = agentos::AgentOSBuilder()
//                 .openai("sk-...", "gpt-4o-mini")
//                 .threads(4)
//                 .build();
//   auto agent = os->agent("MyBot")
//                    .prompt("You are a helpful assistant.")
//                    .tools({"kv_store", "calculator"})
//                    .create();
//   auto reply = agent->run("Hello!");
// ============================================================

// ── Core modules ────────────────────────────────────────────
#include <agentos/agent.hpp>
#include <agentos/core/key_loader.hpp>
#include <agentos/kernel/ollama_backend.hpp>
#include <agentos/kernel/anthropic_backend.hpp>
#ifdef AGENTOS_ENABLE_LLAMACPP
#include <agentos/kernel/llamacpp_backend.hpp>
#endif
#include <agentos/subworkers/runtime.hpp>
#include <agentos/supervisor_agent.hpp>
#ifndef AGENTOS_NO_DUCKDB
#include <agentos/knowledge/knowledge_base.hpp>
#endif

// ── Version info ────────────────────────────────────────────
#define AGENTOS_VERSION_MAJOR 0
#define AGENTOS_VERSION_MINOR 1
#define AGENTOS_VERSION_PATCH 0
#define AGENTOS_VERSION_STRING "0.1.0"

namespace agentos {

// ─────────────────────────────────────────────────────────────
// § SDK.1  Version
// ─────────────────────────────────────────────────────────────

struct Version {
  int major = AGENTOS_VERSION_MAJOR;
  int minor = AGENTOS_VERSION_MINOR;
  int patch = AGENTOS_VERSION_PATCH;
  std::string to_string() const {
    return fmt::format("{}.{}.{}", major, minor, patch);
  }
};

inline Version version() { return {}; }

// ─────────────────────────────────────────────────────────────
// § SDK.2  AgentBuilder — Fluent Agent Configuration
// ─────────────────────────────────────────────────────────────

class AgentBuilder {
public:
  AgentBuilder(AgentOS *os, std::string name) : os_(os) {
    cfg_.name = std::move(name);
  }

  /// Set the system prompt / role definition
  AgentBuilder &prompt(std::string role_prompt) {
    cfg_.role_prompt = std::move(role_prompt);
    return *this;
  }

  /// Set context window size in tokens
  AgentBuilder &context(TokenCount limit) {
    cfg_.context_limit = limit;
    return *this;
  }

  /// Set task priority
  AgentBuilder &priority(Priority p) {
    cfg_.priority = p;
    return *this;
  }

  /// Set RBAC security role
  AgentBuilder &role(std::string security_role) {
    cfg_.security_role = std::move(security_role);
    return *this;
  }

  /// Restrict allowed tools (empty = all tools)
  AgentBuilder &tools(std::vector<std::string> tool_names) {
    cfg_.allowed_tools = std::move(tool_names);
    return *this;
  }

  /// Enable long-term memory persistence
  AgentBuilder &persistent(bool enable = true) {
    cfg_.persist_memory = enable;
    return *this;
  }

  /// Build default ReActAgent
  std::shared_ptr<ReActAgent> create() {
    return os_->create_agent<ReActAgent>(std::move(cfg_));
  }

  /// Build custom Agent subclass
  template <typename AgentT, typename... Args>
  std::shared_ptr<AgentT> create(Args &&...args) {
    return os_->create_agent<AgentT>(std::move(cfg_),
                                      std::forward<Args>(args)...);
  }

  /// Access the underlying config for advanced customization
  AgentConfig &config() { return cfg_; }

private:
  AgentOS *os_;
  AgentConfig cfg_;
};

// ─────────────────────────────────────────────────────────────
// § SDK.3  AgentOSBuilder — Fluent System Construction
// ─────────────────────────────────────────────────────────────

class AgentOSBuilder {
public:
  /// Set OpenAI-compatible backend
  AgentOSBuilder &openai(std::string api_key,
                          std::string model = "gpt-4o-mini",
                          std::string base_url = "https://api.openai.com/v1") {
    api_key_ = std::move(api_key);
    model_ = std::move(model);
    base_url_ = std::move(base_url);
    backend_type_ = BackendType::OpenAI;
    return *this;
  }

  /// Use mock backend for testing
  AgentOSBuilder &mock() {
    backend_type_ = BackendType::Mock;
    return *this;
  }

  /// Use Ollama local backend
  AgentOSBuilder &ollama(std::string model = "llama3",
                          std::string base_url = "http://localhost:11434") {
    custom_backend_ = std::make_unique<kernel::OllamaBackend>(
        std::move(model), std::move(base_url));
    backend_type_ = BackendType::Custom;
    return *this;
  }

  /// Set Anthropic Claude backend
  AgentOSBuilder &anthropic(std::string api_key,
                            std::string model = "claude-sonnet-4-20250514") {
    custom_backend_ = std::make_unique<kernel::AnthropicBackend>(
        std::move(api_key), std::move(model));
    backend_type_ = BackendType::Custom;
    return *this;
  }

#ifdef AGENTOS_ENABLE_LLAMACPP
  /// Set llama.cpp local inference backend
  AgentOSBuilder &llamacpp(kernel::LlamaCppBackend::Config config) {
    custom_backend_ = std::make_unique<kernel::LlamaCppBackend>(std::move(config));
    backend_type_ = BackendType::Custom;
    return *this;
  }
#endif

  /// Use a custom backend
  AgentOSBuilder &backend(std::unique_ptr<kernel::ILLMBackend> b) {
    custom_backend_ = std::move(b);
    backend_type_ = BackendType::Custom;
    return *this;
  }

  /// Set scheduler thread count
  AgentOSBuilder &threads(uint32_t n) {
    cfg_.scheduler_threads = n;
    return *this;
  }

  /// Set tokens-per-minute rate limit
  AgentOSBuilder &tpm(uint32_t limit) {
    cfg_.tpm_limit = limit;
    return *this;
  }

  /// Set data directories (snapshots + long-term memory)
  AgentOSBuilder &data_dir(std::string dir) {
    cfg_.snapshot_dir = dir + "/snapshots";
    cfg_.ltm_dir = dir + "/ltm";
    return *this;
  }

  /// Set snapshot directory
  AgentOSBuilder &snapshot_dir(std::string dir) {
    cfg_.snapshot_dir = std::move(dir);
    return *this;
  }

  /// Set long-term memory directory
  AgentOSBuilder &ltm_dir(std::string dir) {
    cfg_.ltm_dir = std::move(dir);
    return *this;
  }

  /// Enable/disable security module
  AgentOSBuilder &security(bool enable) {
    cfg_.enable_security = enable;
    return *this;
  }

  /// Set log level
  AgentOSBuilder &log_level(LogLevel level) {
    log_level_ = level;
    return *this;
  }

  /// Set hot-reload config file path
  AgentOSBuilder &config_file(std::string path) {
    config_file_ = std::move(path);
    return *this;
  }

  /// Build the AgentOS instance
  std::unique_ptr<AgentOS> build() {
    // Apply log level
    if (log_level_) {
      Logger::instance().set_level(*log_level_);
    }

    // Auto-load API key if not explicitly set (for OpenAI backend)
    if (backend_type_ == BackendType::OpenAI && api_key_.empty()) {
      auto loaded = KeyLoader::load("", "OPENAI_API_KEY", "openai");
      if (loaded.has_value()) api_key_ = *loaded;
    }

    // Create backend
    std::unique_ptr<kernel::ILLMBackend> be;
    switch (backend_type_) {
    case BackendType::OpenAI:
      be = std::make_unique<kernel::OpenAIBackend>(api_key_, base_url_, model_);
      break;
    case BackendType::Mock:
      be = std::make_unique<kernel::MockLLMBackend>();
      break;
    case BackendType::Custom:
      be = std::move(custom_backend_);
      break;
    case BackendType::None:
      break;
    }

    if (!be) {
      throw std::runtime_error(
          "AgentOSBuilder: no backend configured. "
          "Call .openai(), .mock(), or .backend() before .build()");
    }

    auto os = std::make_unique<AgentOS>(std::move(be), cfg_);
    if (!config_file_.empty()) {
      os->configure_hot_reload(config_file_);
    }
    return os;
  }

  /// Access the underlying config for advanced customization
  AgentOS::Config &config() { return cfg_; }

private:
  enum class BackendType { None, OpenAI, Mock, Custom };
  BackendType backend_type_{BackendType::None};

  std::string api_key_;
  std::string model_{"gpt-4o-mini"};
  std::string base_url_{"https://api.openai.com/v1"};
  std::unique_ptr<kernel::ILLMBackend> custom_backend_;

  AgentOS::Config cfg_;
  std::optional<LogLevel> log_level_;
  std::string config_file_;
};

// ─────────────────────────────────────────────────────────────
// § SDK.4  Quickstart — One-call setup from environment
// ─────────────────────────────────────────────────────────────

/// Create an AgentOS from environment variables:
///   OPENAI_API_KEY, OPENAI_BASE_URL, OPENAI_MODEL
///   AGENTOS_DATA_DIR, AGENTOS_THREADS, AGENTOS_TPM
inline std::unique_ptr<AgentOS> quickstart() {
  auto env_or = [](const char *key, const char *def) -> std::string {
    const char *v = std::getenv(key);
    return v ? std::string(v) : std::string(def);
  };

  std::string api_key = env_or("OPENAI_API_KEY", "");
  if (api_key.empty()) {
    throw std::runtime_error(
        "quickstart: OPENAI_API_KEY environment variable not set");
  }

  AgentOSBuilder builder;
  builder.openai(api_key,
                  env_or("OPENAI_MODEL", "gpt-4o-mini"),
                  env_or("OPENAI_BASE_URL", "https://api.openai.com/v1"));

  std::string data_dir = env_or("AGENTOS_DATA_DIR", "");
  if (!data_dir.empty()) {
    builder.data_dir(data_dir);
  }

  std::string threads_str = env_or("AGENTOS_THREADS", "");
  if (!threads_str.empty()) {
    try {
      builder.threads(static_cast<uint32_t>(std::stoul(threads_str)));
    } catch (const std::exception &) { /* use default */ }
  }

  std::string tpm_str = env_or("AGENTOS_TPM", "");
  if (!tpm_str.empty()) {
    try {
      builder.tpm(static_cast<uint32_t>(std::stoul(tpm_str)));
    } catch (const std::exception &) { /* use default */ }
  }

  return builder.build();
}

/// Create a quickstart AgentOS with mock backend (for testing)
inline std::unique_ptr<AgentOS> quickstart_mock() {
  return AgentOSBuilder().mock().security(false).build();
}

// ─────────────────────────────────────────────────────────────
// § SDK.5  JSON Config — Load from JSON
// ─────────────────────────────────────────────────────────────

/// Create AgentOS from a JSON config object.
///
/// JSON schema:
/// {
///   "backend": "openai" | "mock",
///   "api_key": "sk-...",
///   "model": "gpt-4o-mini",
///   "base_url": "https://api.openai.com/v1",
///   "threads": 4,
///   "tpm_limit": 100000,
///   "data_dir": "/var/lib/agentos",
///   "snapshot_dir": "/tmp/snapshots",
///   "ltm_dir": "/tmp/ltm",
///   "security": true,
///   "log_level": "info"
/// }
inline std::unique_ptr<AgentOS> from_json(const nlohmann::json &j) {
  AgentOSBuilder builder;

  // Backend
  std::string backend_type = j.value("backend", "openai");
  if (backend_type == "mock") {
    builder.mock();
  } else {
    std::string api_key = j.value("api_key", "");
    if (api_key.empty()) {
      // Fallback to env var
      const char *env_key = std::getenv("OPENAI_API_KEY");
      if (env_key) api_key = env_key;
    }
    if (api_key.empty()) {
      throw std::runtime_error("from_json: api_key not set in config or OPENAI_API_KEY env");
    }
    builder.openai(api_key,
                    j.value("model", "gpt-4o-mini"),
                    j.value("base_url", "https://api.openai.com/v1"));
  }

  // System config
  if (j.contains("threads"))
    builder.threads(j["threads"].get<uint32_t>());
  if (j.contains("tpm_limit"))
    builder.tpm(j["tpm_limit"].get<uint32_t>());
  if (j.contains("data_dir"))
    builder.data_dir(j["data_dir"].get<std::string>());
  if (j.contains("snapshot_dir"))
    builder.snapshot_dir(j["snapshot_dir"].get<std::string>());
  if (j.contains("ltm_dir"))
    builder.ltm_dir(j["ltm_dir"].get<std::string>());
  if (j.contains("security"))
    builder.security(j["security"].get<bool>());

  // Log level
  if (j.contains("log_level")) {
    std::string ll = j["log_level"].get<std::string>();
    if (ll == "debug") builder.log_level(LogLevel::Debug);
    else if (ll == "info") builder.log_level(LogLevel::Info);
    else if (ll == "warn") builder.log_level(LogLevel::Warn);
    else if (ll == "error") builder.log_level(LogLevel::Error);
    else if (ll == "off") builder.log_level(LogLevel::Off);
  }

  return builder.build();
}

/// Load AgentOS config from a JSON file
inline std::unique_ptr<AgentOS> from_json_file(const std::string &path) {
  std::ifstream ifs(path);
  if (!ifs)
    throw std::runtime_error("from_json_file: cannot open " + path);
  nlohmann::json j;
  ifs >> j;
  return from_json(j);
}

// ─────────────────────────────────────────────────────────────
// § SDK.6  AgentOS::agent() — Fluent Agent Creation
// ─────────────────────────────────────────────────────────────

// Free function to start building an agent (alternative to method)
inline AgentBuilder make_agent(AgentOS &os, std::string name) {
  return AgentBuilder(&os, std::move(name));
}

} // namespace agentos
