#include <agentos/memory/memory.hpp>

#ifndef AGENTOS_NO_DUCKDB
#include <agentos/memory/duckdb_store.hpp>
#endif

#ifndef AGENTOS_NO_SQLITE
#include <agentos/memory/sqlite_store.hpp>
#endif

namespace agentos::memory {

// ─────────────────────────────────────────────────────────────
// WorkingMemory
// ─────────────────────────────────────────────────────────────

Result<std::string> WorkingMemory::write(MemoryEntry entry) {
  std::lock_guard lk(mu_);
  if (entry.id.empty())
    entry.id = "wm_" + std::to_string(id_counter_++);
  entry.created_at = entry.accessed_at = now();

  // LRU 驱逐：确保添加后不超过 capacity
  while (store_.size() >= capacity_ && !store_.empty()) {
    auto oldest = std::min_element(
        store_.begin(), store_.end(), [](const auto &a, const auto &b) {
          return a.second.accessed_at < b.second.accessed_at;
        });
    store_.erase(oldest);
  }
  auto id = entry.id;
  store_[id] = std::move(entry);
  return id;
}

Result<MemoryEntry> WorkingMemory::read(const std::string &id) {
  std::lock_guard lk(mu_);
  auto it = store_.find(id);
  if (it == store_.end())
    return make_error(ErrorCode::NotFound, "WorkingMemory: id not found");
  it->second.accessed_at = now();
  it->second.access_count++;
  return it->second;
}

Result<bool> WorkingMemory::forget(const std::string &id) {
  std::lock_guard lk(mu_);
  return store_.erase(id) > 0;
}

Result<std::vector<SearchResult>> WorkingMemory::search(const Embedding &q_emb,
                                         const MemoryFilter &filter,
                                         size_t top_k) {
  std::lock_guard lk(mu_);
  std::vector<SearchResult> results;
  for (auto &[id, entry] : store_) {
    if (!filter.match(entry.user_id, entry.agent_id, entry.session_id,
                      entry.type))
      continue;
    float score = MemoryEntry::cosine_similarity(q_emb, entry.embedding);
    results.push_back({entry, score});
  }
  std::sort(results.begin(), results.end(),
            [](const auto &a, const auto &b) { return a.score > b.score; });
  if (results.size() > top_k)
    results.resize(top_k);
  return results;
}

// ─────────────────────────────────────────────────────────────
// ShortTermMemory
// ─────────────────────────────────────────────────────────────

Result<std::string> ShortTermMemory::write(MemoryEntry entry) {
  std::lock_guard lk(mu_);
  if (entry.id.empty())
    entry.id = "st_" + std::to_string(id_counter_++);
  entry.created_at = entry.accessed_at = now();

  if (store_.size() >= capacity_) {
    // FIX #21: Deterministic tie-breaking — find entry with lowest score;
    // break ties by oldest created_at first; never evict system entries
    auto victim = store_.begin();
    float min_score = std::numeric_limits<float>::max();
    for (auto it = store_.begin(); it != store_.end(); ++it) {
      if (it->second.source == "system") continue;  // never evict system entries
      float score = it->second.importance /
                    static_cast<float>(it->second.access_count + 1);
      bool better = score < min_score ||
                    (score == min_score &&
                     it->second.created_at < victim->second.created_at);
      if (better) {
        min_score = score;
        victim = it;
      }
    }
    // 从 HNSW 中标记删除（用 find 避免 operator[] 默认插入 BUG）
    if (hnsw_index_) {
      auto label_it = id_to_label_.find(victim->first);
      if (label_it != id_to_label_.end()) {
        hnsw_index_->markDelete(label_it->second);
        label_to_id_.erase(label_it->second);
        id_to_label_.erase(label_it);
      }
    }
    store_.erase(victim);
  }

  auto id = entry.id;

  // 初始化或添加到 HNSW
  if (!entry.embedding.empty()) {
    if (!hnsw_index_) {
      dim_ = entry.embedding.size();
      space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
      hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
          space_.get(), capacity_ * 2, 16, 200);
    }

    // FIX #21: Reject dimension mismatch instead of warning
    if (hnsw_index_ && entry.embedding.size() != dim_) [[unlikely]] {
      return make_error(ErrorCode::InvalidArgument,
                       fmt::format("ShortTermMemory: embedding dimension mismatch: "
                                  "expected {}, got {}",
                                  dim_, entry.embedding.size()));
    }

    if (hnsw_index_ && entry.embedding.size() == dim_) {
      try {
        // 容量不足时动态扩容（与 LTM 一致）
        if (hnsw_index_->cur_element_count >= hnsw_index_->max_elements_) {
          size_t new_cap = hnsw_index_->max_elements_;
          new_cap = (new_cap > SIZE_MAX / 2) ? SIZE_MAX / 2 : new_cap * 2;
          hnsw_index_->resizeIndex(new_cap);
        }
        hnswlib::labeltype label = label_counter_++;
        hnsw_index_->addPoint(entry.embedding.data(), label);
        id_to_label_[id] = label;
        label_to_id_[label] = id;
      } catch (const std::exception &e) {
        LOG_WARN(std::string("ShortTermMemory: HNSW indexing failed: ") + e.what());
      }
    }
  }

