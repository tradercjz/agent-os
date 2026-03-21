#pragma once
// ============================================================
// AgentOS :: Agent 基类 + AgentOS 系统门面
// ============================================================
#include <agentos/core/co_executor.hpp>
#include <agentos/core/hot_config.hpp>
#include <agentos/bus/agent_bus.hpp>
#include <agentos/bus/sqlite_audit_store.hpp>
#include <agentos/context/context.hpp>
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/memory/memory.hpp>
#include <agentos/memory/consolidator.hpp>
#include <agentos/scheduler/scheduler.hpp>
#include <agentos/security/security.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <agentos/tools/tool_learner.hpp>
#include <agentos/tracing.hpp>
#include <agentos/worktree/worktree_manager.hpp>
#include <atomic>
#include <cassert>
#include <concepts>
#include <functional>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace agentos {

class SubworkerRuntime;

// ─────────────────────────────────────────────────────────────
// § A.0  Middleware — Pre/Post Hooks for Agent Operations
// ─────────────────────────────────────────────────────────────

/// Hook context passed to middleware callbacks
struct HookContext {
  AgentId agent_id;
  std::string operation;      // "think", "act", "remember", "recall",
                              // "pre_tool_use", "post_tool_use", "stop"
  std::string input;          // operation input (user msg, tool name, etc.)
  Json args;                  // tool call arguments (for pre/post_tool_use)
  bool cancelled{false};      // set true in pre-hook to skip operation
  std::string cancel_reason;

  // Post-hook result injection (only for post_tool_use)
  tools::ToolResult* result{nullptr};  // mutable pointer to tool result
};

/// Middleware: pre/post callbacks around agent operations
struct Middleware {
  std::string name;
  std::function<void(HookContext &)> before;  // called before operation
  std::function<void(HookContext &)> after;   // called after operation
};

// ─────────────────────────────────────────────────────────────
// § A.1  AgentConfig — Agent 构建配置
// ─────────────────────────────────────────────────────────────

class AgentConfigBuilder;

struct AgentConfig {
  std::string name{};                     // Default: empty
  std::string role_prompt{};              // Default: empty (System prompt / 角色设定)
  std::string security_role{"standard"};  // RBAC 角色
  Priority priority{Priority::Normal};
  TokenCount context_limit{8192};
  std::vector<std::string> allowed_tools{}; // Default: empty (空 = 全部允许)
  bool persist_memory{false};             // 是否使用长期记忆
  worktree::IsolationMode isolation{worktree::IsolationMode::Thread};

  static AgentConfigBuilder builder();
};

class AgentConfigBuilder {
public:
  AgentConfigBuilder& name(std::string n) {
    cfg_.name = std::move(n);
    return *this;
  }
  AgentConfigBuilder& role_prompt(std::string p) {
    cfg_.role_prompt = std::move(p);
    return *this;
  }
  AgentConfigBuilder& security_role(std::string r) {
    cfg_.security_role = std::move(r);
    return *this;
  }
  AgentConfigBuilder& priority(Priority p) {
    cfg_.priority = p;
    return *this;
  }
  AgentConfigBuilder& context_limit(TokenCount l) {
    cfg_.context_limit = l;
    return *this;
  }
  AgentConfigBuilder& tools(std::vector<std::string> t) {
    cfg_.allowed_tools = std::move(t);
    return *this;
  }
  AgentConfigBuilder& persist_memory(bool p) {
    cfg_.persist_memory = p;
    return *this;
  }
  AgentConfigBuilder& isolation(worktree::IsolationMode m) {
    cfg_.isolation = m;
    return *this;
  }
  AgentConfig build() { return std::move(cfg_); }
private:
  AgentConfig cfg_;
};

inline AgentConfigBuilder AgentConfig::builder() { return AgentConfigBuilder{}; }

class AgentOS; // 前向声明

