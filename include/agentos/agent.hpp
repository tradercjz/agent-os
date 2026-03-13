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
#include <cassert>
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

  // Middleware utilities
  bool run_before_hooks(HookContext &ctx);
  void run_after_hooks(HookContext &ctx);

  friend class AgentOS;
};

// ── AgentBase (CRTP) ───────────────────────────
template <typename Derived>
class AgentBase : public Agent {
public:
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

  HookContext hook_ctx{this->id_, "think", user_msg, false, {}};
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

  HookContext act_ctx{this->id_, "act", call.name, false, {}};
  if (!this->run_before_hooks(act_ctx)) return make_error(ErrorCode::Cancelled, act_ctx.cancel_reason);

  if (this->os_->security()) {
    auto check = this->os_->security()->authorize_tool(this->id_, call.name, call.args_json);
    if (!check) return make_error(check.error().code, check.error().message);
  }
  auto result = this->os_->tools().dispatch(call);
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

} // namespace agentos