  store_[id] = std::move(entry);
  dirty_ = true;
  return id;
}

Result<MemoryEntry> ShortTermMemory::read(const std::string &id) {
  std::lock_guard lk(mu_);
  auto it = store_.find(id);
  if (it == store_.end())
    return make_error(ErrorCode::NotFound, "ShortTermMemory: id not found");
  it->second.accessed_at = now();
  it->second.access_count++;
  return it->second;
}

Result<bool> ShortTermMemory::forget(const std::string &id) {
  std::lock_guard lk(mu_);
  auto it = store_.find(id);
  if (it == store_.end())
    return false;

  if (hnsw_index_) {
    auto label_it = id_to_label_.find(id);
    if (label_it != id_to_label_.end()) {
      hnsw_index_->markDelete(label_it->second);
      label_to_id_.erase(label_it->second);
      id_to_label_.erase(label_it);
      ++deleted_label_count_;
      // Compact HNSW when >50% labels are ghosts
      if (deleted_label_count_ > id_to_label_.size()) {
        compact_hnsw_locked();
      }
    }
  }

  dirty_ = true;
  return store_.erase(id) > 0;
}

Result<std::vector<SearchResult>> ShortTermMemory::search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k) {
  std::lock_guard lk(mu_);
  std::vector<SearchResult> results;

  if (q_emb.empty() || !hnsw_index_ || hnsw_index_->cur_element_count == 0) [[unlikely]] {
    // 退化为线性过滤（无 embedding 或索引为空时）
    for (auto &[id, entry] : store_) {
      if (filter.match(entry.user_id, entry.agent_id, entry.session_id,
                       entry.type)) {
        results.push_back({entry, 0.0f});
      }
    }
    if (results.size() > top_k)
      results.resize(top_k);
    return results;
  }

  if (q_emb.size() != dim_) [[unlikely]] {
    return make_error(ErrorCode::InvalidArgument, "Query dimension mismatch");
  }

  // FIX #3: Take snapshots of label_to_id_ and store_ under lock to prevent
  // use-after-free if another thread calls write() or forget() during search.
  // Keep the lock held throughout searchKnn for thread safety with HNSW.
  auto l2i_copy = label_to_id_;
  auto store_copy = store_;

  CustomFilter custom_filter(filter, std::move(l2i_copy), std::move(store_copy));

  // InnerProductSpace: distance = 1.0 - dot_product
  size_t cur_count =
      static_cast<size_t>(hnsw_index_->cur_element_count.load());
  size_t search_k = std::min(top_k * 4, cur_count);
  if (search_k == 0)
    return results;

  std::priority_queue<std::pair<float, hnswlib::labeltype>> res;
  try {
    res = hnsw_index_->searchKnn(q_emb.data(), search_k, &custom_filter);
  } catch (const std::exception &) {
    return results;
  }

  while (!res.empty()) {
    auto [dist, label] = res.top();
    res.pop();

    auto l2i_it = label_to_id_.find(label);
    if (l2i_it == label_to_id_.end())
      continue;
    auto store_it = store_.find(l2i_it->second);
    if (store_it == store_.end())
      continue;

    float sim = 1.0f - dist; // dot product == cosine similarity
    float score = sim * (0.7f + 0.3f * store_it->second.importance);
    // Update access stats for consolidation (forgetting curve)
    store_it->second.accessed_at = now();
    store_it->second.access_count++;
    results.push_back({store_it->second, score});
  }

  // searchKnn 返回最大堆（最大 distance 在前），反转后按 score 排序
  std::sort(results.begin(), results.end(),
            [](const auto &a, const auto &b) { return a.score > b.score; });

  if (results.size() > top_k)
    results.resize(top_k);
  return results;
}