// ── Agent 基础接口 ───────────────────────────
class Agent : private NonCopyable {
public:
  Agent(AgentId id, AgentConfig cfg, AgentOS *os)
      : id_(id), config_(std::move(cfg)), os_(os) {
    assert(os_ != nullptr && "Agent must be created with a valid AgentOS pointer");
  }

  virtual ~Agent() {
    alive_->store(false, std::memory_order_release);
  }

  Agent(const Agent&)            = delete;
  Agent& operator=(const Agent&) = delete;
  Agent(Agent&&)                 = delete;
  Agent& operator=(Agent&&)      = delete;

  virtual void on_start() {}
  virtual void on_stop() {}

  virtual Result<std::string> run(std::string user_input) = 0;

  AgentId id() const { return id_; }
  const AgentConfig &config() const { return config_; }
  const std::filesystem::path& work_dir() const { return work_dir_; }

  void use(Middleware mw) {
    std::lock_guard lk(middleware_mu_);
    middleware_.push_back(std::move(mw));
  }

protected:
  AgentId id_;
  AgentConfig config_;
  AgentOS *os_; 
  std::shared_ptr<bus::Channel> channel_;
  std::vector<Middleware> middleware_;
  mutable std::shared_mutex middleware_mu_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::filesystem::path work_dir_{std::filesystem::current_path()};

  // Middleware utilities
  bool run_before_hooks(HookContext &ctx);
  void run_after_hooks(HookContext &ctx);

  friend class AgentOS;
  friend class SubworkerRuntime;
};

// ── AgentBase (CRTP) ───────────────────────────
// ── Agent Concepts ────────────────────────────
template <typename T>
concept AgentConcept = requires(T a, std::string s) {
  { a.run(s) } -> std::same_as<Result<std::string>>;
  requires std::derived_from<T, Agent>;
};

// ── AgentBase (CRTP) ───────────────────────────
template <typename Derived>
class AgentBase : public Agent {
public:
  AgentBase(AgentId id, AgentConfig cfg, AgentOS *os)
      : Agent(id, std::move(cfg), os) {
    // Moved check to constructor to avoid incomplete type issues during class definition
    static_assert(std::is_base_of_v<Agent, Derived>, "Derived must inherit from Agent");
  }

  using Agent::Agent;

  [[nodiscard]] Result<kernel::LLMResponse> think(std::string user_msg, kernel::ILLMBackend::TokenCallback cb = nullptr);
  [[nodiscard]] Result<tools::ToolResult> act(const kernel::ToolCallRequest &call);
  Result<std::string> remember(std::string content, float importance = 0.5f);
  Result<std::vector<memory::SearchResult>> recall(std::string_view query, size_t k = 5);
  bool send(AgentId target, std::string topic, std::string payload);
  std::optional<bus::BusMessage> recv(Duration timeout = Duration{3000});

  std::future<Result<std::string>> run_async(std::string user_input) {
    auto alive = alive_;
    return std::async(std::launch::async, [this, alive, input = std::move(user_input)]() mutable -> Result<std::string> {
      if (!alive->load(std::memory_order_acquire)) {
        return make_error(ErrorCode::InvalidArgument, "Agent destroyed before async task started");
      }
      return static_cast<Derived*>(this)->run(std::move(input));
    });
  }
};

// ── ReActAgent ──
class ReActAgent : public AgentBase<ReActAgent> {
public:
  using AgentBase<ReActAgent>::AgentBase;
  Result<std::string> run(std::string user_input) override;

protected:
  static constexpr int MAX_STEPS = 10;
};

// ─────────────────────────────────────────────────────────────
// § A.4  AgentOS — 系统总门面
// ─────────────────────────────────────────────────────────────

class AgentOS : private NonCopyable {
public:
  class ConfigBuilder;

