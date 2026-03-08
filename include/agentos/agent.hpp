#pragma once
// ============================================================
// AgentOS :: Agent 基类 + AgentOS 系统门面
// ============================================================
#include <agentos/bus/agent_bus.hpp>
#include <agentos/context/context.hpp>
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/memory/memory.hpp>
#include <agentos/scheduler/scheduler.hpp>
#include <agentos/security/security.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace agentos {

// ─────────────────────────────────────────────────────────────
// § A.0  Middleware — Pre/Post Hooks for Agent Operations
// ─────────────────────────────────────────────────────────────

/// Hook context passed to middleware callbacks
struct HookContext {
  AgentId agent_id;
  std::string operation;      // "think", "act", "remember", "recall"
  std::string input;          // operation input (user msg, tool name, etc.)
  bool cancelled{false};      // set true in pre-hook to skip operation
  std::string cancel_reason;
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

struct AgentConfig {
  std::string name{};                     // Default: empty
  std::string role_prompt{};              // Default: empty (System prompt / 角色设定)
  std::string security_role{"standard"};  // RBAC 角色
  Priority priority{Priority::Normal};
  TokenCount context_limit{8192};
  std::vector<std::string> allowed_tools{}; // Default: empty (空 = 全部允许)
  bool persist_memory{false};             // 是否使用长期记忆
};

// ─────────────────────────────────────────────────────────────
// § A.2  AgentRuntime — Agent 运行时状态
// ─────────────────────────────────────────────────────────────

class AgentOS; // 前向声明

class Agent : private NonCopyable {
public:
  Agent(AgentId id, AgentConfig cfg, AgentOS *os)
      : id_(id), config_(std::move(cfg)), os_(os) {}

  virtual ~Agent() {
    // Signal async tasks that agent is being destroyed
    *alive_ = false;
  }

  // Explicitly deleted to prevent accidental copies.
  // Rationale: Agents maintain unique identity (AgentId), own async state,
  // and are lifecycle-managed via shared_ptr in AgentOS::agents_ map.
  // Copying an Agent would duplicate the AgentId and corrupt the registry.
  Agent(const Agent&)            = delete;
  Agent& operator=(const Agent&) = delete;
  Agent(Agent&&)                 = delete;
  Agent& operator=(Agent&&)      = delete;

  // ── 生命周期钩子（子类可重写）───────────────────────────
  virtual void on_start() {}
  virtual void on_stop() {}

  // ── 核心执行循环（ReAct 范式：Think → Act → Observe）──
  // 子类实现具体业务逻辑
  virtual Result<std::string> run(std::string user_input) = 0;

  // ── 便捷接口（通过 AgentOS 调用各子系统）────────────────
  [[nodiscard]] Result<kernel::LLMResponse>
  think(std::string user_msg, kernel::ILLMBackend::TokenCallback cb = nullptr);
  [[nodiscard]] Result<tools::ToolResult> act(const kernel::ToolCallRequest &call);
  Result<std::string> remember(std::string content, float importance = 0.5f);
  /// \param query Query string. Internally copied on first use, so the view
  /// need not remain valid beyond this call.
  Result<std::vector<memory::SearchResult>> recall(std::string_view query,
                                                   size_t k = 5);
  bool send(AgentId target, std::string topic, std::string payload);
  std::optional<bus::BusMessage> recv(Duration timeout = Duration{3000});

  // ── 异步执行 ────────────────────────────────────────────────
  /// Run agent asynchronously, returning a future for the result
  /// PRECONDITION: The caller must ensure this Agent's lifetime exceeds the
  /// returned future. Capturing `this` by raw pointer means undefined behavior
  /// if the Agent is destroyed before future.get() is called.
  /// Recommended usage: only call run_async() on Agents managed via shared_ptr
  /// and keep the shared_ptr alive until the future completes.
  ///
  /// NOTE: This implementation detects the common case where the Agent is destroyed
  /// before the async task starts by using an alive_ flag. However, this does NOT
  /// prevent use-after-free if the Agent is destroyed mid-execution. It is still
  /// the caller's responsibility to ensure the Agent lifetime outlives the future.
  std::future<Result<std::string>> run_async(std::string user_input) {
    auto alive = alive_;  // Capture shared_ptr to alive flag
    return std::async(std::launch::async,
                      [this, alive, input = std::move(user_input)]() mutable -> Result<std::string> {
                        if (!alive->load(std::memory_order_acquire)) {
                          return make_error(ErrorCode::InvalidArgument,
                                           "Agent destroyed before async task started");
                        }
                        return this->run(std::move(input));
                      });
  }

  // ── Middleware ──────────────────────────────────────────────
  /// Thread-safe. Can be called before or after agent starts processing.
  /// Hooks registered after agent.run() has started will be visible to
  /// subsequent requests (not the currently running one).
  void use(Middleware mw) {
    std::unique_lock lk(middleware_mu_);
    middleware_.push_back(std::move(mw));
  }

