#pragma once
// ============================================================
// AgentOS :: SQLite-backed Long-Term Memory Store
// 替代文件系统 + index.dat 的持久化方案
// 单文件 WAL 模式，事务安全，支持万级记忆高效存取
// ============================================================

#include "agentos/core/types.hpp"
#include "agentos/core/logger.hpp"
#include "agentos/memory/memory.hpp"
#include <cstring>
#include <sqlite3.h>

namespace agentos::memory {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// RAII SQLite 句柄
// ─────────────────────────────────────────────────────────────
class SQLiteDB {
public:
  SQLiteDB() = default;

  bool open(const std::string &path) {
    close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return false;
    }

    // WAL 模式 + 性能调优
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA cache_size=-65536"); // 64MB cache
    exec("PRAGMA temp_store=MEMORY");
    opened_ = true;
    return true;
  }

  void close() {
    // finalize all cached statements
    for (auto &[sql, stmt] : stmt_cache_) {
      if (stmt)
        sqlite3_finalize(stmt);
    }
    stmt_cache_.clear();

    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    opened_ = false;
  }

  ~SQLiteDB() { close(); }

  // Non-copyable
  SQLiteDB(const SQLiteDB &) = delete;
  SQLiteDB &operator=(const SQLiteDB &) = delete;

  bool is_open() const { return opened_; }
  sqlite3 *raw() const { return db_; }

  bool exec(const std::string &sql) {
    char *err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (err) {
      sqlite3_free(err);
    }
    return rc == SQLITE_OK;
  }

  // 带语句缓存的 prepare（mutable: 语句缓存是逻辑 const 的优化）
  sqlite3_stmt *prepare(const std::string &sql) const {
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) {
      sqlite3_reset(it->second);
      sqlite3_clear_bindings(it->second);
      return it->second;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt)
      return nullptr;

    stmt_cache_[sql] = stmt;
    return stmt;
  }

private:
  sqlite3 *db_{nullptr};
  bool opened_{false};
  mutable std::unordered_map<std::string, sqlite3_stmt *> stmt_cache_;
};

// ─────────────────────────────────────────────────────────────
// SQLiteLongTermMemory — IMemoryStore 实现
// ─────────────────────────────────────────────────────────────

class SQLiteLongTermMemory : public IMemoryStore {
public:
  explicit SQLiteLongTermMemory(fs::path dir = "/tmp/agentos_sqlite_ltm",
                                size_t max_elements = 100000)
      : dir_(std::move(dir)), max_elements_(max_elements) {
    fs::create_directories(dir_);

    auto db_path = (dir_ / "memory.db").string();
    if (!db_.open(db_path)) {
      LOG_ERROR(fmt::format("[SQLiteLTM] Failed to open database: {}", db_path));
      return;
    }

    create_schema();
    load_hnsw_index();
    migrate_from_files();
  }

  ~SQLiteLongTermMemory() { save_hnsw_index(); }

