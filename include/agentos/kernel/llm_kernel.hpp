#pragma once
// ============================================================
// AgentOS :: Module 1 — LLM Kernel
// LLM 内核抽象：请求/响应类型、后端接口、限流器、Mock/OpenAI 实现
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/task.hpp>
#include <agentos/core/types.hpp>
#include <agentos/kernel/http_client.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentos::kernel {

// ─────────────────────────────────────────────────────────────
// § 1.1  消息与请求/响应类型
// ─────────────────────────────────────────────────────────────

enum class Role { System, User, Assistant, Tool };

struct ToolCallRequest {
  std::string id;        // 工具调用 ID（LLM 生成）
  std::string name;      // 工具名
  std::string args_json; // 参数 JSON
};

struct Message {
  Role role;
  std::string content;
  std::string name;                        // 工具名（role=Tool 时）
  std::string tool_call_id;                // 工具调用时填写
  
  static constexpr size_t kSmallToolCalls = 4;
  std::vector<ToolCallRequest> tool_calls; 

  // R5-5: Cache token count to avoid redundant estimation.
  mutable TokenCount cached_tokens{0};

  TokenCount tokens() const;

  // 静态工厂方法
  static Message system(std::string c) {
    return {.role = Role::System, .content = std::move(c), .name = {}, .tool_call_id = {}, .tool_calls = {}};
  }
  static Message user(std::string c) {
    return {.role = Role::User, .content = std::move(c), .name = {}, .tool_call_id = {}, .tool_calls = {}};
  }
  static Message assistant(std::string c) {
    return {.role = Role::Assistant, .content = std::move(c), .name = {}, .tool_call_id = {}, .tool_calls = {}};
  }
};

struct LLMRequest {
  std::vector<Message> messages;
  std::string model;
  float temperature{0.7f};
  TokenCount max_tokens{2048};
  Priority priority{Priority::Normal};
  AgentId agent_id{0};
  TaskId task_id{0};
  std::string request_id;  // Correlation ID for tracing; auto-generated if empty
  std::optional<std::vector<std::string>> tool_names; // 允许调用的工具
  std::optional<std::string>
      tools_json; // OpenAI function-calling 格式的工具列表 JSON

  static class LLMRequestBuilder builder();
};

class LLMRequestBuilder {
public:
  LLMRequestBuilder& add_message(Message msg) {
    req_.messages.push_back(std::move(msg));
    return *this;
  }
  LLMRequestBuilder& user(std::string content) {
    return add_message(Message::user(std::move(content)));
  }
  LLMRequestBuilder& system(std::string content) {
    return add_message(Message::system(std::move(content)));
  }
  LLMRequestBuilder& assistant(std::string content) {
    return add_message(Message::assistant(std::move(content)));
  }
  LLMRequestBuilder& model(std::string m) {
    req_.model = std::move(m);
    return *this;
  }
  LLMRequestBuilder& temperature(float t) {
    req_.temperature = t;
    return *this;
  }
  LLMRequestBuilder& max_tokens(TokenCount m) {
    req_.max_tokens = m;
    return *this;
  }
  LLMRequestBuilder& priority(Priority p) {
    req_.priority = p;
    return *this;
  }
  LLMRequestBuilder& agent_id(AgentId id) {
    req_.agent_id = id;
    return *this;
  }
  LLMRequestBuilder& task_id(TaskId id) {
    req_.task_id = id;
    return *this;
  }
  LLMRequestBuilder& request_id(std::string id) {
    req_.request_id = std::move(id);
    return *this;
  }
  LLMRequestBuilder& tools(std::vector<std::string> names) {
    req_.tool_names = std::move(names);
    return *this;
  }
  LLMRequestBuilder& tools_json(std::string json) {
    req_.tools_json = std::move(json);
    return *this;
  }

  LLMRequest build() { return std::move(req_); }

private:
  LLMRequest req_;
};

inline LLMRequestBuilder LLMRequest::builder() { return LLMRequestBuilder{}; }

struct LLMResponse {
  std::string content;                     // 文本输出
  std::vector<ToolCallRequest> tool_calls; // 工具调用请求
  TokenCount prompt_tokens{0};
  TokenCount completion_tokens{0};
  std::string finish_reason; // "stop"|"tool_calls"|"length"

  TokenCount total_tokens() const { return prompt_tokens + completion_tokens; }
  bool wants_tool_call() const { return !tool_calls.empty(); }
};

