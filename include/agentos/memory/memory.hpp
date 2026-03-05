#pragma once
// ============================================================
// AgentOS :: Module 4 — Memory System
// 工作记忆 / 短期记忆 / 长期记忆，统一 IMemoryStore 接口
// ============================================================
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
  static float cosine_similarity(const Embedding &a, const Embedding &b) {
    if (a.size() != b.size())
      return 0.0f;
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

  virtual Result<std::string> write(MemoryEntry entry) = 0;
  virtual Result<MemoryEntry> read(const std::string &id) = 0;
  virtual Result<bool> forget(const std::string &id) = 0;

  // 语义检索 Top-K
  virtual Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                                   const MemoryFilter &filter,
                                                   size_t top_k = 5) = 0;

  // 获取所有记忆
  virtual std::vector<MemoryEntry> get_all() = 0;

  virtual size_t size() const = 0;
  virtual std::string name() const = 0;
};

// ─────────────────────────────────────────────────────────────
// § 4.3  WorkingMemory — 当前窗口内的即时记忆（最快）
// ─────────────────────────────────────────────────────────────

class WorkingMemory : public IMemoryStore {
public:
  explicit WorkingMemory(size_t capacity = 32) : capacity_(capacity) {}

  Result<std::string> write(MemoryEntry entry) override {
    std::lock_guard lk(mu_);
    if (entry.id.empty())
      entry.id = "wm_" + std::to_string(id_counter_++);
    entry.created_at = entry.accessed_at = now();

    // LRU 驱逐
    if (store_.size() >= capacity_) {
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

  Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    auto it = store_.find(id);
    if (it == store_.end())
      return make_error(ErrorCode::NotFound, "WorkingMemory: id not found");
    it->second.accessed_at = now();
    it->second.access_count++;
    return it->second;
  }

  Result<bool> forget(const std::string &id) override {
    std::lock_guard lk(mu_);
    return store_.erase(id) > 0;
  }

  Result<std::vector<SearchResult>> search(const Embedding &q_emb,
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
    for (auto &[id, entry] : store_)
      results.push_back(entry);
    return results;
  }

  size_t size() const override {
    std::lock_guard lk(mu_);
    return store_.size();
  }
  std::string name() const override { return "WorkingMemory"; }
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

  Result<std::string> write(MemoryEntry entry) override {
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

      if (hnsw_index_ && entry.embedding.size() == dim_) {
        hnswlib::labeltype label = label_counter_++;
        hnsw_index_->addPoint(entry.embedding.data(), label);
        id_to_label_[id] = label;
        label_to_id_[label] = id;
      }
    }

    store_[id] = std::move(entry);
    return id;
  }

  Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    auto it = store_.find(id);
    if (it == store_.end())
      return make_error(ErrorCode::NotFound, "ShortTermMemory: id not found");
    it->second.accessed_at = now();
    it->second.access_count++;
    return it->second;
  }

  Result<bool> forget(const std::string &id) override {
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
      }
    }

    return store_.erase(id) > 0;
  }

  // 自定义 HNSW 过滤器：scope 匹配 + 已删除检查
  class CustomFilter : public hnswlib::BaseFilterFunctor {
  public:
    CustomFilter(
        const MemoryFilter &filter,
        const std::unordered_map<hnswlib::labeltype, std::string> &label_to_id,
        const std::unordered_map<std::string, MemoryEntry> &store)
        : flt(filter), l2i(label_to_id), st(store) {}

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
    const std::unordered_map<hnswlib::labeltype, std::string> &l2i;
    const std::unordered_map<std::string, MemoryEntry> &st;
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

    CustomFilter custom_filter(filter, label_to_id_, store_);

    // InnerProductSpace: distance = 1.0 - dot_product
    size_t cur_count =
        static_cast<size_t>(hnsw_index_->cur_element_count.load());
    size_t search_k = std::min(top_k * 4, cur_count);
    if (search_k == 0)
      return results;

    std::priority_queue<std::pair<float, hnswlib::labeltype>> res;
    try {
      res = hnsw_index_->searchKnn(q_emb.data(), search_k, &custom_filter);
    } catch (...) {
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
    for (auto &[id, entry] : store_)
      results.push_back(entry);
    return results;
  }

  size_t size() const override {
    std::lock_guard lk(mu_);
    return store_.size();
  }
  std::string name() const override { return "ShortTermMemory"; }

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
  std::unordered_map<std::string, hnswlib::labeltype> id_to_label_;
  std::unordered_map<hnswlib::labeltype, std::string> label_to_id_;
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
  explicit LongTermMemory(fs::path dir = "/tmp/agentos_ltm",
                          size_t max_elements = 100000)
      : dir_(std::move(dir)), max_elements_(max_elements) {
    fs::create_directories(dir_);
    load_index();
  }

  Result<std::string> write(MemoryEntry entry) override {
    if (entry.id.empty())
      entry.id = "lt_" + std::to_string(id_counter_++);
    entry.created_at = entry.accessed_at = now();

    // 持久化 content 到 .mem 文件
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

    std::lock_guard lk(mu_);

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
      // 容量不足时动态扩容
      if (hnsw_index_->cur_element_count >= hnsw_index_->max_elements_) {
        hnsw_index_->resizeIndex(hnsw_index_->max_elements_ * 2);
      }
      node_label = label_counter_++;
      hnsw_index_->addPoint(entry.embedding.data(), node_label);
      label_to_id_[node_label] = entry.id;
    }

    index_[entry.id] = {entry.id,      node_label,     entry.importance,
                        entry.user_id, entry.agent_id, entry.session_id,
                        entry.type};
    save_index_locked();
    return entry.id;
  }

  Result<MemoryEntry> read(const std::string &id) override {
    auto path = entry_path(id);
    if (!fs::exists(path))
      return make_error(ErrorCode::NotFound, "LTM: entry not found");

    std::ifstream ifs(path);
    MemoryEntry entry;
    std::string line;
    std::getline(ifs, entry.id);
    std::getline(ifs, entry.source);
    std::string imp_str;
    std::getline(ifs, imp_str);
    entry.importance = std::stof(imp_str);
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

    std::lock_guard lk(mu_);
    auto it = index_.find(id);
    if (it != index_.end() && it->second.label != hnswlib::labeltype(-1) &&
        hnsw_index_) {
      try {
        entry.embedding = hnsw_index_->getDataByLabel<float>(it->second.label);
      } catch (...) {
      }
    }

    return entry;
  }

  Result<bool> forget(const std::string &id) override {
    auto path = entry_path(id);
    if (!fs::exists(path))
      return false;
    fs::remove(path);

    std::lock_guard lk(mu_);
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

  Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
    std::unique_lock lk(mu_);
    std::vector<SearchResult> results;

    if (q_emb.empty() || !hnsw_index_ || hnsw_index_->cur_element_count == 0) {
      // 退化线性过滤
      std::vector<std::string> matched_ids;
      for (auto &[id, rec] : index_) {
        if (filter.match(rec.user_id, rec.agent_id, rec.session_id, rec.type)) {
          matched_ids.push_back(id);
        }
      }
      lk.unlock(); // 释放锁以避免在 read() 中死锁

      for (const auto &id : matched_ids) {
        if (auto entry = read(id))
          results.push_back({*entry, 0.0f});
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
    } catch (...) {
      return results;
    }

    std::vector<std::pair<std::string, float>> matched_items;
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
      matched_items.push_back({l2i_it->second, score});
    }

    lk.unlock(); // 释放锁以避免在 read() 中死锁

    for (auto &[id, score] : matched_items) {
      if (auto entry = read(id)) {
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
    std::vector<MemoryEntry> results;
    std::vector<std::string> ids;
    {
      std::lock_guard lk(mu_);
      for (auto &[id, rec] : index_)
        ids.push_back(id);
    }
    for (auto &id : ids) {
      auto entry = read(id);
      if (entry)
        results.push_back(std::move(*entry));
    }
    return results;
  }

  size_t size() const override {
    std::lock_guard lk(mu_);
    return index_.size();
  }
  std::string name() const override { return "LongTermMemory"; }

private:
  fs::path entry_path(const std::string &id) const {
    return dir_ / (id + ".mem");
  }

  void load_index() {
    auto idx_path = dir_ / "index.dat";
    if (!fs::exists(idx_path))
      return;

    std::ifstream ifs(idx_path);
    std::string line;

    if (std::getline(ifs, line) && line.starts_with("DIM ")) {
      dim_ = std::stoull(line.substr(4));
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
        } catch (...) {
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
        } catch (...) {
        }
      }

      index_[rec.id] = std::move(rec);
    }
  }

  void save_index_locked() {
    auto idx_path = dir_ / "index.dat";
    std::ofstream ofs(idx_path);
    ofs << "DIM " << dim_ << "\n";
    for (auto &[id, rec] : index_) {
      ofs << rec.id << " " << rec.label << " " << rec.importance << " "
          << (rec.user_id.empty() ? "-" : rec.user_id) << " "
          << (rec.agent_id.empty() ? "-" : rec.agent_id) << " "
          << (rec.session_id.empty() ? "-" : rec.session_id) << " "
          << (rec.type.empty() ? "-" : rec.type) << "\n";
    }

    if (hnsw_index_) {
      hnsw_index_->saveIndex((dir_ / "hnsw_index.bin").string());
    }
  }

  mutable std::mutex mu_;
  std::unordered_map<std::string, IndexRecord> index_;
  fs::path dir_;
  uint64_t id_counter_{0};
  size_t max_elements_;

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
  enum class LTMBackend { FileBased, SQLite };

  explicit MemorySystem(fs::path ltm_dir = "/tmp/agentos_ltm",
                        LTMBackend backend = LTMBackend::FileBased)
      : working_(std::make_unique<WorkingMemory>(32)),
        short_term_(std::make_unique<ShortTermMemory>(512)),
        graph_(std::make_unique<LocalGraphMemory>(ltm_dir)) {
    // 根据后端类型创建 LTM（都实现 IMemoryStore 接口，可热切换）
    if (backend == LTMBackend::FileBased) {
      long_term_ = std::make_unique<LongTermMemory>(std::move(ltm_dir));
    } else {
      // SQLite 后端通过外部注入（见 set_long_term_store）
      long_term_ = std::make_unique<LongTermMemory>(std::move(ltm_dir));
    }
  }

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
  Result<std::string> remember(std::string content, const Embedding &emb,
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

    // L0 工作记忆（总写）
    auto wm_id = working_->write(entry);

    // L1 短期记忆（总写）
    short_term_->write(entry);

    // L2 长期记忆（重要性 > 0.7 时持久化）
    if (importance > 0.7f) {
      long_term_->write(entry);
    }

    return wm_id;
  }

  // 检索：L0 → L1 → L2 逐层查找
  Result<std::vector<SearchResult>> recall(const Embedding &q_emb,
                                           const MemoryFilter &filter = {},
                                           size_t top_k = 5) {
    std::vector<SearchResult> results;

    // L0 工作记忆
    if (auto r = working_->search(q_emb, filter, top_k)) {
      for (auto &sr : *r)
        results.push_back(sr);
    }

    // L1 短期记忆（去重）
    if (auto r = short_term_->search(q_emb, filter, top_k)) {
      for (auto &sr : *r) {
        bool dup = false;
        for (auto &existing : results) {
          if (existing.entry.id == sr.entry.id) {
            dup = true;
            break;
          }
        }
        if (!dup)
          results.push_back(sr);
      }
    }

    // L2 长期记忆（去重）
    if (auto r = long_term_->search(q_emb, filter, top_k)) {
      for (auto &sr : *r) {
        bool dup = false;
        for (auto &existing : results) {
          if (existing.entry.id == sr.entry.id) {
            dup = true;
            break;
          }
        }
        if (!dup)
          results.push_back(sr);
      }
    }

    // 全局排序取 Top-K
    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });
    if (results.size() > top_k)
      results.resize(top_k);
    return results;
  }

  // 遗忘：从所有层删除
  void forget(const std::string &id) {
    working_->forget(id);
    short_term_->forget(id);
    long_term_->forget(id);
  }

  // 将工作记忆中的重要内容晋升到长期记忆
  void consolidate(float importance_threshold = 0.6f) {
    // 检索所有工作记忆条目
    auto r = working_->get_all();
    for (auto &entry : r) {
      if (entry.importance >= importance_threshold) {
        long_term_->write(entry);
      }
    }
  }

  WorkingMemory &working() { return *working_; }
  ShortTermMemory &short_term() { return *short_term_; }
  IMemoryStore &long_term() { return *long_term_; }
  IGraphMemory &graph() { return *graph_; }

private:
  std::unique_ptr<WorkingMemory> working_;
  std::unique_ptr<ShortTermMemory> short_term_;
  std::unique_ptr<IMemoryStore> long_term_;
  std::unique_ptr<IGraphMemory> graph_;
};

} // namespace agentos::memory