  AgentId id() const { return id_; }
  const AgentConfig &config() const { return config_; }

protected:
  AgentId id_;
  AgentConfig config_;
  AgentOS *os_; // 非拥有指针，指向父系统
  std::shared_ptr<bus::Channel> channel_;
  std::vector<Middleware> middleware_;
  mutable std::shared_mutex middleware_mu_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

  // Run middleware hooks; returns true if operation should proceed
  bool run_before_hooks(HookContext &ctx) {
    // Take a snapshot under shared lock to avoid holding lock during callbacks
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
  void run_after_hooks(HookContext &ctx) {
    // Take a snapshot under shared lock to avoid holding lock during callbacks
    std::vector<Middleware> mw_snapshot;
    {
      std::shared_lock lk(middleware_mu_);
      mw_snapshot = middleware_;
    }
    for (auto &mw : mw_snapshot) {
      if (mw.after) mw.after(ctx);
    }
  }

  friend class AgentOS;
};

// ─────────────────────────────────────────────────────────────
// § A.3  ReActAgent — 通用 ReAct 循环实现
// ─────────────────────────────────────────────────────────────

class ReActAgent : public Agent {
public:
  ReActAgent(AgentId id, AgentConfig cfg, AgentOS *os)
      : Agent(id, std::move(cfg), os) {}

  Result<std::string> run(std::string user_input) override;

private:
  static constexpr int MAX_STEPS = 10; // 防止无限循环
};

// ─────────────────────────────────────────────────────────────
// § A.4  AgentOS — 系统总门面
// ─────────────────────────────────────────────────────────────

class AgentOS : private NonCopyable {
public:
  struct Config {
    uint32_t scheduler_threads{4};
    uint32_t tpm_limit{100000};
    std::string snapshot_dir{
        (std::filesystem::temp_directory_path() / "agentos_snapshots").string()};
    std::string ltm_dir{
        (std::filesystem::temp_directory_path() / "agentos_ltm").string()};
    bool enable_security{true};
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
    bus_ = std::make_unique<bus::AgentBus>(security_.get());
    scheduler_->start();
  }

  ~AgentOS() { scheduler_->shutdown(); }

  // ── Agent 工厂 ─────────────────────────────────────────
  template <typename AgentT = ReActAgent, typename... Args>
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

    agent->on_start();
    return agent;
  }

