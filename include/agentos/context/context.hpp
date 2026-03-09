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
#include <sstream>
#include <unordered_map>

namespace agentos::context {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// § 3.1  ContextWindow — Token 预算管理器
// ─────────────────────────────────────────────────────────────

/// @note Not thread-safe. Callers must synchronize externally (e.g. via ContextManager).
class ContextWindow {
public:
  explicit ContextWindow(TokenCount max_tokens = 8192)
      : max_tokens_(max_tokens) {}

  // 尝试添加消息；若超预算返回 false
  bool try_add(const kernel::Message &msg) {
    TokenCount cost =
        kernel::ILLMBackend::estimate_tokens(msg.content) + 4; // 角色 overhead
    if (used_tokens_ + cost > max_tokens_)
      return false;
    messages_.push_back(msg);
    used_tokens_ += cost;
    return true;
  }

  // 强制添加（可能触发驱逐）
  // 驱逐策略：优先驱逐 importance 最低的非 system 消息
  // importance: System > Tool/Assistant > User（越老越低优先）
  void add_evict_if_needed(const kernel::Message &msg) {
    TokenCount cost = kernel::ILLMBackend::estimate_tokens(msg.content) + 4;
    while (!messages_.empty() && used_tokens_ + cost > max_tokens_) {
      // 找到最佳驱逐候选：非 system、importance 最低
      // 优化：先快速扫描前半部分（老消息），因为 score = role_w + age*0.01
      // 越老的非 system 消息得分越低，大多数情况下 victim 在前半段
      auto best = messages_.end();
      float best_score = std::numeric_limits<float>::max();
      float age_factor = 0.0f;
      size_t pos = 0;
      for (auto it = messages_.begin(); it != messages_.end(); ++it, ++pos) {
        if (it->role == kernel::Role::System) {
          age_factor += 0.01f;
          continue;
        }
        float role_w = (it->role == kernel::Role::Tool) ? 0.3f :
                       (it->role == kernel::Role::Assistant) ? 0.5f : 0.1f;
        float score = role_w + age_factor;
        if (score < best_score) {
          best_score = score;
          best = it;
          // Early exit: User role at early position can't be beaten by later messages
          if (it->role == kernel::Role::User && pos < messages_.size() / 2)
            break;
        }
        age_factor += 0.01f;
      }
      if (best == messages_.end())
        break; // 全是 system 消息
      evicted_.push_back(*best);
      used_tokens_ -= kernel::ILLMBackend::estimate_tokens(best->content) + 4;
      messages_.erase(best);
    }
    messages_.push_back(msg);
    used_tokens_ += cost;
  }

  const std::deque<kernel::Message> &messages() const { return messages_; }
  const std::deque<kernel::Message> &evicted() const { return evicted_; }
  size_t message_count() const { return messages_.size(); }
  TokenCount used_tokens() const { return used_tokens_; }
  TokenCount max_tokens() const { return max_tokens_; }
  float utilization() const {
    return static_cast<float>(used_tokens_) / max_tokens_;
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

  // 注入一段摘要，替换已驱逐的历史
  void inject_summary(std::string summary_content) {
    messages_.push_front(
        kernel::Message::system("=== 历史对话摘要 ===\n" + std::move(summary_content)));
    used_tokens_ = 0;
    for (auto &m : messages_)
      used_tokens_ += kernel::ILLMBackend::estimate_tokens(m.content) + 4;
  }

private:
  TokenCount max_tokens_;
  TokenCount used_tokens_{0};
  std::deque<kernel::Message> messages_;
  std::deque<kernel::Message> evicted_; // 被驱逐的消息，等待持久化
};

// ─────────────────────────────────────────────────────────────
// § 3.2  ContextSnapshot — 序列化/反序列化
// ─────────────────────────────────────────────────────────────

struct ContextSnapshot {
  AgentId agent_id;
  SessionId session_id;
  TimePoint captured_at;
  std::vector<kernel::Message> messages;
  std::string metadata_json; // 任意扩展元数据

