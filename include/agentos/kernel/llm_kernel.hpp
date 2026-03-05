#pragma once
// ============================================================
// AgentOS :: Module 1 — LLM Kernel
// LLM 内核抽象：请求/响应类型、后端接口、限流器、Mock/OpenAI 实现
// ============================================================
#include <agentos/core/task.hpp>
#include <agentos/core/types.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
  std::vector<ToolCallRequest> tool_calls; // Assistant 触发的工具调用记录

  // 静态工厂方法，用 C++20 指定初始化器避免警告
  static Message system(std::string c) {
    return {.role = Role::System,
            .content = std::move(c),
            .name = "",
            .tool_call_id = "",
            .tool_calls = {}};
  }
  static Message user(std::string c) {
    return {.role = Role::User,
            .content = std::move(c),
            .name = "",
            .tool_call_id = "",
            .tool_calls = {}};
  }
  static Message assistant(std::string c) {
    return {.role = Role::Assistant,
            .content = std::move(c),
            .name = "",
            .tool_call_id = "",
            .tool_calls = {}};
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
  std::optional<std::vector<std::string>> tool_names; // 允许调用的工具
  std::optional<std::string>
      tools_json; // OpenAI function-calling 格式的工具列表 JSON
};

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
  explicit TokenBucketRateLimiter(uint32_t tpm_limit)
      : tpm_limit_(tpm_limit), bucket_(tpm_limit),
        refill_rate_(static_cast<double>(tpm_limit) / 60.0),
        last_refill_(Clock::now()) {}

  // 尝试消耗 n 个 token；失败时返回需等待时长
  struct ConsumeResult {
    bool ok;
    Duration wait_ms; // 若 ok=false，需等待的时间
  };

  ConsumeResult try_consume(TokenCount n) {
    std::lock_guard lk(mu_);
    refill_locked();
    if (bucket_ >= static_cast<double>(n)) {
      bucket_ -= n;
      return {true, Duration{0}};
    }
    // 计算需要等待多少毫秒
    double deficit = n - bucket_;
    auto wait_ms = static_cast<int64_t>(deficit / refill_rate_ * 1000.0);
    return {false, Duration{wait_ms}};
  }

  uint32_t available_tokens() const {
    std::lock_guard lk(mu_);
    return static_cast<uint32_t>(bucket_);
  }

private:
  void refill_locked() {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
    bucket_ = std::min(static_cast<double>(tpm_limit_),
                       bucket_ + elapsed * refill_rate_);
    last_refill_ = now;
  }

  uint32_t tpm_limit_;
  double bucket_;
  double refill_rate_; // tokens/sec
  TimePoint last_refill_;
  mutable std::mutex mu_;
};

// ─────────────────────────────────────────────────────────────
// § 1.3  ILLMBackend 接口（纯虚）
// ─────────────────────────────────────────────────────────────

class ILLMBackend {
public:
  virtual ~ILLMBackend() = default;

  // 同步推理（由调度器在工作线程上调用）
  virtual Result<LLMResponse> complete(const LLMRequest &req) = 0;

  // 向量化嵌入
  virtual Result<EmbeddingResponse> embed(const EmbeddingRequest & /*req*/) {
    return make_error(ErrorCode::LLMBackendError,
                      "Embedding not implicitly supported by backend");
  }

  // 流式推理（逐 token 回调）
  using TokenCallback = std::function<void(std::string_view token)>;
  virtual Result<LLMResponse> stream(const LLMRequest &req,
                                     TokenCallback /*cb*/) {
    // 默认降级为非流式
    return complete(req);
  }

  virtual std::string name() const = 0;

  // 估算 prompt token 数（简单启发：英文约 4 字符/token）
  static TokenCount estimate_tokens(std::string_view text) {
    return static_cast<TokenCount>(text.size() / 4 + 1);
  }
};

// ─────────────────────────────────────────────────────────────
// § 1.4  MockBackend（用于测试/离线演示）
// ─────────────────────────────────────────────────────────────

class MockLLMBackend : public ILLMBackend {
public:
  explicit MockLLMBackend(std::string model_name = "mock-gpt")
      : model_name_(std::move(model_name)) {}