void ShortTermMemory::compact_hnsw_locked() {
  // FIX #11: Reentrancy guard — skip if compaction already in progress
  if (compacting_) {
    LOG_WARN("[STM] Compaction already in progress — skipping");
    return;
  }
  compacting_ = true;
  struct Guard { bool& flag; ~Guard() { flag = false; } } guard{compacting_};

  if (!hnsw_index_ || id_to_label_.empty()) return;

  // FIX #11: Save old state for atomic rollback on failure
  auto old_space = std::move(space_);
  auto old_hnsw_index = std::move(hnsw_index_);
  auto old_id_to_label = id_to_label_;
  auto old_label_to_id = label_to_id_;
  auto old_label_counter = label_counter_;
  auto old_deleted_count = deleted_label_count_;

  try {
    size_t live_count = old_id_to_label.size();
    auto new_space = std::make_unique<hnswlib::InnerProductSpace>(dim_);
    auto new_index = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        new_space.get(), std::max(live_count * 2, size_t(64)), 16, 200);

    std::unordered_map<std::string, hnswlib::labeltype> new_id_to_label;
    std::unordered_map<hnswlib::labeltype, std::string> new_label_to_id;
    hnswlib::labeltype new_label = 0;

    for (auto &[id, entry] : store_) {
      if (entry.embedding.empty() || entry.embedding.size() != dim_)
        continue;
      new_index->addPoint(entry.embedding.data(), new_label);
      new_id_to_label[id] = new_label;
      new_label_to_id[new_label] = id;
      ++new_label;
    }

    // Commit new state
    space_ = std::move(new_space);
    hnsw_index_ = std::move(new_index);
    id_to_label_ = std::move(new_id_to_label);
    label_to_id_ = std::move(new_label_to_id);
    label_counter_ = new_label;
    deleted_label_count_ = 0;

    LOG_INFO(fmt::format("ShortTermMemory: HNSW compacted to {} live entries", live_count));
  } catch (const std::exception &e) {
    // Rollback to original state on any exception
    LOG_ERROR(fmt::format("[STM] compact failed, rolling back: {}", e.what()));
    space_ = std::move(old_space);
    hnsw_index_ = std::move(old_hnsw_index);
    id_to_label_ = old_id_to_label;
    label_to_id_ = old_label_to_id;
    label_counter_ = old_label_counter;
    deleted_label_count_ = old_deleted_count;
    // Do NOT rethrow — compaction is best-effort
  }
}

// ─────────────────────────────────────────────────────────────
// ShortTermMemory persistence
// ─────────────────────────────────────────────────────────────

Result<void> ShortTermMemory::save(const std::string& dir) {
  std::lock_guard lk(mu_);
  if (!dirty_ || !hnsw_index_) return {};

  fs::create_directories(dir);

  // 1. Save HNSW index
  std::string hnsw_path = dir + "/stm_hnsw.bin";
  std::string hnsw_tmp  = hnsw_path + ".tmp";
  try {
    hnsw_index_->saveIndex(hnsw_tmp);
  } catch (const std::exception& e) {
    return make_error(ErrorCode::MemoryWriteFailed,
                     fmt::format("HNSW save failed: {}", e.what()));
  }
  std::error_code ec;
  fs::rename(hnsw_tmp, hnsw_path, ec);
  if (ec) {
    return make_error(ErrorCode::MemoryWriteFailed,
                     fmt::format("HNSW rename failed: {}", ec.message()));
  }

  // 2. Save metadata as JSON Lines
  std::string meta_path = dir + "/stm_metadata.jsonl";
  std::string meta_tmp  = meta_path + ".tmp";
  {
    std::ofstream ofs(meta_tmp);
    if (!ofs) {
      return make_error(ErrorCode::MemoryWriteFailed, "cannot open metadata file");
    }
    // First line: header with label_counter and dim
    nlohmann::json header;
    header["label_counter"] = label_counter_;
    header["entry_count"]   = store_.size();
    header["dim"]           = dim_;
    ofs << header.dump() << "\n";

    // One line per entry
    for (const auto& [id, entry] : store_) {
      nlohmann::json j;
      j["id"]           = entry.id;
      j["content"]      = entry.content;
      j["source"]       = entry.source;
      j["user_id"]      = entry.user_id;
      j["agent_id"]     = entry.agent_id;
      j["session_id"]   = entry.session_id;
      j["type"]         = entry.type;
      j["importance"]   = entry.importance;
      j["access_count"] = entry.access_count;
      if (auto it = id_to_label_.find(id); it != id_to_label_.end()) {
        j["hnsw_label"] = it->second;
      }
      ofs << j.dump() << "\n";
    }
    if (!ofs.good()) {
      return make_error(ErrorCode::MemoryWriteFailed, "metadata write failed");
    }
  }
  fs::rename(meta_tmp, meta_path, ec);
  if (ec) {
    return make_error(ErrorCode::MemoryWriteFailed,
                     fmt::format("metadata rename failed: {}", ec.message()));
  }

  dirty_ = false;
  return {};
}