  // 序列化为文本（简单格式，生产可换 protobuf/msgpack）
  std::string serialize() const {
    std::string out;
    out += fmt::format("SNAP:agent={},session={}\n", agent_id, session_id);
    for (auto &m : messages) {
      int role_id = static_cast<int>(m.role);
      // 转义换行和特殊字符
      std::string escaped;
      for (char c : m.content) {
        if (c == '\n')
          escaped += "\\n";
        else if (c == '\r')
          escaped += "\\r";
        else if (c == '\\')
          escaped += "\\\\";
        else
          escaped += c;
      }
      out += fmt::format("MSG:{}:{}\n", role_id, escaped);
    }
    out += "META:" + metadata_json + "\n";
    out += "END\n";
    return out;
  }

  static std::optional<ContextSnapshot> deserialize(const std::string &data) {
    ContextSnapshot snap;
    snap.captured_at = now();
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
      if (line.starts_with("SNAP:")) {
        auto body = line.substr(5);
        auto agent_pos = body.find("agent=");
        auto sess_pos = body.find(",session=");
        if (agent_pos != std::string::npos) {
          auto agent_end = body.find(',', agent_pos);
          auto agent_val = body.substr(agent_pos + 6,
              agent_end != std::string::npos ? agent_end - agent_pos - 6 : std::string::npos);
          try { snap.agent_id = std::stoull(agent_val); } catch (const std::exception &) { continue; }
        }
        if (sess_pos != std::string::npos)
          snap.session_id = body.substr(sess_pos + 9);
      } else if (line.starts_with("MSG:")) {
        auto rest = line.substr(4);
        auto colon = rest.find(':');
        if (colon == std::string::npos)
          continue;
        int role_id;
        try { role_id = std::stoi(rest.substr(0, colon)); } catch (const std::exception &) { continue; }
        if (role_id < 0 || role_id > 3) // Role enum: System=0, User=1, Assistant=2, Tool=3
          continue;
        std::string content;
        // 反转义
        bool escape = false;
        for (char c : rest.substr(colon + 1)) {
          if (escape) {
            if (c == 'n')
              content += '\n';
            else if (c == 'r')
              content += '\r';
            else
              content += c;
            escape = false;
          } else if (c == '\\') {
            escape = true;
          } else {
            content += c;
          }
        }
        snap.messages.push_back(
            {.role = static_cast<kernel::Role>(role_id), .content = content});
      } else if (line.starts_with("META:")) {
        snap.metadata_json = line.substr(5);
      }
    }
    return snap;
  }
};

// ─────────────────────────────────────────────────────────────
// § 3.3  ContextManager — 虚拟上下文管理门面
// ─────────────────────────────────────────────────────────────

// 换页策略回调（将被驱逐的消息写入长期存储）
using EvictionCallback =
    std::function<void(AgentId, const std::vector<kernel::Message> &)>;
// 摘要生成回调（可由 LLM 实现）
using SummarizeFn =
    std::function<std::string(const std::vector<kernel::Message> &)>;

class ContextManager : private NonCopyable {
public:
  explicit ContextManager(fs::path snapshot_dir = "/tmp/agentos_snapshots")
      : snapshot_dir_(std::move(snapshot_dir)) {
    fs::create_directories(snapshot_dir_);
  }

  // 获取或创建某 Agent 的上下文窗口
  ContextWindow &get_window(AgentId agent_id, TokenCount max_tokens = 8192) {
    std::lock_guard lk(mu_);
    auto [it, inserted] = windows_.emplace(agent_id, ContextWindow{max_tokens});
    return it->second;
  }