  Result<std::string> write(MemoryEntry entry) override {
    std::lock_guard lk(mu_);

    if (!db_.is_open())
      return make_error(ErrorCode::MemoryWriteFailed, "SQLiteLTM: DB not open");

    if (entry.id.empty())
      entry.id = "lt_" + std::to_string(id_counter_++);
    entry.created_at = entry.accessed_at = now();

    // INSERT OR REPLACE into SQLite
    auto stmt = db_.prepare(
        "INSERT OR REPLACE INTO entries "
        "(id, content, source, user_id, agent_id, session_id, type, "
        " importance, access_count, created_at, accessed_at, embedding) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt)
      return make_error(ErrorCode::MemoryWriteFailed, "SQLiteLTM: prepare failed");

    sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, entry.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, entry.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, entry.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, entry.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, entry.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, entry.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, entry.importance);
    sqlite3_bind_int(stmt, 9, static_cast<int>(entry.access_count));

    auto created_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          entry.created_at.time_since_epoch())
                          .count();
    auto accessed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           entry.accessed_at.time_since_epoch())
                           .count();
    sqlite3_bind_int64(stmt, 10, created_us);
    sqlite3_bind_int64(stmt, 11, accessed_us);

    if (!entry.embedding.empty()) {
      sqlite3_bind_blob(stmt, 12, entry.embedding.data(),
                        static_cast<int>(entry.embedding.size() * sizeof(float)),
                        SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 12);
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
      return make_error(ErrorCode::MemoryWriteFailed,
                        "SQLiteLTM: insert failed: " +
                            std::string(sqlite3_errmsg(db_.raw())));

    // Add to HNSW index
    add_to_hnsw(entry);

    return entry.id;
  }

  Result<MemoryEntry> read(const std::string &id) override {
    std::lock_guard lk(mu_);
    return read_locked(id);
  }

  Result<bool> forget(const std::string &id) override {
    std::lock_guard lk(mu_);

    // Remove from HNSW
    auto label_it = id_to_label_.find(id);
    if (label_it != id_to_label_.end() && hnsw_index_) {
      hnsw_index_->markDelete(label_it->second);
      label_to_id_.erase(label_it->second);
      id_to_label_.erase(label_it);
    }

    // DELETE from SQLite
    auto stmt = db_.prepare("DELETE FROM entries WHERE id = ?");
    if (!stmt)
      return false;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db_.raw()) > 0;
  }

  Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
    std::lock_guard lk(mu_);
    std::vector<SearchResult> results;

    if (q_emb.empty() || !hnsw_index_ || hnsw_index_->cur_element_count == 0) {
      // Fallback: linear scan via SQLite
      return search_linear_locked(filter, top_k);
    }

    if (q_emb.size() != dim_)
      return make_error(ErrorCode::InvalidArgument, "Dim mismatch");

    // HNSW 过滤器
    SQLiteLTMFilter ltm_filter(filter, label_to_id_, id_meta_);

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

      auto entry_result = read_locked(l2i_it->second);
      if (!entry_result)
        continue;

      float sim = 1.0f - dist;
      float score = sim * (0.7f + 0.3f * entry_result->importance);
      results.push_back({std::move(*entry_result), score});
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

    auto stmt = db_.prepare(
        "SELECT id, content, source, user_id, agent_id, session_id, type, "
        "       importance, access_count, created_at, accessed_at, embedding "
        "FROM entries");
    if (!stmt)
      return results;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      auto entry = row_to_entry(stmt);
      results.push_back(std::move(entry));
    }
    return results;
  }

  size_t size() const override {
    std::lock_guard lk(mu_);
    if (!db_.is_open())
      return 0;
    auto stmt = db_.prepare("SELECT COUNT(*) FROM entries");
    if (!stmt)
      return 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      return static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    return 0;
  }

  std::string name() const override { return "SQLiteLongTermMemory"; }