Result<void> ShortTermMemory::load(const std::string& dir) {
  std::lock_guard lk(mu_);

  std::string hnsw_path = dir + "/stm_hnsw.bin";
  std::string meta_path = dir + "/stm_metadata.jsonl";

  if (!fs::exists(hnsw_path) || !fs::exists(meta_path)) {
    return {}; // Fresh start
  }

  // 1. Read metadata header first (need dim before loading HNSW)
  std::ifstream ifs(meta_path);
  if (!ifs) {
    return make_error(ErrorCode::MemoryReadFailed, "cannot open metadata file");
  }

  std::string line;
  if (!std::getline(ifs, line)) {
    return make_error(ErrorCode::MemoryReadFailed, "empty metadata file");
  }
  nlohmann::json header;
  try {
    header = nlohmann::json::parse(line);
  } catch (const nlohmann::json::parse_error& e) {
    return make_error(ErrorCode::MemoryReadFailed,
                     fmt::format("metadata header parse failed: {}", e.what()));
  }
  label_counter_ = header.value("label_counter", hnswlib::labeltype{0});
  dim_ = header.value("dim", size_t{0});

  if (dim_ == 0) {
    return make_error(ErrorCode::MemoryReadFailed, "invalid dim in metadata header");
  }

  // 2. Recreate space and load HNSW index
  space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
  try {
    hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        space_.get(), hnsw_path);
  } catch (const std::exception& e) {
    hnsw_index_.reset();
    space_.reset();
    return make_error(ErrorCode::MemoryReadFailed,
                     fmt::format("HNSW load failed: {}", e.what()));
  }

  // 3. Load entry metadata
  store_.clear();
  id_to_label_.clear();
  label_to_id_.clear();

  while (std::getline(ifs, line)) {
    if (line.empty()) continue;
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error& e) {
      return make_error(ErrorCode::MemoryReadFailed,
                       fmt::format("metadata entry parse failed: {}", e.what()));
    }
    MemoryEntry entry;
    entry.id         = j.value("id", "");
    entry.content    = j.value("content", "");
    entry.source     = j.value("source", "");
    entry.user_id    = j.value("user_id", "");
    entry.agent_id   = j.value("agent_id", std::string{});
    entry.session_id = j.value("session_id", "");
    entry.type       = j.value("type", "episodic");
    entry.importance = j.value("importance", 0.0f);
    entry.access_count = j.value("access_count", 0u);

    if (j.contains("hnsw_label")) {
      hnswlib::labeltype label = j["hnsw_label"].get<hnswlib::labeltype>();
      id_to_label_[entry.id] = label;
      label_to_id_[label]    = entry.id;

      // Retrieve embedding from HNSW index
      try {
        entry.embedding = hnsw_index_->getDataByLabel<float>(label);
      } catch (const std::exception&) {
        // Label not found in HNSW — skip embedding recovery
      }
    }

    store_[entry.id] = std::move(entry);
  }

  dirty_ = false;
  return {};
}

// ─────────────────────────────────────────────────────────────
// LongTermMemory
// ─────────────────────────────────────────────────────────────

std::filesystem::path LongTermMemory::entry_path(
      const std::filesystem::path &dir, const std::string &id) {
  // SECURITY: Validate id contains only safe characters to prevent path traversal.
  // Reject anything with '/', '..', '\0', or non-alphanumeric characters except '_' and '-'.
  for (char c : id) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
          // Invalid character in memory entry ID — potential path traversal attempt
          throw std::invalid_argument("LongTermMemory: invalid character in entry id: " + id);
      }
  }
  if (id.empty() || id.size() > 255) {
      throw std::invalid_argument("LongTermMemory: entry id out of valid range");
  }
  return dir / (id + ".mem");
}