  struct Config {
    uint32_t scheduler_threads{4};
    uint32_t tpm_limit{100000};
    std::string snapshot_dir{
        (std::filesystem::temp_directory_path() / "agentos_snapshots").string()};
    std::string ltm_dir{
        (std::filesystem::temp_directory_path() / "agentos_ltm").string()};
    bool enable_security{true};
    std::filesystem::path repo_root{std::filesystem::current_path()};
    std::filesystem::path worktree_base{".agentos/worktrees"};
    uint32_t max_worktrees{10};

    static ConfigBuilder builder();
  };

  class ConfigBuilder {
  public:
    ConfigBuilder& scheduler_threads(uint32_t n) {
      cfg_.scheduler_threads = n;
      return *this;
    }
    ConfigBuilder& tpm_limit(uint32_t n) {
      cfg_.tpm_limit = n;
      return *this;
    }
    ConfigBuilder& snapshot_dir(std::string dir) {
      cfg_.snapshot_dir = std::move(dir);
      return *this;
    }
    ConfigBuilder& ltm_dir(std::string dir) {
      cfg_.ltm_dir = std::move(dir);
      return *this;
    }
    ConfigBuilder& enable_security(bool e) {
      cfg_.enable_security = e;
      return *this;
    }
    ConfigBuilder& repo_root(std::filesystem::path p) {
      cfg_.repo_root = std::move(p);
      return *this;
    }
    ConfigBuilder& worktree_base(std::filesystem::path p) {
      cfg_.worktree_base = std::move(p);
      return *this;
    }
    ConfigBuilder& max_worktrees(uint32_t n) {
      cfg_.max_worktrees = n;
      return *this;
    }
    Config build() { return std::move(cfg_); }
  private:
    Config cfg_;
  };

  explicit AgentOS(std::unique_ptr<kernel::ILLMBackend> backend,
                   Config cfg)
      : config_(cfg) { // 先拷贝，后续用 config_ 成员，避免 cfg 被移走后失效
    // FIX #23: Validate config before constructing subsystems (fail-fast at startup)
    auto validate = [&](const Config &c) -> void {
        std::vector<std::string> errors;
        if (c.scheduler_threads == 0)
            errors.push_back("scheduler_threads must be > 0");
        if (c.tpm_limit == 0)
            errors.push_back("tpm_limit must be > 0");
        if (c.snapshot_dir.empty())
            errors.push_back("snapshot_dir must not be empty");
        if (c.ltm_dir.empty())
            errors.push_back("ltm_dir must not be empty");

        unsigned int hw_threads = std::thread::hardware_concurrency();
        if (hw_threads == 0) hw_threads = 4;  // Fallback if detection fails
        unsigned int max_allowed = std::max(4u, hw_threads * 4);  // Allow 4x oversubscription max
        if (c.scheduler_threads > max_allowed)
            errors.push_back(fmt::format("scheduler_threads ({}) exceeds 4x hardware concurrency ({}). "
                                       "Max allowed: {}", c.scheduler_threads, hw_threads, max_allowed));

        if (!errors.empty()) {
            std::string error_msg = "AgentOS config validation failed:";
            for (const auto &err : errors) {
                error_msg += "\n  - " + err;
            }
            throw std::invalid_argument(error_msg);
        }
    };
    validate(config_);

    // Validate that directories can be created
    std::error_code ec;
    std::filesystem::create_directories(config_.snapshot_dir, ec);
    if (ec) {
      throw std::invalid_argument(
          fmt::format("AgentOS: cannot create snapshot_dir '{}': {}",
                      config_.snapshot_dir, ec.message()));
    }
    std::filesystem::create_directories(config_.ltm_dir, ec);
    if (ec) {
      throw std::invalid_argument(
          fmt::format("AgentOS: cannot create ltm_dir '{}': {}",
                      config_.ltm_dir, ec.message()));
    }

    // Initialize subsystems
    kernel_ = std::make_unique<kernel::LLMKernel>(std::move(backend),
                                                  config_.tpm_limit);
    scheduler_ = std::make_unique<scheduler::Scheduler>(
        scheduler::SchedulerPolicy::Priority, config_.scheduler_threads);
    ctx_mgr_ = std::make_unique<context::ContextManager>(config_.snapshot_dir);
    memory_ = std::make_unique<memory::MemorySystem>(config_.ltm_dir);
    tool_mgr_ = std::make_unique<tools::ToolManager>(memory_.get());
    security_ = config_.enable_security
                    ? std::make_unique<security::SecurityManager>()
                    : nullptr;
    // Create audit store for persistent bus message logging
    std::shared_ptr<bus::IAuditStore> audit_store;
    {
        auto audit_db = std::filesystem::path(config_.snapshot_dir) / "audit.db";
        try {
            audit_store = std::make_shared<bus::SqliteAuditStore>(audit_db.string());
        } catch (const std::exception& e) {
            LOG_WARN(fmt::format("Audit store init failed: {} — running without persistence", e.what()));
        }
    }
    bus_ = std::make_unique<bus::AgentBus>(security_.get(), std::move(audit_store));
    consolidator_ = std::make_unique<memory::MemoryConsolidator>(*memory_);
    consolidator_->start();
    tracer_ = std::make_unique<tracing::Tracer>();
    tool_learner_ = std::make_unique<tools::ToolLearner>(*kernel_);
    worktree_mgr_ = std::make_unique<worktree::WorktreeManager>(
        worktree::WorktreeConfig{config_.repo_root, config_.worktree_base,
                                  config_.max_worktrees});
    co_executor_ = std::make_unique<CoExecutor>(2);
    scheduler_->start();
  }

