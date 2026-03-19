#include <agentos/context/context.hpp>
#include <fstream>
#include <limits>

namespace agentos::context {

// ─────────────────────────────────────────────────────────────
// ContextWindow
// ─────────────────────────────────────────────────────────────

void ContextWindow::add_evict_if_needed(const kernel::Message &msg) {
  TokenCount cost = msg.tokens();
  while (!messages_.empty() && used_tokens_ + cost > max_tokens_) {
    // R5-6: Fast heuristic-based eviction.
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
      if (i > 128) break;
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

    if (evict_idx == std::string::npos) {
      for (size_t i = 0; i < messages_.size(); ++i) {
        if (messages_[i].role != kernel::Role::System) { evict_idx = i; break; }
      }
    }

    if (evict_idx == std::string::npos || evict_idx >= messages_.size()) {
      LOG_WARN(fmt::format("[Context] Cannot evict system messages. Budget may overrun."));
      break;
    }
    evicted_.push_back(std::move(messages_[evict_idx]));
    used_tokens_ -= (evicted_.back()).tokens();
    messages_.erase(messages_.begin() + evict_idx);
  }
  // Guard: don't add if budget still exceeded after eviction attempts
  if (used_tokens_ + cost > max_tokens_ && !messages_.empty()) {
    LOG_WARN("[Context] Budget exhausted after eviction attempt, dropping message");
    return;
  }
  messages_.push_back(msg);
  used_tokens_ += cost;
}

void ContextWindow::inject_summary(std::string summary_content) {
  messages_.push_front(
      kernel::Message::system("=== 历史对话摘要 ===\n" + std::move(summary_content)));
  used_tokens_ = 0;
  for (auto &m : messages_)
    used_tokens_ += m.tokens();
}

// ─────────────────────────────────────────────────────────────
// ContextSnapshot
// ─────────────────────────────────────────────────────────────

std::vector<uint8_t> ContextSnapshot::serialize_binary() const {
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
  append_u32(static_cast<uint32_t>(captured_at.time_since_epoch().count() / 1000000));
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

std::optional<ContextSnapshot> ContextSnapshot::deserialize_binary(std::span<const uint8_t> data) {
  if (data.size() < 16) return std::nullopt;
  if (data.size() > 50 * 1024 * 1024) return std::nullopt;
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
    if (len > 10 * 1024 * 1024) return "";
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
  if (msg_count > 100000) return std::nullopt;
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

// ─────────────────────────────────────────────────────────────
// ContextManager
// ─────────────────────────────────────────────────────────────

void ContextManager::append(AgentId agent_id, kernel::Message msg, EvictionCallback on_evict) {
  auto &win = get_window(agent_id);
  std::lock_guard win_lk(win.mu);
  win.add_evict_if_needed(msg);

  if (!win.evicted().empty() && on_evict) {
    std::vector<kernel::Message> evicted(win.evicted().begin(), win.evicted().end());
    win.clear_evicted();
    on_evict(agent_id, evicted);
  }
}

void ContextManager::compress(AgentId agent_id, SummarizeFn summarize) {
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

Result<fs::path> ContextManager::snapshot(AgentId agent_id, const std::string &metadata) {
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

Result<void> ContextManager::restore(AgentId agent_id) {
  fs::path path = snapshot_dir_ / fmt::format("agent_{}_snap.bin", agent_id);
  if (!fs::exists(path)) return make_error(ErrorCode::NotFound, "Snapshot not found");
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  std::streamsize size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(size);
  if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size))
    return make_error(ErrorCode::MemoryReadFailed, "Snapshot read failed");
  auto snap = ContextSnapshot::deserialize_binary(buffer);
  if (!snap) return make_error(ErrorCode::MemoryReadFailed, "Snapshot parse failed");
  auto &win = get_window(agent_id);
  std::lock_guard win_lk(win.mu);
  win.reset();
  for (const auto &m : snap->messages) win.try_add(m);
  return {};
}

// ─────────────────────────────────────────────────────────────
// SessionState
// ─────────────────────────────────────────────────────────────

std::vector<uint8_t> SessionState::serialize_binary() const {
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

  // Magic + version
  buf.push_back('S'); buf.push_back('E'); buf.push_back('S'); buf.push_back('S');
  append_u32(1); // version

  append_u32(static_cast<uint32_t>(agent_id));
  append_str(session_id);
  append_u32(static_cast<uint32_t>(saved_at.time_since_epoch().count() / 1000000));

  // Config JSON
  append_str(config_json);

  // Middleware names
  append_u32(static_cast<uint32_t>(middleware_names.size()));
  for (const auto& name : middleware_names) {
    append_str(name);
  }

  // Embedded context snapshot
  auto ctx_bin = context.serialize_binary();
  append_u32(static_cast<uint32_t>(ctx_bin.size()));
  buf.insert(buf.end(), ctx_bin.begin(), ctx_bin.end());

  // Metadata
  append_str(metadata_json);

  return buf;
}

std::optional<SessionState> SessionState::deserialize_binary(std::span<const uint8_t> data) {
  if (data.size() < 16) return std::nullopt;
  if (data.size() > 50 * 1024 * 1024) return std::nullopt;

  // Check magic
  if (data[0] != 'S' || data[1] != 'E' || data[2] != 'S' || data[3] != 'S')
    return std::nullopt;

  size_t pos = 4;
  auto read_u32 = [&]() -> uint32_t {
    uint32_t v;
    if (pos + 4 > data.size()) return 0;
    std::memcpy(&v, &data[pos], 4);
    pos += 4;
    return v;
  };
  auto read_str = [&]() -> std::string {
    uint32_t len = read_u32();
    if (len > 10 * 1024 * 1024) return "";
    if (pos + len > data.size()) return "";
    std::string s(reinterpret_cast<const char*>(&data[pos]), len);
    pos += len;
    return s;
  };

  uint32_t version = read_u32();
  if (version != 1) return std::nullopt;

  SessionState state;
  state.agent_id = read_u32();
  state.session_id = read_str();
  uint32_t ms = read_u32();
  state.saved_at = TimePoint(Duration(ms));

  state.config_json = read_str();

  uint32_t mw_count = read_u32();
  if (mw_count > 1000) return std::nullopt;
  for (uint32_t i = 0; i < mw_count; ++i) {
    state.middleware_names.push_back(read_str());
  }

  // Embedded context snapshot
  uint32_t ctx_len = read_u32();
  if (ctx_len > 50 * 1024 * 1024 || pos + ctx_len > data.size()) return std::nullopt;
  std::span<const uint8_t> ctx_data(data.data() + pos, ctx_len);
  auto ctx_snap = ContextSnapshot::deserialize_binary(ctx_data);
  if (!ctx_snap) return std::nullopt;
  state.context = std::move(*ctx_snap);
  pos += ctx_len;

  state.metadata_json = read_str();
  return state;
}

// ─────────────────────────────────────────────────────────────
// ContextManager — Session methods
// ─────────────────────────────────────────────────────────────

Result<fs::path> ContextManager::save_session(AgentId agent_id, const std::string& config_json,
                                               const std::vector<std::string>& middleware_names,
                                               const std::string& metadata) {
  SessionState state;
  {
    auto &win = get_window(agent_id);
    std::lock_guard win_lk(win.mu);
    state.agent_id = agent_id;
    state.session_id = fmt::format("{}_{}_{}", agent_id,
        std::chrono::duration_cast<Duration>(now().time_since_epoch()).count(),
        session_seq_.fetch_add(1, std::memory_order_relaxed));
    state.saved_at = now();
    state.config_json = config_json;
    state.middleware_names = middleware_names;
    state.context.agent_id = agent_id;
    state.context.session_id = state.session_id;
    state.context.captured_at = state.saved_at;
    for (const auto &m : win.messages()) state.context.messages.push_back(m);
    state.context.metadata_json = metadata;
    state.metadata_json = metadata;
  }

  auto binary = state.serialize_binary();
  fs::path path = snapshot_dir_ / fmt::format("session_{}_{}.bin", agent_id, state.session_id);

  // Atomic write: temp file + rename
  fs::path tmp_path = path;
  tmp_path += ".tmp";
  {
    std::ofstream ofs(tmp_path, std::ios::binary);
    if (!ofs) return make_error(ErrorCode::MemoryWriteFailed, "Cannot write session file");
    ofs.write(reinterpret_cast<const char*>(binary.data()),
              static_cast<std::streamsize>(binary.size()));
    if (!ofs.good()) return make_error(ErrorCode::MemoryWriteFailed, "Session write incomplete");
  }
  std::error_code ec;
  fs::rename(tmp_path, path, ec);
  if (ec) return make_error(ErrorCode::MemoryWriteFailed, "Session rename failed: " + ec.message());

  return path;
}

Result<SessionState> ContextManager::load_session(AgentId agent_id, const SessionId& session_id) {
  fs::path path = snapshot_dir_ / fmt::format("session_{}_{}.bin", agent_id, session_id);
  if (!fs::exists(path)) return make_error(ErrorCode::NotFound, "Session not found");

  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) return make_error(ErrorCode::MemoryReadFailed, "Cannot open session file");

  auto size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!ifs.read(reinterpret_cast<char*>(buffer.data()), size))
    return make_error(ErrorCode::MemoryReadFailed, "Session read failed");

  auto state = SessionState::deserialize_binary(buffer);
  if (!state) return make_error(ErrorCode::MemoryReadFailed, "Session parse failed");
  return std::move(*state);
}

Result<std::vector<SessionId>> ContextManager::list_sessions(AgentId agent_id) const {
  std::vector<SessionId> sessions;
  std::string prefix = fmt::format("session_{}_", agent_id);

  if (!fs::exists(snapshot_dir_)) return sessions;

  for (const auto& entry : fs::directory_iterator(snapshot_dir_)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    if (name.starts_with(prefix) && name.ends_with(".bin")) {
      // Extract session_id: "session_{agent_id}_{session_id}.bin"
      std::string session_id = name.substr(prefix.size(),
                                           name.size() - prefix.size() - 4);
      sessions.push_back(session_id);
    }
  }
  return sessions;
}

} // namespace agentos::context
