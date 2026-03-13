#pragma once
// ============================================================
// AgentOS :: Module 3 — Context Manager
// 上下文窗口管理、状态快照/恢复、虚拟上下文换页
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
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
        pool_(4096), // Initial 4KB block
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

  // 强制添加（可能触发驱逐）
  void add_evict_if_needed(const kernel::Message &msg) {
    TokenCount cost = msg.tokens();
    while (!messages_.empty() && used_tokens_ + cost > max_tokens_) {
      // R5-6: Fast heuristic-based eviction. 
      // Instead of full O(N) scan, find first occurrence of each role.
      size_t evict_idx = std::string::npos;
      size_t first_user = std::string::npos;
      size_t first_tool = std::string::npos;
      size_t first_assistant = std::string::npos;
      
      for (size_t i = 0; i < messages_.size(); ++i) {
        auto role = messages_[i].role;
        if (role == kernel::Role::User && first_user == std::string::npos) first_user = i;
        else if (role == kernel::Role::Tool && first_tool == std::string::npos) first_tool = i;
        else if (role == kernel::Role::Assistant && first_assistant == std::string::npos) first_assistant = i;
        
        if (first_user != std::string::npos && first_tool != std::string::npos && first_assistant != std::string::npos) break;
        if (i > 128) break; // Defensive limit: good enough victims usually at the front
      }
      
      float worst_score = std::numeric_limits<float>::max();
      auto check = [&](size_t idx, float bias) {
        if (idx == std::string::npos) return;
        float s = static_cast<float>(idx) + bias;
        if (s < worst_score) { worst_score = s; evict_idx = idx; }
      };
      
      check(first_user, -1000.0f);
      check(first_tool, -500.0f);
      check(first_assistant, -100.0f);
      
      // Fallback: if no biased candidates, take the absolute first non-system message
      if (evict_idx == std::string::npos) {
        for (size_t i = 0; i < messages_.size(); ++i) {
          if (messages_[i].role != kernel::Role::System) { evict_idx = i; break; }
        }
      }

      if (evict_idx == std::string::npos || evict_idx >= messages_.size()) {
        LOG_WARN(fmt::format("[Context] Cannot evict system messages. Budget may overun."));
        break;
      }
      evicted_.push_back(std::move(messages_[evict_idx]));
      used_tokens_ -= (evicted_.back()).tokens();
      messages_.erase(messages_.begin() + evict_idx);
    }
    messages_.push_back(msg);
    used_tokens_ += cost;
  }

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
    // pool_.release(); // Note: we don't release pool here to keep memory for reuse
  }

  void inject_summary(std::string summary_content) {
    messages_.push_front(
        kernel::Message::system("=== 历史对话摘要 ===\n" + std::move(summary_content)));
    used_tokens_ = 0;
    for (auto &m : messages_)
      used_tokens_ += m.tokens();
  }