Result<std::string> LongTermMemory::write(MemoryEntry entry) {
  entry.created_at = entry.accessed_at = now();

  std::lock_guard lk(mu_);
  if (entry.id.empty())
    entry.id = "lt_" + std::to_string(id_counter_++);

  // 持久化 content 到 .mem 文件（锁内执行，避免并发写同一文件竞态）
  std::filesystem::path path;
  try {
    path = entry_path(entry.id);
  } catch (const std::invalid_argument &e) {
    return make_error(ErrorCode::InvalidArgument, e.what());
  }
  std::ofstream ofs(path);
  if (!ofs)
    return make_error(ErrorCode::MemoryWriteFailed, "LTM: cannot write file");

  // Write metadata fields
  ofs << entry.id << "\n" << entry.source << "\n" << entry.importance << "\n";
  ofs << entry.user_id << "\n"
      << entry.agent_id << "\n"
      << entry.session_id << "\n"
      << entry.type << "\n";

  if (!ofs.good())
    return make_error(ErrorCode::MemoryWriteFailed, "LTM: metadata write failed");

  // Write content (with escape logic)
  for (char c : entry.content) {
    if (c == '\n')
      ofs << "\\n";
    else
      ofs << c;
  }
  ofs << "\n";

  ofs.flush();
  if (!ofs.good())
    return make_error(ErrorCode::MemoryWriteFailed, "LTM: content flush failed (disk full?)");

  hnswlib::labeltype node_label = hnswlib::labeltype(-1);
  bool has_embedding = !entry.embedding.empty();

  if (has_embedding) {
    if (!hnsw_index_) {
      dim_ = entry.embedding.size();
      space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
      hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
          space_.get(), max_elements_, 16, 200);
    } else if (entry.embedding.size() != dim_) {
      has_embedding = false; // 维度不匹配，跳过索引
    }
  }

  if (has_embedding) {
    try {
      // 容量不足时动态扩容
      if (hnsw_index_->cur_element_count >= hnsw_index_->max_elements_) {
        hnsw_index_->resizeIndex(hnsw_index_->max_elements_ * 2);
      }
      node_label = label_counter_++;
      hnsw_index_->addPoint(entry.embedding.data(), node_label);
      label_to_id_[node_label] = entry.id;
    } catch (const std::exception &e) {
      LOG_WARN(std::string("LTM: HNSW indexing failed: ") + e.what());
      has_embedding = false; // 降级：存储条目但不索引向量
    }
  }

  index_[entry.id] = {entry.id,      node_label,     entry.importance,
                      entry.user_id, entry.agent_id, entry.session_id,
                      entry.type};
  index_dirty_ = true; // 延迟落盘，批量写入时避免 O(N^2)
  return entry.id;
}

Result<MemoryEntry> LongTermMemory::read(const std::string &id) {
  if (auto cached = metadata_cache_.get(id)) {
    return *cached;
  }
  std::lock_guard lk(mu_);
  auto res = read_locked(id);
  if (res) {
    metadata_cache_.put(id, *res);
  }
  return res;
}

Result<bool> LongTermMemory::forget(const std::string &id) {
  std::lock_guard lk(mu_);
  std::filesystem::path path;
  try {
    path = entry_path(id);
  } catch (const std::invalid_argument &e) {
    return make_error(ErrorCode::InvalidArgument, e.what());
  }
  if (!fs::exists(path))
    return false;
  fs::remove(path);
  metadata_cache_.remove(id);

  auto it = index_.find(id);
  if (it != index_.end()) {
    if (hnsw_index_ && it->second.label != hnswlib::labeltype(-1)) {
      hnsw_index_->markDelete(it->second.label);
      label_to_id_.erase(it->second.label);
    }
    index_.erase(it);
    save_index_locked();
  }
  return true;
}