struct EmbeddingRequest {
  std::vector<std::string> inputs;
  std::string model;
};

struct EmbeddingResponse {
  std::vector<std::vector<float>> embeddings;
  TokenCount total_tokens{0};
};

// ─────────────────────────────────────────────────────────────
// § 1.2  Token 桶限流器（Thread-safe）
// ─────────────────────────────────────────────────────────────

class TokenBucketRateLimiter {
public:
  static constexpr int64_t kMaxWaitMs = 300'000; // 5 minutes

  explicit TokenBucketRateLimiter(uint32_t tpm_limit)
      : tpm_limit_(std::max(tpm_limit, 1u)), bucket_(tpm_limit_),
        refill_rate_(static_cast<double>(tpm_limit_) / 60.0),
        last_refill_(Clock::now()) {}

  // 尝试消耗 n 个 token；失败时返回需等待时长
  struct ConsumeResult {
    bool ok;
    Duration wait_ms; // 若 ok=false，需等待的时间
  };

  ConsumeResult try_consume(TokenCount n) {
    std::lock_guard lk(mu_);
    refill_locked();
    if (bucket_ >= static_cast<double>(n)) [[likely]] {
      bucket_ -= n;
      if (bucket_ > 0.0) cv_.notify_all();
      return {true, Duration{0}};
    }
    // 计算需要等待多少毫秒（clamp to sane range）
    double deficit = n - bucket_;
    double raw_wait = deficit / refill_rate_ * 1000.0;
    if (raw_wait > static_cast<double>(kMaxWaitMs)) raw_wait = static_cast<double>(kMaxWaitMs);
    if (raw_wait < 0.0) raw_wait = 0.0;
    auto wait_ms = static_cast<int64_t>(raw_wait);
    return {false, Duration{wait_ms}};
  }

  // 尝试获取 n 个 token，如果不足则等待，最多重试 max_retries 次。
  // 成功返回 {true, total_waited, retries}，失败返回 {false, total_waited, retries}。
  struct AcquireResult {
    bool ok;
    Duration waited;
    int retries;
  };

  AcquireResult acquire(TokenCount n, int max_retries = 2) {
    std::unique_lock lk(mu_);
    auto start_time = Clock::now();
    int attempt = 0;

    while (attempt <= max_retries) {
      refill_locked();
      if (bucket_ >= static_cast<double>(n)) [[likely]] {
        bucket_ -= n;
        if (bucket_ > 0.0) cv_.notify_all();
        auto total_waited = std::chrono::duration_cast<Duration>(Clock::now() - start_time);
        return {true, total_waited, attempt};
      }

      if (attempt == max_retries) {
        break;
      }

      double deficit = n - bucket_;
      double raw_wait = deficit / refill_rate_ * 1000.0;
      if (raw_wait > static_cast<double>(kMaxWaitMs)) raw_wait = static_cast<double>(kMaxWaitMs);
      if (raw_wait < 0.0) raw_wait = 0.0;
      auto wait_ms = Duration{static_cast<int64_t>(raw_wait)};
      auto target_time = Clock::now() + wait_ms;

      // Use condition variable for waiting instead of sleep_for.
      // The predicate handles spurious wakeups by checking if enough tokens
      // have been refilled.
      bool acquired = cv_.wait_until(lk, target_time, [this, n] {
        refill_locked();
        return bucket_ >= static_cast<double>(n);
      });

      if (acquired) {
        bucket_ -= n;
        if (bucket_ > 0.0) cv_.notify_all();
        auto total_waited = std::chrono::duration_cast<Duration>(Clock::now() - start_time);
        // If we acquired it here, attempt is still what it was before waiting.
        return {true, total_waited, attempt + 1};
      }

      ++attempt;
    }

    auto total_waited = std::chrono::duration_cast<Duration>(Clock::now() - start_time);
    return {false, total_waited, attempt};
  }

  uint32_t available_tokens() const noexcept {
    std::lock_guard lk(mu_);
    return static_cast<uint32_t>(bucket_);
  }