private:
  static constexpr size_t kMaxMessages = 50000;
  TokenCount max_tokens_;
  TokenCount used_tokens_{0};
  std::pmr::monotonic_buffer_resource pool_;
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

  std::vector<uint8_t> serialize_binary() const {
    std::vector<uint8_t> buf;
    auto append_u32 = [&](uint32_t v) {
      uint8_t b[4];
      std::memcpy(b, &v, 4);
      buf.insert(buf.end(), b, b + 4);
    };
    auto append_str = [&](const std::string &s) {
      append_u32(static_cast<uint32_t>(s.size()));
      buf.insert(buf.end(), s.begin(), s.end());
    };

    append_u32(static_cast<uint32_t>(agent_id));
    append_str(session_id);
    append_u32(static_cast<uint32_t>(captured_at.time_since_epoch().count() / 1000000)); // ms
    append_u32(static_cast<uint32_t>(messages.size()));
    for (const auto &m : messages) {
      buf.push_back(static_cast<uint8_t>(m.role));
      append_str(m.content);
      append_str(m.name);
      append_str(m.tool_call_id);
    }
    append_str(metadata_json);
    return buf;
  }

  static std::optional<ContextSnapshot> deserialize_binary(std::span<const uint8_t> data) {
    if (data.size() < 16) return std::nullopt;
    if (data.size() > 50 * 1024 * 1024) return std::nullopt; // 50MB limit
    size_t pos = 0;
    auto read_u32 = [&]() -> uint32_t {
      uint32_t v;
      if (pos + 4 > data.size()) return 0;
      std::memcpy(&v, &data[pos], 4);
      pos += 4;
      return v;
    };
    auto read_str = [&]() -> std::string {
      uint32_t len = read_u32();
      if (len > 10 * 1024 * 1024) return ""; // 10MB per string limit
      if (pos + len > data.size()) return "";
      std::string s(reinterpret_cast<const char*>(&data[pos]), len);
      pos += len;
      return s;
    };

    ContextSnapshot snap;
    snap.agent_id = read_u32();
    snap.session_id = read_str();
    uint32_t ms = read_u32();
    snap.captured_at = TimePoint(Duration(ms));
    uint32_t msg_count = read_u32();
    if (msg_count > 100000) return std::nullopt; // Safety limit
    for (uint32_t i = 0; i < msg_count; ++i) {
      if (pos >= data.size()) break;
      kernel::Role role = static_cast<kernel::Role>(data[pos++]);
      std::string content = read_str();
      std::string name = read_str();
      std::string tc_id = read_str();
      snap.messages.push_back({role, std::move(content), std::move(name), std::move(tc_id), {}});
    }
    snap.metadata_json = read_str();
    return snap;
  }
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

  void append(AgentId agent_id, kernel::Message msg, EvictionCallback on_evict = nullptr) {
    auto &win = get_window(agent_id);
    std::lock_guard win_lk(win.mu);
    win.add_evict_if_needed(msg);

    if (!win.evicted().empty() && on_evict) {
      std::vector<kernel::Message> evicted(win.evicted().begin(), win.evicted().end());
      win.clear_evicted();
      on_evict(agent_id, evicted);
    }
  }

  void compress(AgentId agent_id, SummarizeFn summarize) {
    std::vector<kernel::Message> all;
    {
      auto &win = get_window(agent_id);
      std::lock_guard win_lk(win.mu);
      if (!win.is_near_full()) return;
      all.assign(win.messages().begin(), win.messages().end());
    }
    std::string summary = summarize(all);
    {
      auto &win = get_window(agent_id);
      std::lock_guard win_lk(win.mu);
      win.reset();
      win.inject_summary(std::move(summary));
    }
  }

  Result<fs::path> snapshot(AgentId agent_id, const std::string &metadata = "{}") {
    ContextSnapshot snap;
    fs::path path;
    {
      auto &win = get_window(agent_id);
      std::lock_guard win_lk(win.mu);
      snap.agent_id = agent_id;
      snap.session_id = std::to_string(agent_id);
      snap.captured_at = now();
      snap.metadata_json = metadata;
      for (const auto &m : win.messages()) snap.messages.push_back(m);
      path = snapshot_dir_ / fmt::format("agent_{}_snap.bin", agent_id);
    }
    auto binary = snap.serialize_binary();
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return make_error(ErrorCode::MemoryWriteFailed, "Cannot write snapshot");
    ofs.write(reinterpret_cast<const char*>(binary.data()), binary.size());
    return path;
  }

  Result<void> restore(AgentId agent_id) {
    fs::path path = snapshot_dir_ / fmt::format("agent_{}_snap.bin", agent_id);
    if (!fs::exists(path)) return make_error(ErrorCode::NotFound, "Snapshot not found");
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size)) return make_error(ErrorCode::MemoryReadFailed, "Snapshot read failed");
    auto snap = ContextSnapshot::deserialize_binary(buffer);
    if (!snap) return make_error(ErrorCode::MemoryReadFailed, "Snapshot parse failed");
    auto &win = get_window(agent_id);
    std::lock_guard win_lk(win.mu);
    win.reset();
    for (const auto &m : snap->messages) win.try_add(m);
    return {};
  }

  void clear(AgentId agent_id) {
    std::lock_guard lk(map_mu_);
    windows_.erase(agent_id);
  }

private:
  mutable std::shared_mutex map_mu_;
  std::unordered_map<AgentId, std::unique_ptr<ContextWindow>> windows_;
  fs::path snapshot_dir_;
};

} // namespace agentos::context