  ~AgentOS() {
    os_alive_->store(false, std::memory_order_release);
    if (co_executor_) co_executor_->stop();
    consolidator_->stop();
    scheduler_->shutdown();
  }

  // ── Agent 工厂 ─────────────────────────────────────────
  template <AgentConcept AgentT = ReActAgent, typename... Args>
  std::shared_ptr<AgentT> create_agent(AgentConfig cfg, Args &&...args) {
    AgentId id = next_agent_id_++;
    auto agent = std::make_shared<AgentT>(id, std::move(cfg), this,
                                          std::forward<Args>(args)...);
    agent->channel_ = bus_->register_agent(id);

    // 配置安全角色
    if (security_) {
      security_->grant(id, agent->config().security_role);
    }

    // 初始化上下文窗口
    auto &win = ctx_mgr_->get_window(id, agent->config().context_limit);
    if (!agent->config().role_prompt.empty()) {
      win.try_add(kernel::Message::system(agent->config().role_prompt));
    }

    {
      std::lock_guard lk(agents_mu_);
      agents_[id] = agent;
    }

    // Worktree isolation: auto-create worktree for this agent
    if (agent->config().isolation == worktree::IsolationMode::Worktree) {
      std::string wt_name = agent->config().name.empty()
          ? fmt::format("agent-{}", id) : agent->config().name;
      auto wt_res = worktree_mgr_->create(wt_name);
      if (wt_res) {
        agent->work_dir_ = wt_res->path;
      } else {
        LOG_WARN(fmt::format("Failed to create worktree for agent {}: {}",
                             id, wt_res.error().message));
      }
    }

    consolidator_->register_agent(id);
    agent->on_start();
    return agent;
  }

  void destroy_agent(AgentId id) {
    consolidator_->on_agent_destroyed(id);
    std::lock_guard lk(agents_mu_);
    auto it = agents_.find(id);
    if (it == agents_.end())
      return;

    // Worktree cleanup
    if (it->second->config().isolation == worktree::IsolationMode::Worktree) {
      std::string wt_name = it->second->config().name.empty()
          ? fmt::format("agent-{}", id) : it->second->config().name;
      (void)worktree_mgr_->remove(wt_name, /*force=*/true);
    }

    it->second->on_stop();
    bus_->unregister_agent(id);
    ctx_mgr_->clear(id);
    agents_.erase(it);
  }