  void set_rate(size_t new_tpm_limit) {
    std::lock_guard lk(mu_);
    tpm_limit_ = std::max(static_cast<uint32_t>(new_tpm_limit), 1u);
    refill_rate_ = static_cast<double>(tpm_limit_) / 60.0;
    // Cap bucket to new limit
    if (bucket_ > static_cast<double>(tpm_limit_)) {
      bucket_ = static_cast<double>(tpm_limit_);
    }
  }

private:
  void refill_locked() {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
    // Clamp elapsed to prevent integer overflow in large system clock jumps
    elapsed = std::min(elapsed, 3600.0); // Max 1 hour
    bucket_ = std::min(static_cast<double>(tpm_limit_),
                       bucket_ + elapsed * refill_rate_);
    last_refill_ = now;
  }

  uint32_t tpm_limit_;
  double bucket_;
  double refill_rate_; // tokens/sec
  TimePoint last_refill_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
};

// ─────────────────────────────────────────────────────────────
// § 1.3  ILLMBackend 接口（纯虚）
// ─────────────────────────────────────────────────────────────

class ILLMBackend {
public:
  virtual ~ILLMBackend() = default;

  // 同步推理（由调度器在工作线程上调用）
  [[nodiscard]] virtual Result<LLMResponse> complete(const LLMRequest &req) = 0;

  // 向量化嵌入
  [[nodiscard]] virtual Result<EmbeddingResponse> embed(const EmbeddingRequest & /*req*/) {
    return make_error(ErrorCode::LLMBackendError,
                      "Embedding not implicitly supported by backend");
  }

  // 流式推理（逐 token 回调）
  using TokenCallback = std::function<void(std::string_view token)>;
  [[nodiscard]] virtual Result<LLMResponse> stream(const LLMRequest &req,
                                     TokenCallback /*cb*/) {
    // 默认降级为非流式
    return complete(req);
  }

  virtual std::string name() const noexcept = 0;

  // 估算 prompt token 数（简单启发：英文约 4 字符/token）
  static TokenCount estimate_tokens(std::string_view text) {
    return static_cast<TokenCount>(text.size() / 4 + 1);
  }
};

// ── LLM Backend Concept ────────────────────────
template <typename T>
concept LLMBackendConcept = requires(T b, const LLMRequest &req) {
  { b.complete(req) } -> std::same_as<Result<LLMResponse>>;
  { b.name() } -> std::same_as<std::string>;
} && std::derived_from<T, ILLMBackend>;

inline TokenCount Message::tokens() const {
  if (cached_tokens == 0 && !content.empty()) {
    cached_tokens = ILLMBackend::estimate_tokens(content) + 4;
  }
  return cached_tokens;
}

// ─────────────────────────────────────────────────────────────
// § 1.4  MockBackend（用于测试/离线演示）
// ─────────────────────────────────────────────────────────────

class MockLLMBackend : public ILLMBackend {
public:
    struct Rule {
        std::string trigger_pattern; // 可以是普通字符串或正则
        std::string response_content;
        std::optional<std::pair<std::string, std::string>> tool_call; // name, args
        int priority{0};
        bool is_regex{false};

        auto matches(std::string_view text) const -> bool {
            if (is_regex) {
                try {
                    return std::regex_search(text.begin(), text.end(), std::regex(trigger_pattern));
                } catch (...) { return false; }
            }
            return text.find(trigger_pattern) != std::string_view::npos;
        }
    };

    explicit MockLLMBackend(std::string model_name = "mock-gpt")
        : model_name_(std::move(model_name)) {}

    void register_rule(std::string trigger, std::string response, int priority = 0, bool is_regex = false) {
        std::lock_guard lk(mu_);
        rules_.push_back({std::move(trigger), std::move(response), std::nullopt, priority, is_regex});
        sort_rules_locked();
    }

    void register_tool_rule(std::string trigger, std::string tool_name, std::string args_json, int priority = 0, bool is_regex = false) {
        std::lock_guard lk(mu_);
        rules_.push_back({std::move(trigger), "", std::make_pair(std::move(tool_name), std::move(args_json)), priority, is_regex});
        sort_rules_locked();
    }

    [[nodiscard]] Result<LLMResponse> complete(const LLMRequest &req) override {
        std::lock_guard lk(mu_);
        if (req.messages.empty()) return default_response(req);

        std::string_view last_msg = req.messages.back().content;
        
        for (const auto& rule : rules_) {
            if (rule.matches(last_msg)) {
                LLMResponse resp;
                if (rule.tool_call) {
                    resp.tool_calls.push_back({
                        .id = fmt::format("call_{}", call_count_++),
                        .name = rule.tool_call->first,
                        .args_json = rule.tool_call->second,
                    });
                    resp.finish_reason = "tool_calls";
                    resp.completion_tokens = 20;
                } else {
                    resp.content = rule.response_content;
                    resp.finish_reason = "stop";
                    resp.completion_tokens = ILLMBackend::estimate_tokens(rule.response_content);
                }
                resp.prompt_tokens = ILLMBackend::estimate_tokens(last_msg);
                return resp;
            }
        }

        return default_response(req);
    }

