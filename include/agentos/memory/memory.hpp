#pragma once
// ============================================================
// AgentOS :: Module 4 — Memory System
// 工作记忆 / 短期记忆 / 长期记忆，统一 IMemoryStore 接口
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graph_memory.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <hnswlib/hnswlib.h>
#pragma GCC diagnostic pop

namespace agentos::memory {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// § 4.1  MemoryEntry — 原子记忆单元
// ─────────────────────────────────────────────────────────────

using Embedding = std::vector<float>; // 语义向量（维度由后端决定）

struct MemoryFilter {
  std::optional<std::string> user_id;
  std::optional<std::string> agent_id;
  std::optional<std::string> session_id;
  std::optional<std::string> type;

  bool match(const std::string &u, const std::string &a, const std::string &s,
             const std::string &t) const {
    if (user_id && *user_id != u)
      return false;
    if (agent_id && *agent_id != a)
      return false;
    if (session_id && *session_id != s)
      return false;
    if (type && *type != t)
      return false;
    return true;
  }
};

struct MemoryEntry {
  std::string id;
  std::string content;
  std::string source; // 产生来源（agent_id / tool name）

  // Scope metadata
  std::string user_id;
  std::string agent_id;
  std::string session_id;
  std::string type{"episodic"};

  TimePoint created_at;
  TimePoint accessed_at;
  uint32_t access_count{0};
  float importance{0.5f}; // 0~1
  Embedding embedding;
  std::unordered_map<std::string, std::string> tags;
  // FIX #17: Precondition: both vectors must be non-empty and same size.
  // Returns normalized cosine similarity (assumes inputs are L2-normalized).
  static float cosine_similarity(const Embedding &a, const Embedding &b) {
    // Runtime guards that work in release builds
    if (a.empty() || b.empty()) return 0.0f;
    if (a.size() != b.size()) return 0.0f;
    float dot = std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
    return dot; // 已归一化时 dot == cosine
  }
};

// ─────────────────────────────────────────────────────────────
// § 4.2  IMemoryStore 接口
// ─────────────────────────────────────────────────────────────

struct SearchResult {
  MemoryEntry entry;
  float score; // 相关性得分
};

class IMemoryStore {
public:
  virtual ~IMemoryStore() = default;

  [[nodiscard]] virtual Result<std::string> write(MemoryEntry entry) = 0;
  [[nodiscard]] virtual Result<MemoryEntry> read(const std::string &id) = 0;
  [[nodiscard]] virtual Result<bool> forget(const std::string &id) = 0;

  // 语义检索 Top-K
  [[nodiscard]] virtual Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                                   const MemoryFilter &filter,
                                                   size_t top_k = 5) = 0;

  // 获取所有记忆
  virtual std::vector<MemoryEntry> get_all() = 0;

  virtual size_t size() const noexcept = 0;
  virtual std::string name() const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// § 4.3  WorkingMemory — 当前窗口内的即时记忆（最快）
// ─────────────────────────────────────────────────────────────

class WorkingMemory : public IMemoryStore {
public:
  explicit WorkingMemory(size_t capacity = 32) : capacity_(capacity) {}

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override {
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

  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    auto it = store_.find(id);
    if (it == store_.end())
      return make_error(ErrorCode::NotFound, "WorkingMemory: id not found");
    it->second.accessed_at = now();
    it->second.access_count++;
    return it->second;
  }

  [[nodiscard]] Result<bool> forget(const std::string &id) override {
    std::lock_guard lk(mu_);
    return store_.erase(id) > 0;
  }

  [[nodiscard]] Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
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

  std::vector<MemoryEntry> get_all() override {
    std::lock_guard lk(mu_);
    std::vector<MemoryEntry> results;
    results.reserve(store_.size());
    for (const auto &[id, entry] : store_)
      results.push_back(entry);
    return results;
  }