  // ── 提交异步任务 ────────────────────────────────────────
  [[nodiscard]] Result<TaskId> submit_task(std::string name, std::function<void()> work,
                     AgentId agent_id = 0, Priority priority = Priority::Normal,
                     std::vector<TaskId> deps = {}) {
    auto task = std::make_shared<scheduler::AgentTaskDescriptor>();
    task->id = scheduler::Scheduler::new_task_id();
    task->agent_id = agent_id;
    task->name = std::move(name);
    task->work = std::move(work);
    task->priority = priority;
    task->depends_on = std::move(deps);

    return scheduler_->submit(task);
  }

  // ── 子系统访问器 ────────────────────────────────────────
  kernel::LLMKernel &kernel() { return *kernel_; }
  [[nodiscard]] const kernel::LLMKernel& kernel() const noexcept { return *kernel_; }

  scheduler::Scheduler &scheduler() { return *scheduler_; }
  [[nodiscard]] const scheduler::Scheduler& scheduler() const noexcept { return *scheduler_; }

  context::ContextManager &ctx() { return *ctx_mgr_; }
  [[nodiscard]] const context::ContextManager& ctx() const noexcept { return *ctx_mgr_; }

  memory::MemorySystem &memory() { return *memory_; }
  [[nodiscard]] const memory::MemorySystem& memory() const noexcept { return *memory_; }

  tools::ToolManager &tools() { return *tool_mgr_; }
  [[nodiscard]] const tools::ToolManager& tools() const noexcept { return *tool_mgr_; }

  security::SecurityManager *security() { return security_.get(); }
  bus::AgentBus &bus() { return *bus_; }
  memory::MemoryConsolidator &consolidator() { return *consolidator_; }
  tracing::Tracer &tracer() { return *tracer_; }
  tools::ToolLearner &tool_learner() { return *tool_learner_; }
  worktree::WorktreeManager &worktree_mgr() { return *worktree_mgr_; }
  CoExecutor& co_executor() { return *co_executor_; }
  const Config& config() const { return config_; }

  // ── Agent 数量查询 ──────────────────────────────────────
  size_t agent_count() const {
    std::lock_guard lk(agents_mu_);
    return agents_.size();
  }

  // ── 查找 Agent ────────────────────────────────────────────
  /// THREAD-SAFETY: Returns shared_ptr with incremented refcount under lock.
  /// Caller can safely use the returned pointer even if destroy_agent() runs
  /// concurrently—the Agent stays alive as long as the shared_ptr exists.
  std::shared_ptr<Agent> find_agent(AgentId id) const {
    std::lock_guard lk(agents_mu_);
    auto it = agents_.find(id);
    return it != agents_.end() ? it->second : nullptr;
  }

  // ── 注册工具快捷方法 ──────────────────────────────────────
  template <typename Fn>
  void register_tool(tools::ToolSchema schema, Fn &&handler) {
    tool_mgr_->registry().register_fn(std::move(schema),
                                       std::forward<Fn>(handler));
  }

  // ── 系统状态摘要 ────────────────────────────────────────
  std::string status() const {
    std::lock_guard lk(agents_mu_);
    return fmt::format(
        "AgentOS | agents={} | total_tokens={} | total_requests={}",
        agents_.size(), kernel_->metrics().total_tokens.load(),
        kernel_->metrics().total_requests.load());
  }

  // ── Health Check ──────────────────────────────────────────
  struct HealthStatus {
    bool healthy{true};
    bool scheduler_running{false};
    bool backend_available{false};
    size_t active_agents{0};
    uint64_t total_requests{0};
    uint64_t total_errors{0};
    std::string model;