Result<std::vector<SearchResult>> LongTermMemory::search(const Embedding &q_emb,
                                         const MemoryFilter &filter,
                                         size_t top_k) {
  std::lock_guard lk(mu_);
  std::vector<SearchResult> results;

  if (q_emb.empty() || !hnsw_index_ || hnsw_index_->cur_element_count == 0) {
    // 退化线性过滤
    for (auto &[id, rec] : index_) {
      if (filter.match(rec.user_id, rec.agent_id, rec.session_id, rec.type)) {
        if (auto entry = read_locked(id))
          results.push_back({*entry, 0.0f});
      }
    }
    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });
    if (results.size() > top_k)
      results.resize(top_k);
    return results;
  }

  if (q_emb.size() != dim_)
    return make_error(ErrorCode::InvalidArgument, "Dim mismatch");

  // 使用持久化反向映射，O(1) 构造过滤器
  LTMFilter ltm_filter(filter, label_to_id_, index_);

  size_t cur_count =
      static_cast<size_t>(hnsw_index_->cur_element_count.load());
  size_t search_k = std::min(top_k * 4, cur_count);
  if (search_k == 0)
    return results;

  std::priority_queue<std::pair<float, hnswlib::labeltype>> res;
  try {
    res = hnsw_index_->searchKnn(q_emb.data(), search_k, &ltm_filter);
  } catch (const std::exception &) {
    return results;
  }

  while (!res.empty()) {
    auto [dist, label] = res.top();
    res.pop();

    auto l2i_it = label_to_id_.find(label);
    if (l2i_it == label_to_id_.end())
      continue;
    auto idx_it = index_.find(l2i_it->second);
    if (idx_it == index_.end())
      continue;

    float sim = 1.0f - dist;
    float score = sim * (0.7f + 0.3f * idx_it->second.importance);

    if (auto entry = read_locked(l2i_it->second)) {
      results.push_back({*entry, score});
    }
  }

  std::sort(results.begin(), results.end(),
            [](const auto &a, const auto &b) { return a.score > b.score; });
  if (results.size() > top_k)
    results.resize(top_k);
  return results;
}

Result<MemoryEntry> LongTermMemory::read_locked(const std::string &id) {
  std::filesystem::path path;
  try {
    path = entry_path(id);
  } catch (const std::invalid_argument &e) {
    return make_error(ErrorCode::InvalidArgument, e.what());
  }
  if (!fs::exists(path))
    return make_error(ErrorCode::NotFound, "LTM: entry not found");

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return make_error(ErrorCode::NotFound, fmt::format("Cannot open memory file: {}", path.string()));
  }
  MemoryEntry entry;
  std::string line;
  std::getline(ifs, entry.id);
  std::getline(ifs, entry.source);
  std::string imp_str;
  std::getline(ifs, imp_str);
  try {
    entry.importance = std::clamp(std::stof(imp_str), 0.0f, 1.0f);
  } catch (const std::exception &) {
    entry.importance = 0.5f; // Default on parse failure
  }
  std::getline(ifs, entry.user_id);
  std::getline(ifs, entry.agent_id);
  std::getline(ifs, entry.session_id);
  std::getline(ifs, entry.type);
  std::getline(ifs, line);
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == 'n') {
      entry.content += '\n';
      i++;
    } else {
      entry.content += line[i];
    }
  }

  auto it = index_.find(id);
  if (it != index_.end() && it->second.label != hnswlib::labeltype(-1) &&
      hnsw_index_) {
    try {
      entry.embedding = hnsw_index_->getDataByLabel<float>(it->second.label);
    } catch (const std::exception &) {
      // Label not found in HNSW — skip embedding recovery
    }
  }

  return entry;
}

void LongTermMemory::handle_corrupt_index(const std::filesystem::path &index_path) {
  auto bad_path = index_path;
  bad_path.replace_extension(".bad");
  std::error_code ec;
  std::filesystem::rename(index_path, bad_path, ec);
  if (!ec) {
    LOG_WARN(fmt::format("[LTM] Corrupt index renamed to {} for inspection. "
                         "Starting with empty index.", bad_path.string()));
  } else {
    LOG_ERROR(fmt::format("[LTM] Cannot rename corrupt index: {}", ec.message()));
  }
}