  size_t size() const noexcept override {
    std::lock_guard lk(mu_);
    return store_.size();
  }
  std::string name() const noexcept override { return "WorkingMemory"; }
  void clear() {
    std::lock_guard lk(mu_);
    store_.clear();
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, MemoryEntry> store_;
  size_t capacity_;
  uint64_t id_counter_{0};
};

// ─────────────────────────────────────────────────────────────
// § 4.4  ShortTermMemory — Session 内共享记忆（中速）
// ─────────────────────────────────────────────────────────────

class ShortTermMemory : public IMemoryStore {
public:
  explicit ShortTermMemory(size_t capacity = 512) : capacity_(capacity) {}

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override {
    std::lock_guard lk(mu_);
    if (entry.id.empty())
      entry.id = "st_" + std::to_string(id_counter_++);
    entry.created_at = entry.accessed_at = now();

    if (store_.size() >= capacity_) {
      // 驱逐重要性最低、最久未访问的条目
      auto victim = std::min_element(
          store_.begin(), store_.end(), [](const auto &a, const auto &b) {
            float score_a =
                a.second.importance / (1.0f + a.second.access_count);
            float score_b =
                b.second.importance / (1.0f + b.second.access_count);
            return score_a < score_b;
          });
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
      if (hnsw_index_ && entry.embedding.size() != dim_) {
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
    return id;
  }

  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    auto it = store_.find(id);
    if (it == store_.end())
      return make_error(ErrorCode::NotFound, "ShortTermMemory: id not found");
    it->second.accessed_at = now();
    it->second.access_count++;
    return it->second;
  }

  [[nodiscard]] Result<bool> forget(const std::string &id) override {
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

    return store_.erase(id) > 0;
  }

  // 自定义 HNSW 过滤器：scope 匹配 + 已删除检查
  // FIX #3: Capture data by value to prevent use-after-free
  class CustomFilter : public hnswlib::BaseFilterFunctor {
  public:
    CustomFilter(
        const MemoryFilter &filter,
        std::unordered_map<hnswlib::labeltype, std::string> label_to_id,
        std::unordered_map<std::string, MemoryEntry> store)
        : flt(filter), l2i(std::move(label_to_id)), st(std::move(store)) {}

    bool operator()(hnswlib::labeltype label) override {
      auto id_it = l2i.find(label);
      if (id_it == l2i.end())
        return false;
      auto entry_it = st.find(id_it->second);
      if (entry_it == st.end())
        return false;
      auto &entry = entry_it->second;
      return flt.match(entry.user_id, entry.agent_id, entry.session_id,
                       entry.type);
    }

  private:
    const MemoryFilter &flt;
    // Store by value to allow safe copying of data from search()
    std::unordered_map<hnswlib::labeltype, std::string> l2i;
    std::unordered_map<std::string, MemoryEntry> st;
  };

  Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
    std::lock_guard lk(mu_);
    std::vector<SearchResult> results;

    if (q_emb.empty() || !hnsw_index_ || hnsw_index_->cur_element_count == 0) {
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

    if (q_emb.size() != dim_) {
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
      results.push_back({store_it->second, score});
    }

    // searchKnn 返回最大堆（最大 distance 在前），反转后按 score 排序
    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });

    if (results.size() > top_k)
      results.resize(top_k);
    return results;
  }

  std::vector<MemoryEntry> get_all() override {
    std::lock_guard lk(mu_);
    std::vector<MemoryEntry> results;
    results.reserve(store_.size());
    for (const auto &[id, entry] : store_)
      results.push_back(entry);
    return results;
  }

  size_t size() const noexcept override {
    std::lock_guard lk(mu_);
    return store_.size();
  }
  std::string name() const noexcept override { return "ShortTermMemory"; }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, MemoryEntry> store_;
  size_t capacity_;
  uint64_t id_counter_{0};

  // HNSW 索引（unique_ptr 自动管理生命周期）
  std::unique_ptr<hnswlib::InnerProductSpace> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_index_;
  size_t dim_{0};
  hnswlib::labeltype label_counter_{0};
  size_t deleted_label_count_{0};
  std::unordered_map<std::string, hnswlib::labeltype> id_to_label_;
  std::unordered_map<hnswlib::labeltype, std::string> label_to_id_;

