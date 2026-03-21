#pragma once
// ============================================================
// AgentOS :: Module 4 — Memory System
// 工作记忆 / 短期记忆 / 长期记忆，统一 IMemoryStore 接口
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <algorithm>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
  [[nodiscard]] static float cosine_similarity(const Embedding &a, const Embedding &b) noexcept {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0f;

    const float* data_a = a.data();
    const float* data_b = b.data();
    size_t n = a.size();
    float dot = 0.0f;

    // Use compiler hint for SIMD vectorization
#if defined(__clang__) || defined(__GNUC__)
#pragma omp simd reduction(+:dot)
    for (size_t i = 0; i < n; ++i) {
        dot += data_a[i] * data_b[i];
    }
#else
    dot = std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
#endif
    return dot;
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

// ── Memory Store Concept ───────────────────────
template <typename T>
concept MemoryStoreConcept = requires(T s, MemoryEntry entry, const std::string &id,
                                      const Embedding &q_emb, const MemoryFilter &filter) {
  { s.write(std::move(entry)) } -> std::same_as<Result<std::string>>;
  { s.read(id) } -> std::same_as<Result<MemoryEntry>>;
  { s.forget(id) } -> std::same_as<Result<bool>>;
  { s.search(q_emb, filter) } -> std::same_as<Result<std::vector<SearchResult>>>;
  { s.name() } -> std::same_as<std::string>;
} && std::derived_from<T, IMemoryStore>;

// ─────────────────────────────────────────────────────────────
// § 4.3  WorkingMemory — 当前窗口内的即时记忆（最快）
// ─────────────────────────────────────────────────────────────

class WorkingMemory : public IMemoryStore {
public:
  explicit WorkingMemory(size_t capacity = 32) : capacity_(capacity) {}

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override;
  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override;
  [[nodiscard]] Result<bool> forget(const std::string &id) override;
  [[nodiscard]] Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override;

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

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override;
  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override;
  [[nodiscard]] Result<bool> forget(const std::string &id) override;

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

  [[nodiscard]] Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override;

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

  /// Persist HNSW index + metadata to disk (atomic write via temp+rename)
  [[nodiscard]] Result<void> save(const std::string& dir);

  /// Load HNSW index + metadata from disk (missing files = fresh start)
  [[nodiscard]] Result<void> load(const std::string& dir);

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
  bool compacting_{false}; // FIX #11: guards against reentrant compaction
  bool dirty_{false};      // Track whether save() is needed

  // Rebuild HNSW index with only live entries (caller must hold mu_)
  void compact_hnsw_locked();
};

// ─────────────────────────────────────────────────────────────
// § 4.4.1  LRUCache (Generic Metadata Cache)
// ─────────────────────────────────────────────────────────────
template <typename K, typename V>
class LRUCache {
public:
  explicit LRUCache(size_t capacity = 1024) : capacity_(capacity) {}

  std::optional<V> get(const K &key) {
    std::lock_guard lk(mu_);
    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) return std::nullopt;
    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
    return it->second->second;
  }

  void put(const K &key, V value) {
    std::lock_guard lk(mu_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
      cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
      it->second->second = std::move(value);
      return;
    }
    if (cache_list_.size() >= capacity_) {
      auto last = cache_list_.back();
      cache_map_.erase(last.first);
      cache_list_.pop_back();
    }
    cache_list_.emplace_front(key, std::move(value));
    cache_map_[key] = cache_list_.begin();
  }

  void remove(const K &key) {
    std::lock_guard lk(mu_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
      cache_list_.erase(it->second);
      cache_map_.erase(it);
    }
  }

  void clear() {
    std::lock_guard lk(mu_);
    cache_list_.clear();
    cache_map_.clear();
  }

private:
  size_t capacity_;
  std::mutex mu_;
  std::list<std::pair<K, V>> cache_list_;
  std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> cache_map_;
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
    // FIX #13: use std::call_once to ensure load_index() is called exactly once
    // and is thread-safe even if the object is accessed from multiple threads
    // immediately after construction.
    std::call_once(load_once_, [this] { load_index(); });
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

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override;
  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override;
  [[nodiscard]] Result<bool> forget(const std::string &id) override;

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
                                           size_t top_k = 5) override;

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
  static std::filesystem::path entry_path(
        const std::filesystem::path &dir, const std::string &id);

  fs::path entry_path(const std::string &id) const {
    return entry_path(dir_, id);
  }

  /// 锁内读取（调用者必须已持有 mu_）
  Result<MemoryEntry> read_locked(const std::string &id);

  // On load failure: rename corrupt file and start fresh
  void handle_corrupt_index(const std::filesystem::path &index_path);

  void load_index();
  void save_index_locked();

  mutable std::mutex mu_;
  mutable std::once_flag load_once_; // FIX #13: Guard for one-time load_index() call
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
  LRUCache<std::string, MemoryEntry> metadata_cache_;
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

  ~MemorySystem() { save_indexes(); }

  // 高级构造：注入自定义 LTM 后端（如 SQLiteLongTermMemory）
  MemorySystem(fs::path ltm_dir, std::unique_ptr<IMemoryStore> custom_ltm)
      : working_(std::make_unique<WorkingMemory>(32)),
        short_term_(std::make_unique<ShortTermMemory>(512)),
        long_term_(std::move(custom_ltm)),
        graph_(std::make_unique<LocalGraphMemory>(ltm_dir)),
        ltm_dir_(std::move(ltm_dir)) {
    load_indexes();
  }

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
                               MemoryFilter filter = {});

  // 检索：L0 → L1 → L2 逐层查找（O(n) 去重）
  [[nodiscard]] Result<std::vector<SearchResult>> recall(const Embedding &q_emb,
                                           const MemoryFilter &filter = {},
                                           size_t top_k = 5);

  // 遗忘：从所有层删除
  void forget(const std::string &id) {
    (void)working_->forget(id);
    (void)short_term_->forget(id);
    (void)long_term_->forget(id);
  }

  // 将工作记忆中的重要内容晋升到长期记忆，返回晋升数量
  size_t consolidate(float importance_threshold = 0.6f);

  // Smart consolidation: L0->L1 for moderate scores, L0->L2 for high scores
  // Uses promotion_score() which factors in importance, frequency, and recency
  size_t smart_consolidate(float l1_threshold = 0.4f, float l2_threshold = 0.7f);

  // FIX #30: consolidate() with timeout to prevent blocking indefinitely
  bool consolidate(std::chrono::milliseconds timeout, float importance_threshold = 0.6f);

  WorkingMemory &working() { return *working_; }
  const WorkingMemory &working() const { return *working_; }
  ShortTermMemory &short_term() { return *short_term_; }
  const ShortTermMemory &short_term() const { return *short_term_; }
  IMemoryStore &long_term() { return *long_term_; }
  const IMemoryStore &long_term() const { return *long_term_; }
  IGraphMemory &graph() { return *graph_; }
  const IGraphMemory &graph() const { return *graph_; }

  /// Persist STM HNSW index to ltm_dir_
  void save_indexes() {
    if (ltm_dir_.empty()) return;
    try {
      (void)short_term_->save(ltm_dir_.string());
    } catch (const std::exception& e) {
      LOG_WARN(fmt::format("MemorySystem: save_indexes failed: {}", e.what()));
    }
  }

  /// Load STM HNSW index from ltm_dir_
  void load_indexes() {
    if (ltm_dir_.empty()) return;
    (void)short_term_->load(ltm_dir_.string());
  }

private:
  std::unique_ptr<WorkingMemory> working_;
  std::unique_ptr<ShortTermMemory> short_term_;
  std::unique_ptr<IMemoryStore> long_term_;
  std::unique_ptr<IGraphMemory> graph_;
  fs::path ltm_dir_;
};

} // namespace agentos::memory