private:
  // ── 元数据缓存（用于 HNSW filter，避免每次 filter 都读 SQLite）──
  struct MetaRecord {
    std::string user_id;
    std::string agent_id;
    std::string session_id;
    std::string type;
    float importance;
  };

  // HNSW 过滤器
  class SQLiteLTMFilter : public hnswlib::BaseFilterFunctor {
  public:
    SQLiteLTMFilter(
        const MemoryFilter &filter,
        const std::unordered_map<hnswlib::labeltype, std::string> &label_to_id,
        const std::unordered_map<std::string, MetaRecord> &id_meta)
        : flt_(filter), l2i_(label_to_id), meta_(id_meta) {}

    bool operator()(hnswlib::labeltype label) override {
      auto id_it = l2i_.find(label);
      if (id_it == l2i_.end())
        return false;
      auto meta_it = meta_.find(id_it->second);
      if (meta_it == meta_.end())
        return false;
      auto &m = meta_it->second;
      return flt_.match(m.user_id, m.agent_id, m.session_id, m.type);
    }

  private:
    const MemoryFilter &flt_;
    const std::unordered_map<hnswlib::labeltype, std::string> &l2i_;
    const std::unordered_map<std::string, MetaRecord> &meta_;
  };

  void create_schema() {
    db_.exec(
        "CREATE TABLE IF NOT EXISTS entries ("
        "  id TEXT PRIMARY KEY,"
        "  content TEXT NOT NULL,"
        "  source TEXT DEFAULT '',"
        "  user_id TEXT DEFAULT '',"
        "  agent_id TEXT DEFAULT '',"
        "  session_id TEXT DEFAULT '',"
        "  type TEXT DEFAULT 'episodic',"
        "  importance REAL DEFAULT 0.5,"
        "  access_count INTEGER DEFAULT 0,"
        "  created_at INTEGER DEFAULT 0,"
        "  accessed_at INTEGER DEFAULT 0,"
        "  embedding BLOB"
        ")");

    // 为 filter 常用字段建索引
    db_.exec("CREATE INDEX IF NOT EXISTS idx_user_id ON entries(user_id)");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_type ON entries(type)");
  }

  // ── 从 SELECT 结果行提取 MemoryEntry（列顺序必须匹配 SELECT_ALL_COLS）──
  static constexpr const char *SELECT_ALL_COLS =
      "id, content, source, user_id, agent_id, session_id, type, "
      "importance, access_count, created_at, accessed_at, embedding";

  static std::string col_text(sqlite3_stmt *stmt, int col) {
    auto p = sqlite3_column_text(stmt, col);
    return p ? std::string(reinterpret_cast<const char *>(p)) : "";
  }

  static MemoryEntry row_to_entry(sqlite3_stmt *stmt) {
    MemoryEntry entry;
    entry.id = col_text(stmt, 0);
    entry.content = col_text(stmt, 1);
    entry.source = col_text(stmt, 2);
    entry.user_id = col_text(stmt, 3);
    entry.agent_id = col_text(stmt, 4);
    entry.session_id = col_text(stmt, 5);
    entry.type = col_text(stmt, 6);

    entry.importance = static_cast<float>(sqlite3_column_double(stmt, 7));
    entry.access_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 8));

    auto created_us = sqlite3_column_int64(stmt, 9);
    auto accessed_us = sqlite3_column_int64(stmt, 10);
    entry.created_at = TimePoint(std::chrono::microseconds(created_us));
    entry.accessed_at = TimePoint(std::chrono::microseconds(accessed_us));

    // Embedding from BLOB
    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
      const float *blob_data =
          static_cast<const float *>(sqlite3_column_blob(stmt, 11));
      int blob_bytes = sqlite3_column_bytes(stmt, 11);
      size_t num_floats = blob_bytes / sizeof(float);
      entry.embedding.assign(blob_data, blob_data + num_floats);
    }

    return entry;
  }

  Result<MemoryEntry> read_locked(const std::string &id) {
    std::string sql = std::string("SELECT ") + SELECT_ALL_COLS +
                      " FROM entries WHERE id = ?";
    auto stmt = db_.prepare(sql);
    if (!stmt)
      return make_error(ErrorCode::MemoryReadFailed, "SQLiteLTM: prepare failed");

    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW)
      return make_error(ErrorCode::NotFound, "SQLiteLTM: entry not found");

    return row_to_entry(stmt);
  }

  Result<std::vector<SearchResult>>
  search_linear_locked(const MemoryFilter &filter, size_t top_k) {
    std::vector<SearchResult> results;

    // Build WHERE clause dynamically — single-pass SELECT all columns
    std::string sql = std::string("SELECT ") + SELECT_ALL_COLS +
                      " FROM entries WHERE 1=1";
    if (filter.user_id)
      sql += " AND user_id = ?";
    if (filter.agent_id)
      sql += " AND agent_id = ?";
    if (filter.session_id)
      sql += " AND session_id = ?";
    if (filter.type)
      sql += " AND type = ?";
    sql += " ORDER BY importance DESC LIMIT ?";

    auto stmt = db_.prepare(sql);
    if (!stmt)
      return results;

    int bind_idx = 1;
    if (filter.user_id)
      sqlite3_bind_text(stmt, bind_idx++, filter.user_id->c_str(), -1,
                        SQLITE_TRANSIENT);
    if (filter.agent_id)
      sqlite3_bind_text(stmt, bind_idx++, filter.agent_id->c_str(), -1,
                        SQLITE_TRANSIENT);
    if (filter.session_id)
      sqlite3_bind_text(stmt, bind_idx++, filter.session_id->c_str(), -1,
                        SQLITE_TRANSIENT);
    if (filter.type)
      sqlite3_bind_text(stmt, bind_idx++, filter.type->c_str(), -1,
                        SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, bind_idx, static_cast<int64_t>(top_k));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      auto entry = row_to_entry(stmt);
      results.push_back({std::move(entry), 0.0f});
    }
    return results;
  }

  void add_to_hnsw(const MemoryEntry &entry) {
    bool has_embedding = !entry.embedding.empty();

    if (has_embedding) {
      if (!hnsw_index_) {
        dim_ = entry.embedding.size();
        space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
        hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            space_.get(), max_elements_, 16, 200);
      } else if (entry.embedding.size() != dim_) {
        has_embedding = false;
      }
    }

    if (has_embedding) {
      // 容量不足时动态扩容
      if (hnsw_index_->cur_element_count >= hnsw_index_->max_elements_) {
        hnsw_index_->resizeIndex(hnsw_index_->max_elements_ * 2);
      }

      // 如果 ID 已存在，先标记删除旧的
      auto old_it = id_to_label_.find(entry.id);
      if (old_it != id_to_label_.end()) {
        hnsw_index_->markDelete(old_it->second);
        label_to_id_.erase(old_it->second);
      }

      hnswlib::labeltype label = label_counter_++;
      hnsw_index_->addPoint(entry.embedding.data(), label);
      id_to_label_[entry.id] = label;
      label_to_id_[label] = entry.id;
    }

    // 更新元数据缓存
    id_meta_[entry.id] = {entry.user_id, entry.agent_id, entry.session_id,
                          entry.type, entry.importance};
  }

  void save_hnsw_index() {
    if (hnsw_index_) {
      hnsw_index_->saveIndex((dir_ / "hnsw_index.bin").string());
    }
  }

  void load_hnsw_index() {
    // 从 SQLite 加载所有 embedding 重建 HNSW
    auto stmt = db_.prepare(
        "SELECT id, user_id, agent_id, session_id, type, importance, embedding "
        "FROM entries WHERE embedding IS NOT NULL");
    if (!stmt)
      return;

    std::vector<std::pair<std::string, Embedding>> entries_with_emb;
    std::vector<MetaRecord> metas;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      auto col0 = sqlite3_column_text(stmt, 0);
      if (!col0)
        continue; // id 为 NULL 时跳过
      std::string id(reinterpret_cast<const char *>(col0));

      auto col1 = sqlite3_column_text(stmt, 1);
      auto col2 = sqlite3_column_text(stmt, 2);
      auto col3 = sqlite3_column_text(stmt, 3);
      auto col4 = sqlite3_column_text(stmt, 4);

      MetaRecord meta;
      meta.user_id = col1 ? std::string(reinterpret_cast<const char *>(col1)) : "";
      meta.agent_id =
          col2 ? std::string(reinterpret_cast<const char *>(col2)) : "";
      meta.session_id =
          col3 ? std::string(reinterpret_cast<const char *>(col3)) : "";
      meta.type = col4 ? std::string(reinterpret_cast<const char *>(col4)) : "";
      meta.importance = static_cast<float>(sqlite3_column_double(stmt, 5));

      const float *blob =
          static_cast<const float *>(sqlite3_column_blob(stmt, 6));
      int blob_bytes = sqlite3_column_bytes(stmt, 6);
      size_t num_floats = blob_bytes / sizeof(float);

      Embedding emb(blob, blob + num_floats);
      entries_with_emb.push_back({id, std::move(emb)});
      metas.push_back(meta);
      id_meta_[id] = metas.back();

      // 恢复 id_counter_
      if (id.starts_with("lt_")) {
        try {
          uint64_t seq = std::stoull(id.substr(3));
          id_counter_ = std::max(id_counter_, seq + 1);
        } catch (const std::exception &) {
        }
      }
    }

    if (entries_with_emb.empty())
      return;

    // 从 SQLite embeddings 重建 HNSW（确保 label 映射一致）
    dim_ = entries_with_emb[0].second.size();
    space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);

    size_t capacity =
        std::max(max_elements_, entries_with_emb.size() * 2);
    hnsw_index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
        space_.get(), capacity, 16, 200);

    for (size_t i = 0; i < entries_with_emb.size(); ++i) {
      auto &[id, emb] = entries_with_emb[i];
      if (emb.size() != dim_)
        continue;

      hnswlib::labeltype label = label_counter_++;
      hnsw_index_->addPoint(emb.data(), label);
      id_to_label_[id] = label;
      label_to_id_[label] = id;
    }
  }

  // ── 旧格式迁移：从 .mem 文件 + index.dat 迁移到 SQLite ──
  void migrate_from_files() {
    auto idx_path = dir_ / "index.dat";
    if (!fs::exists(idx_path))
      return;

    LOG_INFO("[SQLiteLTM] Migrating from file-based storage...");

    std::ifstream ifs(idx_path);
    std::string line;

    // 跳过 DIM 行
    std::getline(ifs, line);

    int migrated = 0;
    while (std::getline(ifs, line)) {
      if (line.empty())
        continue;

      std::istringstream ls(line);
      std::string id;
      hnswlib::labeltype old_label;
      float importance;
      std::string user_id, agent_id, session_id, type;
      ls >> id >> old_label >> importance >> user_id >> agent_id >> session_id >>
          type;

      if (user_id == "-")
        user_id = "";
      if (agent_id == "-")
        agent_id = "";
      if (session_id == "-")
        session_id = "";
      if (type == "-")
        type = "";

      // 读取 .mem 文件内容
      auto mem_path = dir_ / (id + ".mem");
      if (!fs::exists(mem_path))
        continue;

      std::ifstream mem_ifs(mem_path);
      MemoryEntry entry;
      std::string tmp;
      std::getline(mem_ifs, entry.id);
      std::getline(mem_ifs, entry.source);
      std::string imp_str;
      std::getline(mem_ifs, imp_str);
      try {
        entry.importance = std::stof(imp_str);
      } catch (const std::exception &) {
        entry.importance = importance;
      }
      std::getline(mem_ifs, entry.user_id);
      std::getline(mem_ifs, entry.agent_id);
      std::getline(mem_ifs, entry.session_id);
      std::getline(mem_ifs, entry.type);
      std::getline(mem_ifs, tmp);

      // 解码转义的换行
      for (size_t i = 0; i < tmp.size(); ++i) {
        if (tmp[i] == '\\' && i + 1 < tmp.size() && tmp[i + 1] == 'n') {
          entry.content += '\n';
          i++;
        } else {
          entry.content += tmp[i];
        }
      }

      entry.created_at = entry.accessed_at = now();

      // 写入 SQLite（不走 HNSW，embedding 已丢失）
      auto stmt = db_.prepare(
          "INSERT OR IGNORE INTO entries "
          "(id, content, source, user_id, agent_id, session_id, type, "
          " importance, access_count, created_at, accessed_at) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)");
      if (!stmt)
        continue;

      sqlite3_bind_text(stmt, 1, entry.id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, entry.content.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, entry.source.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, entry.user_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, entry.agent_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6, entry.session_id.c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7, entry.type.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(stmt, 8, entry.importance);

      auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
                    entry.created_at.time_since_epoch())
                    .count();
      sqlite3_bind_int64(stmt, 9, ts);
      sqlite3_bind_int64(stmt, 10, ts);

      if (sqlite3_step(stmt) == SQLITE_DONE) {
        migrated++;
        // 删除旧文件
        fs::remove(mem_path);
      }

      // 更新元数据缓存
      id_meta_[entry.id] = {entry.user_id, entry.agent_id, entry.session_id,
                            entry.type, entry.importance};
    }

    if (migrated > 0) {
      LOG_INFO(fmt::format("[SQLiteLTM] Migrated {} entries from file storage", migrated));
      // 删除旧索引文件
      fs::remove(idx_path);
      auto old_hnsw = dir_ / "hnsw_index.bin";
      if (fs::exists(old_hnsw))
        fs::remove(old_hnsw);
    }
  }

  mutable std::mutex mu_;
  fs::path dir_;
  SQLiteDB db_;
  size_t max_elements_;
  uint64_t id_counter_{0};

  // HNSW 索引
  std::unique_ptr<hnswlib::InnerProductSpace> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_index_;
  size_t dim_{0};
  hnswlib::labeltype label_counter_{0};
  std::unordered_map<std::string, hnswlib::labeltype> id_to_label_;
  std::unordered_map<hnswlib::labeltype, std::string> label_to_id_;

  // 元数据缓存（避免 HNSW filter 时反复读 SQLite）
  std::unordered_map<std::string, MetaRecord> id_meta_;
};

} // namespace agentos::memory