    [[nodiscard]] Result<EmbeddingResponse> embed(const EmbeddingRequest &req) override {
        EmbeddingResponse resp;
        for (const auto &input : req.inputs) {
            std::vector<float> emb(embed_dim_, 0.0f);
            size_t h = std::hash<std::string>{}(input);
            float norm_sq = 0.0f;
            for (size_t i = 0; i < embed_dim_; ++i) {
                h ^= (h << 13) ^ (h >> 7) ^ (i * 2654435761ULL);
                emb[i] = static_cast<float>(static_cast<int32_t>(h & 0xFFFF) - 32768) / 32768.0f;
                norm_sq += emb[i] * emb[i];
            }
            float norm = std::sqrt(norm_sq);
            if (norm > 0.0f) {
                for (auto &v : emb) v /= norm;
            }
            resp.embeddings.push_back(std::move(emb));
            resp.total_tokens += ILLMBackend::estimate_tokens(input);
        }
        return resp;
    }

    std::string name() const noexcept override { return model_name_; }
    void set_embed_dim(size_t dim) { embed_dim_ = dim; }

private:
    void sort_rules_locked() {
        std::stable_sort(rules_.begin(), rules_.end(), [](const Rule& a, const Rule& b) {
            return a.priority > b.priority; // 更高优先级在前
        });
    }

    LLMResponse default_response(const LLMRequest& req) const {
        std::string text = req.messages.empty() ? "" : req.messages.back().content;
        std::string reply = fmt::format("[MockLLM:{}] 收到 {} 条消息", model_name_, req.messages.size());
        
        LLMResponse resp;
        resp.content = std::move(reply);
        resp.finish_reason = "stop";
        resp.prompt_tokens = ILLMBackend::estimate_tokens(text);
        resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);
        return resp;
    }

    std::string model_name_;
    std::vector<Rule> rules_;
    std::atomic<uint64_t> call_count_{0};
    size_t embed_dim_{1536};
    mutable std::mutex mu_;
};

// ─────────────────────────────────────────────────────────────
// § 1.5  OpenAIBackend（HTTP 实现，通过 curl 子进程）
// ─────────────────────────────────────────────────────────────

class OpenAIBackend : public ILLMBackend {
public:
  // api_key    : OpenAI API Key（或兼容服务的 Key）
  // base_url   : API 基础 URL，默认 OpenAI，可替换为 Ollama/Azure/硅基流动等
  // default_model : 默认模型名，可被 LLMRequest.model 覆盖
  explicit OpenAIBackend(std::string api_key,
                         std::string base_url = "https://api.openai.com/v1",
                         std::string default_model = "gpt-4o-mini")
      : api_key_(std::move(api_key)), base_url_(std::move(base_url)),
        default_model_(std::move(default_model)) {
    // R7-17: Validate base URL against SSRF attacks
    validate_and_warn_ssrf();
  }

  [[nodiscard]] Result<LLMResponse> complete(const LLMRequest &req) override;
  [[nodiscard]] Result<LLMResponse> stream(const LLMRequest &req, TokenCallback cb) override;
  [[nodiscard]] Result<EmbeddingResponse> embed(const EmbeddingRequest &req) override;
  std::string name() const noexcept override { return "openai/" + default_model_; }

private:
  std::string build_request_json(const LLMRequest &req) const;
  Result<LLMResponse> parse_response(const std::string &json_str) const;
  Result<std::string> http_post(const std::string &endpoint,
                                const std::string &body) const;

  // R7-17: SSRF validation
  void validate_and_warn_ssrf() const;

  std::string api_key_;
  std::string base_url_;
  std::string default_model_;
  HttpClient http_client_;
};

// ─────────────────────────────────────────────────────────────
// § 1.6  EmbeddingCache：向量嵌入 LRU 缓存 (Thread-safe)
// ─────────────────────────────────────────────────────────────
class EmbeddingCache {
public:
  explicit EmbeddingCache(size_t capacity = 1024) : capacity_(capacity) {}