  // 注册规则：当 prompt 包含 trigger 时，回复 response
  void register_rule(std::string trigger, std::string response) {
    rules_.emplace_back(std::move(trigger), std::move(response));
  }

  // 注册工具调用规则
  void register_tool_rule(std::string trigger, std::string tool_name,
                          std::string args_json) {
    tool_rules_.emplace_back(std::move(trigger), std::move(tool_name),
                             std::move(args_json));
  }

  Result<LLMResponse> complete(const LLMRequest &req) override {
    // 收集所有消息内容
    std::string combined;
    for (auto &m : req.messages)
      combined += m.content + " ";

    // 匹配工具调用规则
    for (auto &[trigger, tool, args] : tool_rules_) {
      if (combined.find(trigger) != std::string::npos) {
        LLMResponse resp;
        resp.tool_calls.push_back({
            .id = "call_" + std::to_string(call_count_++),
            .name = tool,
            .args_json = args,
        });
        resp.finish_reason = "tool_calls";
        resp.prompt_tokens = ILLMBackend::estimate_tokens(combined);
        resp.completion_tokens = 20;
        return resp;
      }
    }

    // 匹配文本规则
    for (auto &[trigger, response] : rules_) {
      if (combined.find(trigger) != std::string::npos) {
        LLMResponse resp;
        resp.content = response;
        resp.finish_reason = "stop";
        resp.prompt_tokens = ILLMBackend::estimate_tokens(combined);
        resp.completion_tokens = ILLMBackend::estimate_tokens(response);
        return resp;
      }
    }

    // 默认回复
    std::string default_reply =
        fmt::format("[MockLLM:{}] 收到 {} 条消息，最后一条：\"{}\"",
                    model_name_, req.messages.size(),
                    req.messages.empty() ? "" : req.messages.back().content);

    LLMResponse resp;
    resp.content = default_reply;
    resp.finish_reason = "stop";
    resp.prompt_tokens = ILLMBackend::estimate_tokens(combined);
    resp.completion_tokens = ILLMBackend::estimate_tokens(default_reply);
    return resp;
  }

  // 向量嵌入：生成确定性伪向量（基于文本内容哈希）
  Result<EmbeddingResponse> embed(const EmbeddingRequest &req) override {
    EmbeddingResponse resp;
    for (const auto &input : req.inputs) {
      std::vector<float> emb(embed_dim_, 0.0f);
      // 确定性哈希：同一文本始终生成相同向量
      std::hash<std::string> hasher;
      size_t h = hasher(input);
      float norm_sq = 0.0f;
      for (size_t i = 0; i < embed_dim_; ++i) {
        // 用哈希种子生成伪随机分量
        h ^= (h << 13) ^ (h >> 7) ^ (i * 2654435761ULL);
        emb[i] = static_cast<float>(static_cast<int32_t>(h & 0xFFFF) - 32768) /
                 32768.0f;
        norm_sq += emb[i] * emb[i];
      }
      // L2 归一化（使 inner product == cosine similarity）
      float norm = std::sqrt(norm_sq);
      if (norm > 0.0f) {
        for (auto &v : emb)
          v /= norm;
      }
      resp.embeddings.push_back(std::move(emb));
      resp.total_tokens += ILLMBackend::estimate_tokens(input);
    }
    return resp;
  }

  std::string name() const override { return model_name_; }

  // 设置 embed() 输出维度（默认 1536，可调小以加速测试）
  void set_embed_dim(size_t dim) { embed_dim_ = dim; }

private:
  std::string model_name_;
  std::vector<std::pair<std::string, std::string>> rules_;
  std::vector<std::tuple<std::string, std::string, std::string>> tool_rules_;
  std::atomic<uint64_t> call_count_{0};
  size_t embed_dim_{1536};
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
        default_model_(std::move(default_model)) {}

  Result<LLMResponse> complete(const LLMRequest &req) override;
  Result<LLMResponse> stream(const LLMRequest &req, TokenCallback cb) override;
  Result<EmbeddingResponse> embed(const EmbeddingRequest &req) override;
  std::string name() const override { return "openai/" + default_model_; }

private:
  std::string build_request_json(const LLMRequest &req) const;
  Result<LLMResponse> parse_response(const std::string &json_str) const;
  Result<std::string> http_post(const std::string &endpoint,
                                const std::string &body) const;