  void destroy_agent(AgentId id) {
    std::lock_guard lk(agents_mu_);
    auto it = agents_.find(id);
    if (it == agents_.end())
      return;
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

  // ── Graceful shutdown with task draining ───────────────────
  void graceful_shutdown(Duration timeout = Duration{10000}) {
    // 1. Stop accepting new agents
    // 2. Wait for pending tasks to complete
    scheduler_->drain(timeout);
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

  mutable std::mutex agents_mu_;
  std::unordered_map<AgentId, std::shared_ptr<Agent>> agents_;
  std::atomic<AgentId> next_agent_id_{1};
};

// ─────────────────────────────────────────────────────────────
// § A.5  Agent 便捷方法实现（需要 AgentOS 完整定义）
// ─────────────────────────────────────────────────────────────

inline Result<kernel::LLMResponse>
Agent::think(std::string user_msg, kernel::ILLMBackend::TokenCallback cb) {
  // THREAD-SAFETY NOTE: os_ capture is not atomic. think() must not be called
  // concurrently with detach()/destroy operations. Users are responsible for
  // ensuring agent lifetime outlives all think() calls.
  AgentOS *os = os_;
  if (!os)
    return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");

  // Middleware: before think
  HookContext hook_ctx{id_, "think", user_msg, false, {}};
  if (!run_before_hooks(hook_ctx))
    return make_error(ErrorCode::Cancelled, hook_ctx.cancel_reason);

  // 追加到上下文
  os->ctx().append(id_, kernel::Message::user(user_msg));

  // 构建请求
  kernel::LLMRequest req;
  req.agent_id = id_;
  req.priority = config_.priority;
  auto &win = os->ctx().get_window(id_, config_.context_limit);
  req.messages.clear();
  req.messages.reserve(win.messages().size());
  for (const auto &m : win.messages()) {
    req.messages.push_back(m);
  }

  // 将工具 Schema 注入请求（供真实 LLM function calling 使用）
  if (!config_.allowed_tools.empty()) {
    req.tool_names = config_.allowed_tools;
    // 生成 OpenAI function-calling 格式的 tools 数组
    std::string tj = os->tools().tools_json(config_.allowed_tools);
    if (!tj.empty() && tj != "[]")
      req.tools_json = tj;
  } else {
    // allowed_tools 为空表示允许全部工具
    std::string tj = os->tools().tools_json({});
    if (!tj.empty() && tj != "[]")
      req.tools_json = tj;
  }

  auto result = cb ? os->kernel().stream_infer(req, std::move(cb))
                   : os->kernel().infer(req);
  if (result) {
    // 追加 assistant 回复到上下文，并携带 tool_calls（如果有）
    auto m = kernel::Message::assistant(result->content);
    if (result->wants_tool_call()) {
      m.tool_calls = result->tool_calls;
    }
    os->ctx().append(id_, std::move(m));
  }
  run_after_hooks(hook_ctx);
  return result;
}

inline Result<tools::ToolResult>
Agent::act(const kernel::ToolCallRequest &call) {
  if (!os_)
    return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");

  // Middleware: before act
  HookContext act_ctx{id_, "act", call.name, false, {}};
  if (!run_before_hooks(act_ctx))
    return make_error(ErrorCode::Cancelled, act_ctx.cancel_reason);

  // ECL 安全检查
  if (os_->security()) {
    auto check =
        os_->security()->authorize_tool(id_, call.name, call.args_json);
    if (!check) {
      return make_error(check.error().code, check.error().message);
    }
  }
  auto result = os_->tools().dispatch(call);
  run_after_hooks(act_ctx);
  return result;
}

inline Result<std::string> Agent::remember(std::string content,
                                           float importance) {
  if (!os_)
    return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
  kernel::EmbeddingRequest req{{content}, ""};
  auto emb_res = os_->kernel().embed(req);
  memory::Embedding emb;
  if (emb_res && !emb_res->embeddings.empty()) {
    emb = std::move(emb_res->embeddings[0]);
  } else if (!emb_res) {
    LOG_WARN(fmt::format("Agent {}: embedding failed for remember(), storing without vector", id_));
  }
  return os_->memory().remember(std::move(content), emb, std::to_string(id_),
                                importance);
}

inline Result<std::vector<memory::SearchResult>>
Agent::recall(std::string_view query, size_t k) {
  if (!os_)
    return make_error(ErrorCode::InvalidArgument, "Agent not attached to AgentOS");
  if (query.empty())
    return make_error(ErrorCode::InvalidArgument, "recall: query must not be empty");
  kernel::EmbeddingRequest req{{std::string(query)}, ""};
  auto emb_res = os_->kernel().embed(req);
  memory::Embedding emb;
  if (emb_res && !emb_res->embeddings.empty()) {
    emb = std::move(emb_res->embeddings[0]);
  } else if (!emb_res) {
    // Embedding 失败时退化为空向量检索（线性扫描），不再静默
    return make_error(emb_res.error().code,
                      "recall: embedding failed — " + emb_res.error().message);
  }
  return os_->memory().recall(emb, {}, k);
}

inline bool Agent::send(AgentId target, std::string topic,
                        std::string payload) {
  if (!os_) return false;
  return os_->bus().send(bus::BusMessage::make_request(id_, target, std::move(topic),
                                                       std::move(payload)));
}

inline std::optional<bus::BusMessage> Agent::recv(Duration timeout) {
  if (!channel_)
    return std::nullopt;
  return channel_->recv(timeout);
}

// ─────────────────────────────────────────────────────────────
// § A.6  ReActAgent::run() — ReAct 循环实现
// ─────────────────────────────────────────────────────────────

inline Result<std::string> ReActAgent::run(std::string user_input) {
  // 先从记忆中检索相关上下文
  // Recall with single retry on transient failure
  auto recall_result = recall(user_input, 3);
  if (!recall_result) {
    LOG_WARN(fmt::format("[ReActAgent] recall failed (attempt 1): {}",
                         recall_result.error().message));
    // Single retry after brief backoff for transient errors
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    recall_result = recall(user_input, 3);
    if (!recall_result) {
      LOG_WARN(fmt::format("[ReActAgent] recall failed (attempt 2): {} — continuing without memory",
                           recall_result.error().message));
    }
  }

  if (recall_result && !recall_result->empty()) {
    std::string mem_ctx = "相关记忆：\n";
    for (auto &sr : *recall_result) {
      mem_ctx += "- " + sr.entry.content + "\n";
    }
    os_->ctx().append(id_, kernel::Message::system(mem_ctx));
  }

  for (int step = 0; step < MAX_STEPS; ++step) {
    // ── Think ──
    auto resp = think(step == 0 ? user_input : "[继续]");
    if (!resp)
      return make_unexpected(resp.error());

    // ── 完成（无工具调用）──
    if (!resp->wants_tool_call()) {
      // 将最终结果存入记忆
      (void)remember(fmt::format("Q: {} → A: {}", user_input, resp->content), 0.6f);
      return resp->content;
    }

    // ── Act（处理工具调用）──
    for (auto &tc : resp->tool_calls) {
      auto tool_result = act(tc);
      std::string obs;
      if (tool_result) {
        obs = tool_result->success ? tool_result->output
                                   : "工具执行失败: " + tool_result->error;
      } else {
        obs = "工具调用被拒绝: " + tool_result.error().message;
      }

      // Observe：将工具结果追加到上下文
      kernel::Message obs_msg;
      obs_msg.role = kernel::Role::Tool;
      obs_msg.content = obs;
      obs_msg.tool_call_id = tc.id;
      obs_msg.name = tc.name;
      os_->ctx().append(id_, obs_msg);
    }
  }

  return make_error(ErrorCode::Unknown,
                    "ReAct loop exceeded max steps without reaching stop");
}

} // namespace agentos