    std::string to_json() const {
      nlohmann::json j;
      j["healthy"] = healthy;
      j["scheduler"] = scheduler_running;
      j["backend"] = backend_available;
      j["agents"] = active_agents;
      j["requests"] = total_requests;
      j["errors"] = total_errors;
      j["model"] = model;
      return j.dump();
    }
  };

  HealthStatus health() const {
    HealthStatus h;
    h.scheduler_running = scheduler_->is_running();
    h.backend_available = true; // backend is constructed if we reach here
    h.model = kernel_->model_name();
    h.total_requests = kernel_->metrics().total_requests.load();
    h.total_errors = kernel_->metrics().errors.load();
    {
      std::lock_guard lk(agents_mu_);
      h.active_agents = agents_.size();
    }
    h.healthy = h.scheduler_running && h.backend_available;
    return h;
  }

  // ── HotConfig integration ─────────────────────────────────
  void configure_hot_reload(const std::string& config_path) {
    hot_config_ = std::make_unique<HotConfig>(config_path);
    hot_config_->on_change("tpm_limit", [this](const std::string&, const nlohmann::json& v) {
      kernel_->rate_limiter().set_rate(v.get<size_t>());
    });
    hot_config_->on_change("log_level", [](const std::string&, const nlohmann::json& v) {
      auto s = v.get<std::string>();
      LogLevel level = LogLevel::Warn;
      if (s == "debug") level = LogLevel::Debug;
      else if (s == "info") level = LogLevel::Info;
      else if (s == "warn") level = LogLevel::Warn;
      else if (s == "error") level = LogLevel::Error;
      else if (s == "off") level = LogLevel::Off;
      Logger::instance().set_level(level);
    });
    (void)hot_config_->reload();
    hot_config_->start_watching();
  }

  HotConfig* hot_config() { return hot_config_.get(); }

  // ── Graceful shutdown with task draining ───────────────────
  void graceful_shutdown(Duration timeout = Duration{10000}) {
    // 1. Stop accepting new agents
    // 2. Wait for pending tasks to complete
    if (!scheduler_->drain(timeout)) {
      LOG_WARN("AgentOS: graceful_shutdown drain timed out, forcing shutdown");
    }
    // 3. Shutdown scheduler threads
    scheduler_->shutdown();
  }

private:
  Config config_;
  std::unique_ptr<kernel::LLMKernel> kernel_;
  std::unique_ptr<scheduler::Scheduler> scheduler_;
  std::unique_ptr<context::ContextManager> ctx_mgr_;
  std::unique_ptr<memory::MemorySystem> memory_;
  std::unique_ptr<tools::ToolManager> tool_mgr_;
  std::unique_ptr<security::SecurityManager> security_;
  std::unique_ptr<bus::AgentBus> bus_;
  std::unique_ptr<memory::MemoryConsolidator> consolidator_;
  std::unique_ptr<tracing::Tracer> tracer_;
  std::unique_ptr<tools::ToolLearner> tool_learner_;
  std::unique_ptr<worktree::WorktreeManager> worktree_mgr_;
  std::unique_ptr<CoExecutor> co_executor_;
  std::unique_ptr<HotConfig> hot_config_;
  std::shared_ptr<std::atomic<bool>> os_alive_ = std::make_shared<std::atomic<bool>>(true);

  mutable std::mutex agents_mu_;
  std::unordered_map<AgentId, std::shared_ptr<Agent>> agents_;
  std::atomic<AgentId> next_agent_id_{1};
};

// ─────────────────────────────────────────────────────────────
// § A.5  AgentBase 便捷方法实现（需要 AgentOS 完整定义）
// ─────────────────────────────────────────────────────────────