  std::string api_key_;
  std::string base_url_;
  std::string default_model_;
};

// ─────────────────────────────────────────────────────────────
// § 1.6  LLMKernel：内核门面，含限流、指标收集
// ─────────────────────────────────────────────────────────────

struct KernelMetrics {
  std::atomic<uint64_t> total_requests{0};
  std::atomic<uint64_t> total_tokens{0};
  std::atomic<uint64_t> rate_limit_hits{0};
  std::atomic<uint64_t> errors{0};
};

class LLMKernel : private NonCopyable {
public:
  explicit LLMKernel(std::unique_ptr<ILLMBackend> backend,
                     uint32_t tpm_limit = 100000)
      : backend_(std::move(backend)), rate_limiter_(tpm_limit) {}

  // 主要入口：带限流的同步推理
  Result<LLMResponse> infer(const LLMRequest &req) {
    // 估算本次请求的 token 消耗
    TokenCount estimated = 0;
    for (auto &m : req.messages)
      estimated += ILLMBackend::estimate_tokens(m.content);
    estimated += req.max_tokens;

    // 限流检查（重试最多 3 次，等待后重试）
    for (int attempt = 0; attempt < 3; ++attempt) {
      auto [ok, wait] = rate_limiter_.try_consume(estimated);
      if (ok)
        break;
      if (attempt == 2) {
        metrics_.rate_limit_hits++;
        return make_error(
            ErrorCode::RateLimitExceeded,
            fmt::format("Rate limit: need {}ms wait", wait.count()));
      }
      std::this_thread::sleep_for(wait);
    }

    metrics_.total_requests++;
    auto result = backend_->complete(req);
    if (result) {
      metrics_.total_tokens += result->total_tokens();
    } else {
      metrics_.errors++;
    }
    return result;
  }

  // 流式推理：在推理过程中通过回调函数实时返回生成的 token
  Result<LLMResponse> stream_infer(const LLMRequest &req,
                                   ILLMBackend::TokenCallback cb) {
    TokenCount estimated = 0;
    for (auto &m : req.messages)
      estimated += ILLMBackend::estimate_tokens(m.content);
    estimated += req.max_tokens;

    for (int attempt = 0; attempt < 3; ++attempt) {
      auto [ok, wait] = rate_limiter_.try_consume(estimated);
      if (ok)
        break;
      if (attempt == 2) {
        metrics_.rate_limit_hits++;
        return make_error(
            ErrorCode::RateLimitExceeded,
            fmt::format("Rate limit: need {}ms wait", wait.count()));
      }
      std::this_thread::sleep_for(wait);
    }

    metrics_.total_requests++;
    auto result = backend_->stream(req, std::move(cb));
    if (result) {
      metrics_.total_tokens += result->total_tokens();
    } else {
      metrics_.errors++;
    }
    return result;
  }

  // 获取文本向量嵌入
  Result<EmbeddingResponse> embed(const EmbeddingRequest &req) {
    TokenCount estimated = 0;
    for (const auto &text : req.inputs) {
      estimated += ILLMBackend::estimate_tokens(text);
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
      auto [ok, wait] = rate_limiter_.try_consume(estimated);
      if (ok)
        break;
      if (attempt == 2) {
        metrics_.rate_limit_hits++;
        return make_error(
            ErrorCode::RateLimitExceeded,
            fmt::format("Rate limit: need {}ms wait", wait.count()));
      }
      std::this_thread::sleep_for(wait);
    }

    metrics_.total_requests++;
    auto result = backend_->embed(req);
    if (result) {
      metrics_.total_tokens += result->total_tokens;
    } else {
      metrics_.errors++;
    }
    return result;
  }

  ILLMBackend &backend() { return *backend_; }
  KernelMetrics &metrics() { return metrics_; }
  std::string model_name() const { return backend_->name(); }

private:
  std::unique_ptr<ILLMBackend> backend_;
  TokenBucketRateLimiter rate_limiter_;
  KernelMetrics metrics_;
};

} // namespace agentos::kernel