  // Rebuild HNSW index with only live entries (caller must hold mu_)
  void compact_hnsw_locked() {
    if (!hnsw_index_ || id_to_label_.empty()) return;
    try {
      size_t live_count = id_to_label_.size();
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

      space_ = std::move(new_space);
      hnsw_index_ = std::move(new_index);
      id_to_label_ = std::move(new_id_to_label);
      label_to_id_ = std::move(new_label_to_id);
      label_counter_ = new_label;
      deleted_label_count_ = 0;

      LOG_INFO(fmt::format("ShortTermMemory: HNSW compacted to {} live entries", live_count));
    } catch (const std::exception &e) {
      LOG_WARN(std::string("ShortTermMemory: HNSW compaction failed: ") + e.what());
    }
  }
};

// ─────────────────────────────────────────────────────────────
// § 4.5  LongTermMemory — 持久化磁盘记忆（慢但无限）
// ─────────────────────────────────────────────────────────────

class LongTermMemory : public IMemoryStore {
private:
  struct IndexRecord {
    std::string id;
    hnswlib::labeltype label;
    float importance;
    std::string user_id;
    std::string agent_id;
    std::string session_id;
    std::string type;
  };

public:
  explicit LongTermMemory(fs::path dir = "",
                          size_t max_elements = 100000)
      : dir_(dir.empty() ? fs::temp_directory_path() / "agentos_ltm" : std::move(dir)),
        max_elements_(max_elements) {
    fs::create_directories(dir_);
    load_index();
  }

  /// FIX #22: Check if loading succeeded
  [[nodiscard]] bool is_loaded() const noexcept { return load_ok_; }

  ~LongTermMemory() {
    // 析构时自动落盘脏索引
    std::lock_guard lk(mu_);
    if (index_dirty_) save_index_locked();
  }

  /// 显式刷新索引到磁盘（批量写入后调用）
  void flush() {
    std::lock_guard lk(mu_);
    if (index_dirty_) save_index_locked();
  }

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override {
    entry.created_at = entry.accessed_at = now();

    std::lock_guard lk(mu_);
    if (entry.id.empty())
      entry.id = "lt_" + std::to_string(id_counter_++);

    // 持久化 content 到 .mem 文件（锁内执行，避免并发写同一文件竞态）
    auto path = entry_path(entry.id);
    std::ofstream ofs(path);
    if (!ofs)
      return make_error(ErrorCode::MemoryWriteFailed, "LTM: cannot write file");
    ofs << entry.id << "\n" << entry.source << "\n" << entry.importance << "\n";
    ofs << entry.user_id << "\n"
        << entry.agent_id << "\n"
        << entry.session_id << "\n"
        << entry.type << "\n";

    for (char c : entry.content) {
      if (c == '\n')
        ofs << "\\n";
      else
        ofs << c;
    }
    ofs << "\n";
    ofs.flush();
    if (!ofs.good())
      return make_error(ErrorCode::MemoryWriteFailed, "LTM: disk write failed (disk full?)");

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
    index_dirty_ = true; // 延迟落盘，批量写入时避免 O(N²)
    return entry.id;
  }

  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    return read_locked(id);
  }