template <typename Derived>
Result<kernel::LLMResponse>
AgentBase<Derived>::think(std::string user_msg, kernel::ILLMBackend::TokenCallback cb) {
  AgentOS *os = this->os_;
  if (!os) return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
  if (user_msg.empty()) return make_error(ErrorCode::InvalidArgument, "think: user_msg must not be empty");

  HookContext hook_ctx{this->id_, "think", user_msg, {}, false, {}, nullptr};
  if (!this->run_before_hooks(hook_ctx)) return make_error(ErrorCode::Cancelled, hook_ctx.cancel_reason);

  os->ctx().append(this->id_, kernel::Message::user(user_msg));

  kernel::LLMRequest req;
  req.agent_id = this->id_;
  req.priority = this->config_.priority;
  auto &win = os->ctx().get_window(this->id_, this->config_.context_limit);
  req.messages.assign(win.messages().begin(), win.messages().end());

  std::string tj = os->tools().tools_json(this->config_.allowed_tools);
  if (!tj.empty() && tj != "[]") req.tools_json = std::move(tj);

  auto result = cb ? os->kernel().stream_infer(req, std::move(cb))
                   : os->kernel().infer(req);
  if (result) {
    auto m = kernel::Message::assistant(result->content);
    if (result->wants_tool_call()) m.tool_calls = result->tool_calls;
    os->ctx().append(this->id_, std::move(m));
  }
  this->run_after_hooks(hook_ctx);
  return result;
}

template <typename Derived>
Result<tools::ToolResult>
AgentBase<Derived>::act(const kernel::ToolCallRequest &call) {
  if (!this->os_) return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");

  HookContext act_ctx{this->id_, "act", call.name, {}, false, {}, nullptr};
  if (!this->run_before_hooks(act_ctx)) return make_error(ErrorCode::Cancelled, act_ctx.cancel_reason);

  // Parse args for tool-level hooks
  Json parsed_args;
  try { parsed_args = Json::parse(call.args_json); } catch (...) { parsed_args = Json::object(); }

  // Pre-tool-use hook: can cancel specific tool calls
  HookContext pre_tool_ctx{this->id_, "pre_tool_use", call.name, parsed_args, false, {}, nullptr};
  if (!this->run_before_hooks(pre_tool_ctx)) return make_error(ErrorCode::Cancelled, pre_tool_ctx.cancel_reason);

  if (this->os_->security()) {
    auto check = this->os_->security()->authorize_tool(this->id_, call.name, call.args_json);
    if (!check) return make_error(check.error().code, check.error().message);
  }

  // Tool Learning: inject prompt hints and apply parameter fixes
  auto& learner = this->os_->tool_learner();
  std::string corrected_args = call.args_json;
  if (learner.enabled()) {
    auto hints = learner.get_prompt_hints(call.name);
    if (!hints.empty()) {
      this->os_->ctx().append(this->id_, kernel::Message::system(hints));
    }
    corrected_args = learner.apply_param_fixes(call.name, call.args_json);
  }

  kernel::ToolCallRequest corrected_call{call.id, call.name, corrected_args};
  auto result = this->os_->tools().dispatch(corrected_call);

  // Tool Learning: record failure and analyze
  if (!result.success && learner.enabled()) {
    tools::ToolFailureRecord record{call.name, call.args_json, result.error, now(), this->id_};
    learner.record_failure(record);
    learner.analyze_failure(record);
  }

  // Post-tool-use hook: can mutate result
  HookContext post_tool_ctx{this->id_, "post_tool_use", call.name, parsed_args, false, {}, &result};
  this->run_after_hooks(post_tool_ctx);

  this->run_after_hooks(act_ctx);
  return result;
}

template <typename Derived>
Result<std::string> AgentBase<Derived>::remember(std::string content, float importance) {
  if (!this->os_) return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
  kernel::EmbeddingRequest req{{content}, ""};
  auto emb_res = this->os_->kernel().embed(req);
  memory::Embedding emb;
  if (emb_res && !emb_res->embeddings.empty()) {
    emb = std::move(emb_res->embeddings[0]);
  }
  return this->os_->memory().remember(std::move(content), emb, std::to_string(this->id_), importance);
}