void LongTermMemory::load_index() {
  // FIX #5: Wrap entire loading block in try-catch for exception safety
  try {
    auto idx_path = dir_ / "index.dat";
    if (!fs::exists(idx_path)) {
      load_ok_ = true; // No index file is acceptable (fresh start)
      return;
    }

    std::ifstream ifs(idx_path);
    std::string line;

    if (std::getline(ifs, line) && line.starts_with("DIM ")) {
      try { dim_ = std::stoull(line.substr(4)); } catch (const std::exception &) {
        load_ok_ = false;
        return;
      }
      if (dim_ > 0) {
        space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
        try {
          auto bin_path = dir_ / "hnsw_index.bin";
          if (fs::exists(bin_path)) {
            hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                space_.get(), bin_path.string());
          } else {
            hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                space_.get(), max_elements_, 16, 200);
          }
        } catch (const std::exception &e) {
          LOG_WARN(std::string("LTM: HNSW load failed: ") + e.what());
          hnsw_index_.reset();
        }
      }
    }

    while (std::getline(ifs, line)) {
      if (line.empty())
        continue;
      IndexRecord rec;
      std::istringstream ls(line);
      ls >> rec.id >> rec.label >> rec.importance >> rec.user_id >>
          rec.agent_id >> rec.session_id >> rec.type;

      if (!ls) {
        throw std::runtime_error("malformed index record: " + line);
      }

      if (rec.user_id == "-")
        rec.user_id = "";
      if (rec.agent_id == "-")
        rec.agent_id = "";
      if (rec.session_id == "-")
        rec.session_id = "";
      if (rec.type == "-")
        rec.type = "";

      if (rec.label != hnswlib::labeltype(-1)) {
        label_counter_ = std::max(label_counter_, rec.label + 1);
        label_to_id_[rec.label] = rec.id; // 恢复反向映射
      }

      // 恢复 id_counter_
      if (rec.id.starts_with("lt_")) {
        try {
          uint64_t seq = std::stoull(rec.id.substr(3));
          id_counter_ = std::max(id_counter_, seq + 1);
        } catch (const std::exception &) {
        }
      }

      index_[rec.id] = std::move(rec);
    }

    load_ok_ = true;
  } catch (const std::exception &e) {
    LOG_ERROR(fmt::format("[LTM] Index load failed: {} — checking for recovery", e.what()));
    auto idx_path = dir_ / "index.dat";
    if (fs::exists(idx_path)) {
      handle_corrupt_index(idx_path);
    }
    // Reset to safe state on failure
    hnsw_index_.reset();
    space_.reset();
    index_.clear();
    label_to_id_.clear();
    load_ok_ = false;
  }
}

void LongTermMemory::save_index_locked() {
  // Write to temp file first, then rename (atomic swap)
  auto idx_path = dir_ / "index.dat";
  auto tmp_path = dir_ / "index.dat.tmp";
  std::ofstream ofs(tmp_path);
  if (!ofs) return; // Can't write, keep dirty flag
  ofs << "DIM " << dim_ << "\n";
  for (auto &[id, rec] : index_) {
    ofs << rec.id << " " << rec.label << " " << rec.importance << " "
        << (rec.user_id.empty() ? "-" : rec.user_id) << " "
        << (rec.agent_id.empty() ? "-" : rec.agent_id) << " "
        << (rec.session_id.empty() ? "-" : rec.session_id) << " "
        << (rec.type.empty() ? "-" : rec.type) << "\n";
  }
  ofs.flush();
  if (!ofs.good()) {
    ofs.close();
    fs::remove(tmp_path);
    return; // Disk full — keep dirty flag, retry later
  }
  ofs.close();
  fs::rename(tmp_path, idx_path);
  index_dirty_ = false;

  if (hnsw_index_) {
    hnsw_index_->saveIndex((dir_ / "hnsw_index.bin").string());
  }
}

// ─────────────────────────────────────────────────────────────
// MemorySystem
// ─────────────────────────────────────────────────────────────

MemorySystem::MemorySystem(fs::path ltm_dir, LTMBackend backend)
    : working_(std::make_unique<WorkingMemory>(32)),
      short_term_(std::make_unique<ShortTermMemory>(512)),
      graph_(std::make_unique<LocalGraphMemory>(ltm_dir.empty() ? fs::temp_directory_path() / "agentos_ltm" : ltm_dir)) {
  // LOW PRIORITY: Use temp directory if ltm_dir is empty
  if (ltm_dir.empty()) {
    ltm_dir = fs::temp_directory_path() / "agentos_ltm";
  }
  ltm_dir_ = ltm_dir;
  // 根据后端类型创建 LTM（都实现 IMemoryStore 接口，可热切换）
#ifndef AGENTOS_NO_DUCKDB
  if (backend == LTMBackend::DuckDB) {
    long_term_ = std::make_unique<DuckDBLongTermMemory>(std::move(ltm_dir));
  } else
#endif
#ifndef AGENTOS_NO_SQLITE
  if (backend == LTMBackend::SQLite) {
    long_term_ = std::make_unique<SQLiteLongTermMemory>(std::move(ltm_dir));
  } else
#endif
  {
    (void)backend;  // suppress unused warning when both disabled
    long_term_ = std::make_unique<LongTermMemory>(std::move(ltm_dir));
  }
  load_indexes();
}