  [[nodiscard]] Result<bool> forget(const std::string &id) override {
    std::lock_guard lk(mu_);
    auto path = entry_path(id);
    if (!fs::exists(path))
      return false;
    fs::remove(path);

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

  // HNSW 过滤器：使用持久化反向映射 + IndexRecord 进行 scope 过滤
  class LTMFilter : public hnswlib::BaseFilterFunctor {
  public:
    LTMFilter(
        const MemoryFilter &filter,
        const std::unordered_map<hnswlib::labeltype, std::string> &label_to_id,
        const std::unordered_map<std::string, IndexRecord> &index)
        : flt_(filter), l2i_(label_to_id), idx_(index) {}

    bool operator()(hnswlib::labeltype label) override {
      auto id_it = l2i_.find(label);
      if (id_it == l2i_.end())
        return false;
      auto rec_it = idx_.find(id_it->second);
      if (rec_it == idx_.end())
        return false;
      auto &rec = rec_it->second;
      return flt_.match(rec.user_id, rec.agent_id, rec.session_id, rec.type);
    }

  private:
    const MemoryFilter &flt_;
    const std::unordered_map<hnswlib::labeltype, std::string> &l2i_;
    const std::unordered_map<std::string, IndexRecord> &idx_;
  };

  [[nodiscard]] Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
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

  std::vector<MemoryEntry> get_all() override {
    std::lock_guard lk(mu_);
    std::vector<MemoryEntry> results;
    results.reserve(index_.size());
    for (const auto &[id, rec] : index_) {
      if (auto entry = read_locked(id))
        results.push_back(std::move(*entry));
    }
    return results;
  }

  size_t size() const noexcept override {
    std::lock_guard lk(mu_);
    return index_.size();
  }
  std::string name() const noexcept override { return "LongTermMemory"; }

private:
  fs::path entry_path(const std::string &id) const {
    return dir_ / (id + ".mem");
  }

  /// 锁内读取（调用者必须已持有 mu_）
  Result<MemoryEntry> read_locked(const std::string &id) {
    auto path = entry_path(id);
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

  void load_index() {
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
      LOG_WARN(std::string("LTM: load_index() failed: ") + e.what());
      // Reset to safe state on failure
      hnsw_index_.reset();
      space_.reset();
      index_.clear();
      label_to_id_.clear();
      load_ok_ = false;
    }
  }

  void save_index_locked() {
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

  mutable std::mutex mu_;
  std::unordered_map<std::string, IndexRecord> index_;
  fs::path dir_;
  uint64_t id_counter_{0};
  size_t max_elements_;
  bool index_dirty_{false}; // 延迟索引落盘标记
  bool load_ok_{false}; // FIX #22: Track whether load_index() succeeded

  // HNSW 索引（unique_ptr 自动管理生命周期）
  std::unique_ptr<hnswlib::InnerProductSpace> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_index_;
  size_t dim_{0};
  hnswlib::labeltype label_counter_{0};
  std::unordered_map<hnswlib::labeltype, std::string> label_to_id_; // 持久化反向映射
};

// ─────────────────────────────────────────────────────────────
// § 4.6  MemorySystem — 统一门面（三级缓存 L0/L1/L2）
// ─────────────────────────────────────────────────────────────

class MemorySystem : private NonCopyable {
public:
  // LTM 后端类型
  enum class LTMBackend { FileBased, SQLite, DuckDB };

  // 定义在 memory.cpp 以避免循环 include（SQLiteLongTermMemory 在 sqlite_store.hpp）
  // LOW PRIORITY: Use fs::temp_directory_path() if ltm_dir is empty
  explicit MemorySystem(fs::path ltm_dir = "",
                        LTMBackend backend = LTMBackend::FileBased);

  // 高级构造：注入自定义 LTM 后端（如 SQLiteLongTermMemory）
  MemorySystem(fs::path ltm_dir, std::unique_ptr<IMemoryStore> custom_ltm)
      : working_(std::make_unique<WorkingMemory>(32)),
        short_term_(std::make_unique<ShortTermMemory>(512)),
        long_term_(std::move(custom_ltm)),
        graph_(std::make_unique<LocalGraphMemory>(std::move(ltm_dir))) {}

  // 运行时切换 LTM 后端
  void set_long_term_store(std::unique_ptr<IMemoryStore> store) {
    long_term_ = std::move(store);
  }

  // 便捷方法：添加情景记忆（绑定 Session 和 User）
  Result<std::string> add_episodic(std::string content, const Embedding &emb,
                                   const std::string &user_id,
                                   const std::string &session_id,
                                   float importance = 0.5f) {
    MemoryFilter f;
    f.user_id = user_id;
    f.session_id = session_id;
    f.type = "episodic";
    return remember(std::move(content), emb, "agent", importance, f);
  }

  // 便捷方法：添加语义记忆（绑定 User，无 Session 强关联）
  Result<std::string> add_semantic(std::string content, const Embedding &emb,
                                   const std::string &user_id,
                                   float importance = 0.8f) {
    MemoryFilter f;
    f.user_id = user_id;
    f.type = "semantic";
    return remember(std::move(content), emb, "agent", importance, f);
  }

  // ==== Graph Memory High-Level APIs ====

  Result<bool> add_triplet(const std::string &subject,
                           const std::string &predicate,
                           const std::string &object, float weight = 1.0f) {
    GraphEdge edge;
    edge.source_id = subject;
    edge.target_id = object;
    edge.relation = predicate;
    edge.weight = weight;
    return graph_->add_edge(edge);
  }

  Result<Subgraph> query_graph(const std::string &start_entity, int k_hop = 2) {
    return graph_->k_hop_search(start_entity, k_hop);
  }

  // ======================================

  // 写入：优先写入工作记忆；重要性高的自动同步到长期
  [[nodiscard]] Result<std::string> remember(std::string content, const Embedding &emb,
                               std::string source = "agent",
                               float importance = 0.5f,
                               MemoryFilter filter = {}) {
    MemoryEntry entry;
    entry.content = std::move(content);
    entry.embedding = emb;
    entry.source = std::move(source);
    entry.importance = importance;
    entry.user_id = filter.user_id.value_or("");
    entry.agent_id = filter.agent_id.value_or("");
    entry.session_id = filter.session_id.value_or("");
    entry.type = filter.type.value_or("episodic");

    // FIX #10: Check return values for all write() calls
    // L0 工作记忆（总写）
    auto wm_id = working_->write(entry);
    if (!wm_id) {
      LOG_WARN("MemorySystem: WorkingMemory write failed");
      return wm_id;
    }

    // L1 短期记忆（总写）
    auto st_result = short_term_->write(entry);
    if (!st_result) {
      LOG_WARN("MemorySystem: ShortTermMemory write failed");
      // Continue despite L1 failure; L0 succeeded
    }

    // L2 长期记忆（重要性 > 0.7 时持久化）
    if (importance > 0.7f) {
      auto lt_result = long_term_->write(entry);
      if (!lt_result) {
        LOG_WARN("MemorySystem: LongTermMemory write failed (non-critical, entry in L0)");
        // Continue; L0/L1 already succeeded
      }
    }

    return wm_id;
  }

  // 检索：L0 → L1 → L2 逐层查找（O(n) 去重）
  [[nodiscard]] Result<std::vector<SearchResult>> recall(const Embedding &q_emb,
                                           const MemoryFilter &filter = {},
                                           size_t top_k = 5) {
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

    // L0 工作记忆
    auto r0 = working_->search(q_emb, filter, top_k);
    merge(r0);

    // L1 短期记忆
    auto r1 = short_term_->search(q_emb, filter, top_k);
    merge(r1);

    // L2 长期记忆
    auto r2 = long_term_->search(q_emb, filter, top_k);
    merge(r2);

    // 全局排序取 Top-K
    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });
    if (results.size() > top_k)
      results.resize(top_k);
    return results;
  }