template <typename Derived>
Result<std::vector<memory::SearchResult>>
AgentBase<Derived>::recall(std::string_view query, size_t k) {
  if (!this->os_) return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
  kernel::EmbeddingRequest req{{std::string(query)}, ""};
  auto emb_res = this->os_->kernel().embed(req);
  if (!emb_res) return make_error(emb_res.error().code, "recall: embedding failed — " + emb_res.error().message);
  
  memory::Embedding emb;
  if (!emb_res->embeddings.empty()) emb = std::move(emb_res->embeddings[0]);
  return this->os_->memory().recall(emb, {}, k);
}

template <typename Derived>
bool AgentBase<Derived>::send(AgentId target, std::string topic, std::string payload) {
  if (!this->os_) return false;
  return this->os_->bus().send(bus::BusMessage::make_request(this->id_, target, std::move(topic), std::move(payload)));
}

template <typename Derived>
std::optional<bus::BusMessage> AgentBase<Derived>::recv(Duration timeout) {
  // channel_ is already initialized in AgentOS::create_agent via register_agent
  return this->channel_ ? this->channel_->recv(timeout) : std::nullopt;
}

// ── Middleware Utils ─────────────────────
inline bool Agent::run_before_hooks(HookContext &ctx) {
  std::vector<Middleware> mw_snapshot;
  {
    std::shared_lock lk(middleware_mu_);
    mw_snapshot = middleware_;
  }
  for (auto &mw : mw_snapshot) {
    if (mw.before) mw.before(ctx);
    if (ctx.cancelled) return false;
  }
  return true;
}

inline void Agent::run_after_hooks(HookContext &ctx) {
  std::vector<Middleware> mw_snapshot;
  {
    std::shared_lock lk(middleware_mu_);
    mw_snapshot = middleware_;
  }
  for (auto &mw : mw_snapshot) {
    if (mw.after) mw.after(ctx);
  }
}

// ─────────────────────────────────────────────────────────────
// § A.6  ReActAgent::run() — ReAct 循环实现
// ─────────────────────────────────────────────────────────────

inline Result<std::string> ReActAgent::run(std::string user_input) {
  auto recall_result = this->recall(user_input, 3);
  if (recall_result && !recall_result->empty()) {
    std::string mem_ctx = "相关记忆：\n";
    for (auto &sr : *recall_result) mem_ctx += "- " + sr.entry.content + "\n";
    this->os_->ctx().append(this->id_, kernel::Message::system(mem_ctx));
  }

  for (int step = 0; step < MAX_STEPS; ++step) {
    auto resp = this->think(step == 0 ? user_input : "[继续]");
    if (!resp) return make_unexpected(resp.error());

    if (!resp->wants_tool_call()) {
      // Stop hook: middleware can force the agent to continue
      HookContext stop_ctx{this->id_, "stop", resp->content, {}, false, {}, nullptr};
      if (!this->run_before_hooks(stop_ctx)) {
        // cancelled = true means "don't stop, keep going"
        continue;
      }
      (void)this->remember(fmt::format("Q: {} → A: {}", user_input, resp->content), 0.6f);
      return resp->content;
    }

    for (auto &tc : resp->tool_calls) {
      auto tool_result = this->act(tc);
      std::string obs = tool_result ? (tool_result->success ? tool_result->output : "工具执行失败: " + tool_result->error)
                                    : "工具调用被拒绝: " + tool_result.error().message;

      kernel::Message obs_msg;
      obs_msg.role = kernel::Role::Tool;
      obs_msg.content = std::move(obs);
      obs_msg.tool_call_id = tc.id;
      obs_msg.name = tc.name;
      this->os_->ctx().append(this->id_, std::move(obs_msg));
    }
  }

  return make_error(ErrorCode::Timeout, "ReActAgent: exceeded max_steps");
}

inline AgentOS::ConfigBuilder AgentOS::Config::builder() { return AgentOS::ConfigBuilder{}; }

} // namespace agentos