Result<std::string> MemorySystem::remember(std::string content, const Embedding &emb,
                             std::string source,
                             float importance,
                             MemoryFilter filter) {
  MemoryEntry entry;
  entry.content = std::move(content);
  entry.embedding = emb;
  entry.source = std::move(source);
  entry.importance = importance;
  entry.user_id = filter.user_id.value_or("");
  entry.agent_id = filter.agent_id.value_or("");
  entry.session_id = filter.session_id.value_or("");
  entry.type = filter.type.value_or("episodic");

  // L0: Working memory (STM) — this is the primary write
  auto r0 = working_->write(entry);
  if (!r0) {
      return make_error(r0.error().code,
          fmt::format("remember: L0 write failed: {}", r0.error().message));
  }

  // L1: Short-term memory — best-effort, log failure
  if (short_term_) {
      auto r1 = short_term_->write(entry);
      if (!r1) {
          LOG_WARN(fmt::format("[Memory] remember: L1 write failed (id={}): {}",
                               r0.value(), r1.error().message));
      }
  }

  // L2: Long-term memory — best-effort, log failure
  if (long_term_ && importance > 0.7f) {
      auto r2 = long_term_->write(entry);
      if (!r2) {
          LOG_WARN(fmt::format("[Memory] remember: L2 write failed (id={}): {}",
                               r0.value(), r2.error().message));
      }
  }

  return r0;  // Return L0 ID
}

Result<std::vector<SearchResult>> MemorySystem::recall(const Embedding &q_emb,
                                         const MemoryFilter &filter,
                                         size_t top_k) {
  std::vector<SearchResult> results;
  std::unordered_set<std::string> seen_ids; // O(1) 去重

  auto merge = [&](Result<std::vector<SearchResult>> &r) {
    if (!r)
      return;
    for (auto &sr : *r) {
      if (seen_ids.insert(sr.entry.id).second) {
        results.push_back(std::move(sr));
      }
    }
  };

  // L0 search with timing
  auto t0_start = std::chrono::steady_clock::now();
  auto r0 = working_->search(q_emb, filter, top_k);
  auto t0_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0_start).count();
  if (t0_ms > 100) {
    LOG_WARN(fmt::format("[Memory] L0 search slow: {}ms", t0_ms));
  }
  merge(r0);

  // L1 short-term memory search with timing
  auto t1_start = std::chrono::steady_clock::now();
  auto r1 = short_term_->search(q_emb, filter, top_k);
  auto t1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t1_start).count();
  if (t1_ms > 100) {
    LOG_WARN(fmt::format("[Memory] L1 search slow: {}ms", t1_ms));
  }
  merge(r1);

  // L2 long-term memory search with timing
  auto t2_start = std::chrono::steady_clock::now();
  auto r2 = long_term_->search(q_emb, filter, top_k);
  auto t2_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t2_start).count();
  if (t2_ms > 100) {
    LOG_WARN(fmt::format("[Memory] L2 search slow: {}ms", t2_ms));
  }
  merge(r2);

  // 全局排序取 Top-K
  std::sort(results.begin(), results.end(),
            [](const auto &a, const auto &b) { return a.score > b.score; });
  if (results.size() > top_k)
    results.resize(top_k);
  return results;
}

size_t MemorySystem::consolidate(float importance_threshold) {
  auto r = working_->get_all();
  size_t promoted = 0;
  for (auto &entry : r) {
    if (entry.importance >= importance_threshold) {
      (void)long_term_->write(entry);
      ++promoted;
    }
  }
  // Flush LTM index once after batch write (avoids O(N^2) index saves)
  if (promoted > 0) {
    if (auto *ltm = dynamic_cast<LongTermMemory *>(long_term_.get()))
      ltm->flush();
  }
  return promoted;
}

bool MemorySystem::consolidate(std::chrono::milliseconds timeout, float importance_threshold) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto r = working_->get_all();
  size_t promoted = 0;
  for (auto &entry : r) {
    if (std::chrono::steady_clock::now() > deadline) {
      // Timeout exceeded; partial promotion is acceptable
      if (promoted > 0) {
        if (auto *ltm = dynamic_cast<LongTermMemory *>(long_term_.get()))
          ltm->flush();
      }
      return false; // Indicate timeout
    }
    if (entry.importance >= importance_threshold) {
      (void)long_term_->write(entry);
      ++promoted;
    }
  }
  // Flush LTM index once after batch write
  if (promoted > 0) {
    if (auto *ltm = dynamic_cast<LongTermMemory *>(long_term_.get()))
      ltm->flush();
  }
  return true; // Completed successfully
}

} // namespace agentos::memory