  // 遗忘：从所有层删除
  void forget(const std::string &id) {
    (void)working_->forget(id);
    (void)short_term_->forget(id);
    (void)long_term_->forget(id);
  }

  // 将工作记忆中的重要内容晋升到长期记忆，返回晋升数量
  size_t consolidate(float importance_threshold = 0.6f) {
    auto r = working_->get_all();
    size_t promoted = 0;
    for (auto &entry : r) {
      if (entry.importance >= importance_threshold) {
        (void)long_term_->write(entry);
        ++promoted;
      }
    }
    // Flush LTM index once after batch write (avoids O(N²) index saves)
    if (promoted > 0) {
      if (auto *ltm = dynamic_cast<LongTermMemory *>(long_term_.get()))
        ltm->flush();
    }
    return promoted;
  }

  // FIX #30: consolidate() with timeout to prevent blocking indefinitely
  bool consolidate(std::chrono::milliseconds timeout, float importance_threshold = 0.6f) {
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

  WorkingMemory &working() { return *working_; }
  const WorkingMemory &working() const { return *working_; }
  ShortTermMemory &short_term() { return *short_term_; }
  const ShortTermMemory &short_term() const { return *short_term_; }
  IMemoryStore &long_term() { return *long_term_; }
  const IMemoryStore &long_term() const { return *long_term_; }
  IGraphMemory &graph() { return *graph_; }
  const IGraphMemory &graph() const { return *graph_; }

private:
  std::unique_ptr<WorkingMemory> working_;
  std::unique_ptr<ShortTermMemory> short_term_;
  std::unique_ptr<IMemoryStore> long_term_;
  std::unique_ptr<IGraphMemory> graph_;
};

} // namespace agentos::memory