  std::optional<std::vector<float>> get(const std::string &input) {
    std::lock_guard lk(mu_);
    auto it = cache_map_.find(input);
    if (it == cache_map_.end()) return std::nullopt;
    
    // Move to front (LRU)
    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
    return it->second->second;
  }

  void put(const std::string &input, std::vector<float> embedding) {
    std::lock_guard lk(mu_);
    auto it = cache_map_.find(input);
    if (it != cache_map_.end()) {
      cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
      it->second->second = std::move(embedding);
      return;
    }

    if (cache_list_.size() >= capacity_) {
      auto last = cache_list_.back();
      cache_map_.erase(last.first);
      cache_list_.pop_back();
    }

    cache_list_.emplace_front(input, std::move(embedding));
    cache_map_[input] = cache_list_.begin();
  }

  void clear() {
    std::lock_guard lk(mu_);
    cache_list_.clear();
    cache_map_.clear();
  }

private:
  size_t capacity_;
  std::mutex mu_;
  using Entry = std::pair<std::string, std::vector<float>>;
  std::list<Entry> cache_list_;
  std::unordered_map<std::string, std::list<Entry>::iterator> cache_map_;
};

// ─────────────────────────────────────────────────────────────
// § 1.7  LLMKernel：内核门面，含限流、指标收集
// ─────────────────────────────────────────────────────────────

struct KernelMetrics {
  std::atomic<uint64_t> total_requests{0};
  std::atomic<uint64_t> total_tokens{0};
  std::atomic<uint64_t> rate_limit_hits{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> retries{0};

  // Saturation increment: won't overflow, caps at UINT64_MAX
  static inline void saturating_add(std::atomic<uint64_t> &counter, uint64_t delta) noexcept {
    uint64_t cur = counter.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = (cur > UINT64_MAX - delta) ? UINT64_MAX : cur + delta;
    } while (!counter.compare_exchange_weak(cur, next, std::memory_order_relaxed));
  }
};

class LLMKernel : private NonCopyable {
public:
  explicit LLMKernel(std::unique_ptr<ILLMBackend> backend,
                     uint32_t tpm_limit = 100000,
                     uint32_t max_retries = 3)
      : backend_(std::move(backend)), rate_limiter_(tpm_limit),
        max_retries_(max_retries) {}

  // 主要入口：带限流 + 重试的同步推理
  [[nodiscard]] Result<LLMResponse> infer(const LLMRequest &req) {
    TokenCount estimated = estimate_request_tokens(req);
    if (auto err = acquire_rate_limit(estimated))
      return make_unexpected(*err);

    metrics_.total_requests++;
    return with_retry([&]() { return backend_->complete(req); });
  }

  // 流式推理（不重试，因为 callback 有副作用）
  [[nodiscard]] Result<LLMResponse> stream_infer(const LLMRequest &req,
                                   ILLMBackend::TokenCallback cb) {
    TokenCount estimated = estimate_request_tokens(req);
    if (auto err = acquire_rate_limit(estimated))
      return make_unexpected(*err);

    metrics_.total_requests++;
    auto result = backend_->stream(req, std::move(cb));
    track_result(result);
    return result;
  }

  // 获取文本向量嵌入
  [[nodiscard]] Result<EmbeddingResponse> embed(const EmbeddingRequest &req) {
    EmbeddingResponse final_resp;
    EmbeddingRequest missed_req;
    std::vector<int> missed_indices;

    for (size_t i = 0; i < req.inputs.size(); ++i) {
      if (auto cached = embed_cache_.get(req.inputs[i])) {
        if (final_resp.embeddings.size() <= i) final_resp.embeddings.resize(i + 1);
        final_resp.embeddings[i] = std::move(*cached);
      } else {
        missed_req.inputs.push_back(req.inputs[i]);
        missed_indices.push_back(i);
      }
    }

    if (missed_req.inputs.empty()) {
      return final_resp;
    }

    missed_req.model = req.model;
    TokenCount estimated = 0;
    for (const auto &text : missed_req.inputs)
      estimated += ILLMBackend::estimate_tokens(text);

    if (auto err = acquire_rate_limit(estimated))
      return make_unexpected(*err);

    metrics_.total_requests++;
    auto result = backend_->embed(missed_req);
    if (result) {
      KernelMetrics::saturating_add(metrics_.total_tokens, result->total_tokens);
      for (size_t j = 0; j < result->embeddings.size(); ++j) {
        int original_idx = missed_indices[j];
        if (final_resp.embeddings.size() <= static_cast<size_t>(original_idx)) 
          final_resp.embeddings.resize(original_idx + 1);
        final_resp.embeddings[original_idx] = result->embeddings[j];
        embed_cache_.put(missed_req.inputs[j], std::move(result->embeddings[j]));
      }
      final_resp.total_tokens = result->total_tokens;
    } else {
      metrics_.errors++;
      return make_unexpected(result.error());
    }
    return final_resp;
  }

