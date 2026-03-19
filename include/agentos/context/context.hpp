#pragma once
// ============================================================
// AgentOS :: Module 3 — Context Manager
// 上下文窗口管理、状态快照/恢复、虚拟上下文换页
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <memory_resource>
#include <vector>

namespace agentos::context {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// § 3.1  ContextWindow — Token 预算管理器
// ─────────────────────────────────────────────────────────────

class ContextWindow {
public:
  explicit ContextWindow(TokenCount max_tokens = 8192)
      : max_tokens_(max_tokens),
        messages_(&pool_),
        evicted_(&pool_) {}

  // R5-1: Per-window mutex for granular locking
  mutable std::mutex mu;

  // 尝试添加消息；若超预算返回 false
  bool try_add(const kernel::Message &msg) {
    TokenCount cost = msg.tokens();
    if (used_tokens_ + cost > max_tokens_ || messages_.size() >= kMaxMessages)
      return false;
    messages_.push_back(msg);
    used_tokens_ += cost;
    return true;
  }

  // 强制添加（可能触发驱逐）— 实现在 context.cpp
  void add_evict_if_needed(const kernel::Message &msg);

  const std::pmr::deque<kernel::Message> &messages() const { return messages_; }
  const std::pmr::deque<kernel::Message> &evicted() const { return evicted_; }
  size_t message_count() const { return messages_.size(); }
  TokenCount used_tokens() const { return used_tokens_; }
  TokenCount max_tokens() const { return max_tokens_; }
  float utilization() const {
    return static_cast<float>(used_tokens_) / static_cast<float>(max_tokens_);
  }

  bool is_near_full(float threshold = 0.85f) const {
    return utilization() >= threshold;
  }

  void clear_evicted() { evicted_.clear(); }
  void reset() {
    messages_.clear();
    evicted_.clear();
    used_tokens_ = 0;
  }

  void inject_summary(std::string summary_content);

private:
  static constexpr size_t kMaxMessages = 50000;
  TokenCount max_tokens_;
  TokenCount used_tokens_{0};
  std::pmr::unsynchronized_pool_resource pool_;
  std::pmr::deque<kernel::Message> messages_;
  std::pmr::deque<kernel::Message> evicted_;
};

// ─────────────────────────────────────────────────────────────
// § 3.2  ContextSnapshot
// ─────────────────────────────────────────────────────────────

struct ContextSnapshot {
  AgentId agent_id;
  SessionId session_id;
  TimePoint captured_at;
  std::vector<kernel::Message> messages;
  std::string metadata_json;

  std::vector<uint8_t> serialize_binary() const;
  static std::optional<ContextSnapshot> deserialize_binary(std::span<const uint8_t> data);
};

// ─────────────────────────────────────────────────────────────
// § 3.2b SessionState — 完整会话状态（用于 save/resume）
// ─────────────────────────────────────────────────────────────

struct SessionState {
  AgentId agent_id;
  SessionId session_id;
  TimePoint saved_at;
  std::string config_json;                    // AgentConfig serialized as JSON
  std::vector<std::string> middleware_names;   // for re-registration by application
  ContextSnapshot context;
  std::string metadata_json;

  std::vector<uint8_t> serialize_binary() const;
  static std::optional<SessionState> deserialize_binary(std::span<const uint8_t> data);
};

// ─────────────────────────────────────────────────────────────
// § 3.3  ContextManager
// ─────────────────────────────────────────────────────────────

using EvictionCallback = std::function<void(AgentId, const std::vector<kernel::Message> &)>;
using SummarizeFn = std::function<std::string(const std::vector<kernel::Message> &)>;

class ContextManager : private NonCopyable {
public:
  explicit ContextManager(fs::path snapshot_dir = std::filesystem::temp_directory_path() / "agentos_snapshots")
      : snapshot_dir_(std::move(snapshot_dir)) {
    fs::create_directories(snapshot_dir_);
  }

  ContextWindow &get_window(AgentId agent_id, TokenCount max_tokens = 8192) {
    {
      std::shared_lock lk(map_mu_);
      auto it = windows_.find(agent_id);
      if (it != windows_.end()) return *it->second;
    }
    std::lock_guard lk(map_mu_);
    auto [it, inserted] = windows_.emplace(agent_id, std::make_unique<ContextWindow>(max_tokens));
    return *it->second;
  }

  void append(AgentId agent_id, kernel::Message msg, EvictionCallback on_evict = nullptr);
  void compress(AgentId agent_id, SummarizeFn summarize);
  Result<fs::path> snapshot(AgentId agent_id, const std::string &metadata = "{}");
  Result<void> restore(AgentId agent_id);

  // Session persistence
  Result<fs::path> save_session(AgentId agent_id, const std::string& config_json,
                                const std::vector<std::string>& middleware_names,
                                const std::string& metadata = "{}");
  Result<SessionState> load_session(AgentId agent_id, const SessionId& session_id);
  Result<std::vector<SessionId>> list_sessions(AgentId agent_id) const;

  void clear(AgentId agent_id) {
    std::lock_guard lk(map_mu_);
    windows_.erase(agent_id);
  }

private:
  mutable std::shared_mutex map_mu_;
  std::unordered_map<AgentId, std::unique_ptr<ContextWindow>> windows_;
  fs::path snapshot_dir_;
  std::atomic<uint64_t> session_seq_{0};
};

} // namespace agentos::context