  // 向上下文追加消息，自动换页
  void append(AgentId agent_id, kernel::Message msg,
              EvictionCallback on_evict = nullptr) {
    std::lock_guard lk(mu_);
    auto it = windows_.find(agent_id);
    if (it == windows_.end()) {
      // 窗口不存在时自动创建（默认 8192 token）
      it = windows_.emplace(agent_id, ContextWindow{8192}).first;
    }
    auto &win = it->second;
    win.add_evict_if_needed(msg);

    // 若有驱逐，触发回调
    if (!win.evicted().empty() && on_evict) {
      std::vector<kernel::Message> evicted(win.evicted().begin(), win.evicted().end());
      win.clear_evicted();
      on_evict(agent_id, evicted);
    }
  }

  // 当上下文接近满时，用摘要替换历史
  void compress(AgentId agent_id, SummarizeFn summarize) {
    std::vector<kernel::Message> all;

    // Phase 1: collect messages under lock
    {
      std::lock_guard lk(mu_);
      auto it = windows_.find(agent_id);
      if (it == windows_.end())
        return;
      auto &win = it->second;
      if (!win.is_near_full())
        return;
      all.assign(win.messages().begin(), win.messages().end());
    }

    // Phase 2: call summarize callback WITHOUT lock (may call LLM)
    std::string summary = summarize(all);

    // Phase 3: apply summary under lock
    {
      std::lock_guard lk(mu_);
      auto it = windows_.find(agent_id);
      if (it == windows_.end())
        return;
      it->second.reset();
      it->second.inject_summary(std::move(summary));
    }
  }

  // 快照：将 Agent 上下文序列化到磁盘
  Result<fs::path> snapshot(AgentId agent_id,
                            const std::string &metadata = "{}") {
    // Phase 1: collect snapshot data under lock
    ContextSnapshot snap;
    fs::path path;
    {
      std::lock_guard lk(mu_);
      auto it = windows_.find(agent_id);
      if (it == windows_.end())
        return make_error(ErrorCode::NotFound,
                          fmt::format("No context for agent {}", agent_id));

      snap.agent_id = agent_id;
      snap.session_id = std::to_string(agent_id);
      snap.captured_at = now();
      snap.metadata_json = metadata;
      for (auto &m : it->second.messages())
        snap.messages.push_back(m);
      path = snapshot_dir_ / fmt::format("agent_{}_snap.txt", agent_id);
    }

    // Phase 2: file I/O outside lock
    std::ofstream ofs(path);
    if (!ofs)
      return make_error(
          ErrorCode::MemoryWriteFailed,
          fmt::format("Cannot write snapshot to {}", path.string()));
    ofs << snap.serialize();
    ofs.flush();
    if (!ofs.good())
      return make_error(
          ErrorCode::MemoryWriteFailed,
          fmt::format("Write failed (disk full?) for {}", path.string()));
    return path;
  }

  // 从磁盘恢复上下文（断点续传）
  Result<void> restore(AgentId agent_id) {
    // Phase 1: File I/O outside lock to avoid blocking other agents
    fs::path path;
    {
      std::lock_guard lk(mu_);
      path = snapshot_dir_ / fmt::format("agent_{}_snap.txt", agent_id);
    }

    if (!fs::exists(path))
      return make_error(ErrorCode::NotFound,
                        fmt::format("Snapshot not found: {}", path.string()));

    std::ifstream ifs(path);
    std::string data((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    auto snap = ContextSnapshot::deserialize(data);
    if (!snap)
      return make_error(ErrorCode::MemoryReadFailed, "Snapshot parse failed");

    // Phase 2: Apply under lock
    std::lock_guard lk(mu_);
    auto &win = windows_.emplace(agent_id, ContextWindow{8192}).first->second;
    win.reset();
    for (auto &m : snap->messages)
      win.try_add(m);
    return {};
  }

  // 清除 Agent 上下文
  void clear(AgentId agent_id) {
    std::lock_guard lk(mu_);
    windows_.erase(agent_id);
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<AgentId, ContextWindow> windows_;
  fs::path snapshot_dir_;
};

} // namespace agentos::context