  Task<Result<LLMResponse>> infer_async(LLMRequest req) {
      co_return infer(std::move(req));
  }

  Task<Result<EmbeddingResponse>> embed_async(EmbeddingRequest req) {
      co_return embed(std::move(req));
  }

  ILLMBackend &backend() noexcept { return *backend_; }
  KernelMetrics &metrics() noexcept { return metrics_; }
  std::string model_name() const noexcept { return backend_->name(); }
  TokenBucketRateLimiter &rate_limiter() noexcept { return rate_limiter_; }

private:
  // Estimate total tokens for a chat request
  static TokenCount estimate_request_tokens(const LLMRequest &req) {
    TokenCount estimated = 0;
    for (auto &m : req.messages)
      estimated += ILLMBackend::estimate_tokens(m.content);
    estimated += req.max_tokens;
    return estimated;
  }

  // Try to acquire rate limit tokens, retrying up to 3 times
  std::optional<Error> acquire_rate_limit(TokenCount estimated) {
    auto [ok, waited, retries] = rate_limiter_.acquire(estimated, 2);
    if (ok)
      return std::nullopt;

    metrics_.rate_limit_hits++;
    return Error{ErrorCode::RateLimitExceeded,
                 fmt::format("Rate limit exhausted after {} retries "
                             "(needed={:.1f} tokens, bucket={:.1f}, rate={:.1f}/s, waited={}ms)",
                             retries, static_cast<double>(estimated),
                             rate_limiter_.available_tokens(),
                             rate_limiter_.available_tokens() / 60.0, waited.count())};
  }

  // Track metrics after LLM call
  void track_result(const Result<LLMResponse> &result) {
    if (result) {
      KernelMetrics::saturating_add(metrics_.total_tokens, result->total_tokens());
    } else {
      metrics_.errors++;
    }
  }

  // Check if an error is transient (worth retrying)
  static bool is_transient_error(const Error &err) {
    // Retry on backend errors (HTTP 5xx, timeouts, network failures)
    // Also retry on rate limits (429) with backoff
    if (err.code == ErrorCode::Timeout) return true;
    if (err.code == ErrorCode::RateLimitExceeded) return true;
    if (err.code != ErrorCode::LLMBackendError) return false;
    // Check for 5xx patterns in error message
    auto &msg = err.message;
    return msg.find("HTTP 5") != std::string::npos ||
           msg.find("CURLE_") != std::string::npos ||
           msg.find("HTTP request failed") != std::string::npos ||
           msg.find("timed out") != std::string::npos;
  }

  // Retry with exponential backoff for transient errors
  template <typename Fn>
  auto with_retry(Fn &&fn) -> decltype(fn()) {
    Result<LLMResponse> last_result = make_error(ErrorCode::Unknown, "no attempt");
    for (uint32_t attempt = 0; attempt <= max_retries_; ++attempt) {
      if (attempt > 0) {
        // Exponential backoff: 200ms, 400ms, 800ms... + jitter
        auto base_ms = 200 * (1u << (attempt - 1));
        auto jitter = static_cast<uint32_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(base_ms + jitter));
        metrics_.retries++;
      }
      last_result = fn();
      if (last_result) {
        track_result(last_result);
        return last_result;
      }
      // Don't retry non-transient errors
      if (!is_transient_error(last_result.error())) {
        track_result(last_result);
        return last_result;
      }
      LOG_WARN(fmt::format("LLMKernel: transient error (attempt {}/{}): {}",
                           attempt + 1, max_retries_ + 1,
                           last_result.error().message));
    }
    track_result(last_result);
    return last_result;
  }

  std::unique_ptr<ILLMBackend> backend_;
  TokenBucketRateLimiter rate_limiter_;
  EmbeddingCache embed_cache_;
  KernelMetrics metrics_;
  uint32_t max_retries_;
};

} // namespace agentos::kernel
