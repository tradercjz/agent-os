#pragma once
// ============================================================
// AgentOS :: DuckDB-backed Long-Term Memory Store
// 替代 SQLite 的高性能列存持久化方案
// 支持向量存储 + 结构化 SQL 查询 + 分析聚合
// ============================================================

#include "agentos/core/logger.hpp"
#include "agentos/core/types.hpp"
#include "agentos/memory/memory.hpp"
#include <cstring>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <duckdb.hpp>
#pragma GCC diagnostic pop

namespace agentos::memory {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// RAII DuckDB 连接封装
// ─────────────────────────────────────────────────────────────
class DuckDBConn {
public:
  DuckDBConn() = default;

  bool open(const std::string &path) {
    close();
    try {
      duckdb::DBConfig config;
      config.SetOptionByName("threads", duckdb::Value::INTEGER(2));
      db_ = std::make_unique<duckdb::DuckDB>(path, &config);
      con_ = std::make_unique<duckdb::Connection>(*db_);
      opened_ = true;
      return true;
    } catch (const std::exception &e) {
      LOG_ERROR(std::string("[DuckDB] Failed to open: ") + e.what());
      db_.reset();
      con_.reset();
      return false;
    }
  }

  void close() {
    con_.reset();
    db_.reset();
    opened_ = false;
  }

  ~DuckDBConn() { close(); }

  // Non-copyable
  DuckDBConn(const DuckDBConn &) = delete;
  DuckDBConn &operator=(const DuckDBConn &) = delete;

  bool is_open() const { return opened_; }

  bool exec(const std::string &sql) {
    if (!con_)
      return false;
    try {
      auto result = con_->Query(sql);
      return !result->HasError();
    } catch (const std::exception &) {
      return false;
    }
  }

  duckdb::Connection *connection() const { return con_.get(); }

private:
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> con_;
  bool opened_{false};
};

// ─────────────────────────────────────────────────────────────
// DuckDBLongTermMemory — IMemoryStore 实现
// ─────────────────────────────────────────────────────────────

class DuckDBLongTermMemory : public IMemoryStore {
public:
  explicit DuckDBLongTermMemory(fs::path dir = "/tmp/agentos_duckdb_ltm",
                                size_t max_elements = 100000)
      : dir_(std::move(dir)), max_elements_(max_elements) {
    fs::create_directories(dir_);

    auto db_path = (dir_ / "memory.duckdb").string();
    if (!db_.open(db_path)) {
      LOG_ERROR(std::string("[DuckDBLTM] Failed to open database: ") + db_path);
      return;
    }

    create_schema();
    load_hnsw_index();
  }

  ~DuckDBLongTermMemory() override { save_hnsw_index(); }

  Result<std::string> write(MemoryEntry entry) override {
    std::lock_guard lk(mu_);

    if (!db_.is_open())
      return make_error(ErrorCode::MemoryWriteFailed, "DuckDBLTM: DB not open");

    if (entry.id.empty())
      entry.id = "lt_" + std::to_string(id_counter_++);
    entry.created_at = entry.accessed_at = now();

    auto *con = db_.connection();
    if (!con)
      return make_error(ErrorCode::MemoryWriteFailed,
                        "DuckDBLTM: no connection");

    try {
      // DELETE existing entry if any (DuckDB has no INSERT OR REPLACE)
      auto del = con->Prepare("DELETE FROM entries WHERE id = $1");
      (void)del->Execute(entry.id);

      auto stmt = con->Prepare(
          "INSERT INTO entries "
          "(id, content, source, user_id, agent_id, session_id, type, "
          " importance, access_count, created_at, accessed_at, embedding) "
          "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)");

      auto created_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             entry.created_at.time_since_epoch())
                             .count();
      auto accessed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              entry.accessed_at.time_since_epoch())
              .count();

      // Serialize embedding to BLOB (BLOB_RAW accepts raw bytes)
      duckdb::Value emb_val;
      if (!entry.embedding.empty()) {
        std::string raw(
            reinterpret_cast<const char *>(entry.embedding.data()),
            entry.embedding.size() * sizeof(float));
        emb_val = duckdb::Value::BLOB_RAW(raw);
      } else {
        emb_val = duckdb::Value(duckdb::LogicalType::BLOB);
      }

      auto result = stmt->Execute(
          entry.id, entry.content, entry.source, entry.user_id,
          entry.agent_id, entry.session_id, entry.type,
          static_cast<double>(entry.importance),
          static_cast<int32_t>(entry.access_count), created_us, accessed_us,
          emb_val);

      if (result->HasError())
        return make_error(ErrorCode::MemoryWriteFailed,
                          "DuckDBLTM: insert failed: " +
                              result->GetError());
    } catch (const std::exception &e) {
      return make_error(ErrorCode::MemoryWriteFailed,
                        std::string("DuckDBLTM: ") + e.what());
    }

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

    // Remove metadata cache
    id_meta_.erase(id);

    // DELETE from DuckDB
    auto *con = db_.connection();
    if (!con)
      return false;

    try {
      auto stmt = con->Prepare("DELETE FROM entries WHERE id = $1");
      auto result = stmt->Execute(id);
      return !result->HasError();
    } catch (const std::exception &) {
      return false;
    }
  }

  Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                           const MemoryFilter &filter,
                                           size_t top_k = 5) override {
    std::lock_guard lk(mu_);
    std::vector<SearchResult> results;

    if (q_emb.empty() || !hnsw_index_ ||
        hnsw_index_->cur_element_count == 0) {
      // Fallback: linear scan via DuckDB
      return search_linear_locked(filter, top_k);
    }

    if (q_emb.size() != dim_)
      return make_error(ErrorCode::InvalidArgument, "Dim mismatch");

    // HNSW 过滤器
    DuckDBLTMFilter ltm_filter(filter, label_to_id_, id_meta_);

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

    auto *con = db_.connection();
    if (!con)
      return results;

    try {
      auto qr = con->Query("SELECT " + std::string(SELECT_ALL_COLS) +
                            " FROM entries");
      if (qr->HasError())
        return results;

      while (auto chunk = qr->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          results.push_back(chunk_row_to_entry(*chunk, row));
        }
      }
    } catch (const std::exception &) {
    }
    return results;
  }

  size_t size() const override {
    std::lock_guard lk(mu_);
    if (!db_.is_open())
      return 0;
    auto *con = db_.connection();
    if (!con)
      return 0;
    try {
      auto result = con->Query("SELECT COUNT(*) FROM entries");
      if (result->HasError())
        return 0;
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0)
        return 0;
      return static_cast<size_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
    } catch (const std::exception &) {
      return 0;
    }
  }

  std::string name() const override { return "DuckDBLongTermMemory"; }

  // ── 结构化 SQL 查询接口（DuckDB 特有）──────────────────

  /// 执行任意 SQL 查询，返回 JSON 数组
  /// 仅允许 SELECT 查询，禁止 DDL/DML 防注入
  struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string error;
    bool ok() const { return error.empty(); }
  };

  QueryResult sql_query(const std::string &sql) {
    std::lock_guard lk(mu_);
    QueryResult qr;

    // 安全：仅允许 SELECT
    std::string trimmed = sql;
    auto pos = trimmed.find_first_not_of(" \t\r\n");
    if (pos != std::string::npos)
      trimmed = trimmed.substr(pos);

    // Case-insensitive check for SELECT
    if (trimmed.size() < 6) {
      qr.error = "Query too short";
      return qr;
    }
    std::string prefix = trimmed.substr(0, 6);
    for (auto &c : prefix)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (prefix != "select") {
      qr.error = "Only SELECT queries are allowed";
      return qr;
    }

    auto *con = db_.connection();
    if (!con) {
      qr.error = "No database connection";
      return qr;
    }

    try {
      auto result = con->Query(sql);
      if (result->HasError()) {
        qr.error = result->GetError();
        return qr;
      }

      // Extract column names
      for (idx_t i = 0; i < result->ColumnCount(); i++) {
        qr.columns.push_back(result->ColumnName(i));
      }

      // Extract rows
      while (auto chunk = result->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          std::vector<std::string> row_data;
          for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
            auto val = chunk->GetValue(col, row);
            row_data.push_back(val.IsNull() ? "NULL" : val.ToString());
          }
          qr.rows.push_back(std::move(row_data));
        }
      }
    } catch (const std::exception &e) {
      qr.error = e.what();
    }

    return qr;
  }

  /// 预定义的结构化查询：按字段过滤 + 排序 + 限制
  /// @note where_clause 和 order_by 经过安全校验（禁止分号、DDL 关键词）
  QueryResult query_by_filter(const std::string &where_clause = "1=1",
                              const std::string &order_by = "importance DESC",
                              size_t limit = 100) {
    if (!validate_sql_fragment(where_clause) ||
        !validate_sql_fragment(order_by)) {
      return QueryResult{{}, {}, "Unsafe SQL fragment detected"};
    }
    std::string sql = "SELECT id, content, source, user_id, type, importance, "
                      "access_count, created_at "
                      "FROM entries WHERE " +
                      where_clause + " ORDER BY " + order_by +
                      " LIMIT " + std::to_string(limit);
    return sql_query(sql);
  }

  /// 聚合查询：按类型/用户统计
  /// @note group_by 和 agg 经过安全校验
  QueryResult aggregate(const std::string &group_by,
                         const std::string &agg = "COUNT(*) as cnt, "
                                                   "AVG(importance) as avg_imp") {
    if (!validate_sql_fragment(group_by) ||
        !validate_sql_fragment(agg)) {
      return QueryResult{{}, {}, "Unsafe SQL fragment detected"};
    }
    std::string sql = "SELECT " + group_by + ", " + agg +
                      " FROM entries GROUP BY " + group_by +
                      " ORDER BY cnt DESC";
    return sql_query(sql);
  }

private:
  // ── SQL fragment 安全校验（防注入）──────────────────
  static bool validate_sql_fragment(const std::string &s) {
    // 禁止分号（多语句注入）
    if (s.find(';') != std::string::npos)
      return false;
    // 禁止 NUL 字节
    if (s.find('\0') != std::string::npos)
      return false;
    // 禁止 DDL/DML 关键词（大小写不敏感）
    auto lower = s;
    for (auto &c : lower)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char *kw :
         {"drop ", "alter ", "create ", "insert ", "update ", "delete ",
          "truncate ", "exec ", "execute "}) {
      if (lower.find(kw) != std::string::npos)
        return false;
    }
    return true;
  }

  // ── 元数据缓存（用于 HNSW filter）──
  struct MetaRecord {
    std::string user_id;
    std::string agent_id;
    std::string session_id;
    std::string type;
    float importance;
  };

  // HNSW 过滤器
  class DuckDBLTMFilter : public hnswlib::BaseFilterFunctor {
  public:
    DuckDBLTMFilter(
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
        "  id VARCHAR PRIMARY KEY,"
        "  content VARCHAR NOT NULL,"
        "  source VARCHAR DEFAULT '',"
        "  user_id VARCHAR DEFAULT '',"
        "  agent_id VARCHAR DEFAULT '',"
        "  session_id VARCHAR DEFAULT '',"
        "  type VARCHAR DEFAULT 'episodic',"
        "  importance DOUBLE DEFAULT 0.5,"
        "  access_count INTEGER DEFAULT 0,"
        "  created_at BIGINT DEFAULT 0,"
        "  accessed_at BIGINT DEFAULT 0,"
        "  embedding BLOB"
        ")");

    // 为 filter 常用字段建索引
    db_.exec("CREATE INDEX IF NOT EXISTS idx_user_id ON entries(user_id)");
    db_.exec("CREATE INDEX IF NOT EXISTS idx_type ON entries(type)");
    db_.exec(
        "CREATE INDEX IF NOT EXISTS idx_importance ON entries(importance)");
  }

  static constexpr const char *SELECT_ALL_COLS =
      "id, content, source, user_id, agent_id, session_id, type, "
      "importance, access_count, created_at, accessed_at, embedding";

  static MemoryEntry chunk_row_to_entry(duckdb::DataChunk &chunk, idx_t row) {
    MemoryEntry entry;
    auto get_str = [&](idx_t col) -> std::string {
      auto val = chunk.GetValue(col, row);
      return val.IsNull() ? "" : val.ToString();
    };

    entry.id = get_str(0);
    entry.content = get_str(1);
    entry.source = get_str(2);
    entry.user_id = get_str(3);
    entry.agent_id = get_str(4);
    entry.session_id = get_str(5);
    entry.type = get_str(6);

    auto imp_val = chunk.GetValue(7, row);
    entry.importance =
        imp_val.IsNull() ? 0.5f : static_cast<float>(imp_val.GetValue<double>());

    auto ac_val = chunk.GetValue(8, row);
    entry.access_count =
        ac_val.IsNull() ? 0 : static_cast<uint32_t>(ac_val.GetValue<int32_t>());

    auto ct_val = chunk.GetValue(9, row);
    int64_t created_us = ct_val.IsNull() ? 0 : ct_val.GetValue<int64_t>();
    auto at_val = chunk.GetValue(10, row);
    int64_t accessed_us = at_val.IsNull() ? 0 : at_val.GetValue<int64_t>();
    entry.created_at = TimePoint(std::chrono::microseconds(created_us));
    entry.accessed_at = TimePoint(std::chrono::microseconds(accessed_us));

    // Embedding from BLOB（校验对齐）
    auto emb_val = chunk.GetValue(11, row);
    if (!emb_val.IsNull()) {
      auto blob = duckdb::StringValue::Get(emb_val);
      if (blob.size() % sizeof(float) == 0) {
        size_t num_floats = blob.size() / sizeof(float);
        if (num_floats > 0) {
          const float *data = reinterpret_cast<const float *>(blob.data());
          entry.embedding.assign(data, data + num_floats);
        }
      }
    }

    return entry;
  }

  Result<MemoryEntry> read_locked(const std::string &id) {
    auto *con = db_.connection();
    if (!con)
      return make_error(ErrorCode::MemoryReadFailed,
                        "DuckDBLTM: no connection");

    try {
      auto stmt = con->Prepare(
          std::string("SELECT ") + SELECT_ALL_COLS +
          " FROM entries WHERE id = $1");
      auto result = stmt->Execute(id);
      if (result->HasError())
        return make_error(ErrorCode::MemoryReadFailed,
                          "DuckDBLTM: query failed");

      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0)
        return make_error(ErrorCode::NotFound, "DuckDBLTM: entry not found");

      return chunk_row_to_entry(*chunk, 0);
    } catch (const std::exception &e) {
      return make_error(ErrorCode::MemoryReadFailed,
                        std::string("DuckDBLTM: ") + e.what());
    }
  }

  Result<std::vector<SearchResult>>
  search_linear_locked(const MemoryFilter &filter, size_t top_k) {
    std::vector<SearchResult> results;

    auto *con = db_.connection();
    if (!con)
      return results;

    // Build WHERE clause with escaped string literals
    auto escape = [](const std::string &s) -> std::string {
      if (s.find('\0') != std::string::npos)
        return "''"; // reject NUL bytes
      std::string out;
      out.reserve(s.size() + 2);
      out += '\'';
      for (char c : s) {
        if (c == '\'')
          out += "''";
        else
          out += c;
      }
      out += '\'';
      return out;
    };

    std::string sql = std::string("SELECT ") + SELECT_ALL_COLS +
                      " FROM entries WHERE 1=1";
    if (filter.user_id)
      sql += " AND user_id = " + escape(*filter.user_id);
    if (filter.agent_id)
      sql += " AND agent_id = " + escape(*filter.agent_id);
    if (filter.session_id)
      sql += " AND session_id = " + escape(*filter.session_id);
    if (filter.type)
      sql += " AND type = " + escape(*filter.type);
    sql += " ORDER BY importance DESC LIMIT " + std::to_string(top_k);

    try {
      auto qr = con->Query(sql);
      if (qr->HasError())
        return results;

      while (auto chunk = qr->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          auto entry = chunk_row_to_entry(*chunk, row);
          results.push_back({std::move(entry), 0.0f});
        }
      }
    } catch (const std::exception &) {
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
      try {
        hnsw_index_->saveIndex((dir_ / "hnsw_index.bin").string());
      } catch (const std::exception &e) {
        LOG_WARN(std::string("[DuckDBLTM] Failed to save HNSW: ") + e.what());
      }
    }
  }

  void load_hnsw_index() {
    // 从 DuckDB 加载所有 embedding 重建 HNSW
    auto *con = db_.connection();
    if (!con)
      return;

    try {
      auto qr = con->Query(
          "SELECT id, user_id, agent_id, session_id, type, importance, "
          "embedding "
          "FROM entries WHERE embedding IS NOT NULL");
      if (qr->HasError())
        return;

      std::vector<std::pair<std::string, Embedding>> entries_with_emb;
      std::vector<MetaRecord> metas;

      while (auto chunk = qr->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          auto id_val = chunk->GetValue(0, row);
          if (id_val.IsNull())
            continue;
          std::string id = id_val.ToString();

          MetaRecord meta;
          auto v1 = chunk->GetValue(1, row);
          auto v2 = chunk->GetValue(2, row);
          auto v3 = chunk->GetValue(3, row);
          auto v4 = chunk->GetValue(4, row);
          meta.user_id = v1.IsNull() ? "" : v1.ToString();
          meta.agent_id = v2.IsNull() ? "" : v2.ToString();
          meta.session_id = v3.IsNull() ? "" : v3.ToString();
          meta.type = v4.IsNull() ? "" : v4.ToString();
          auto v5 = chunk->GetValue(5, row);
          meta.importance = v5.IsNull() ? 0.5f
                            : static_cast<float>(v5.GetValue<double>());

          auto emb_val = chunk->GetValue(6, row);
          if (emb_val.IsNull())
            continue;

          auto blob = duckdb::StringValue::Get(emb_val);
          if (blob.size() % sizeof(float) != 0 || blob.empty())
            continue;
          size_t num_floats = blob.size() / sizeof(float);

          const float *data = reinterpret_cast<const float *>(blob.data());
          Embedding emb(data, data + num_floats);

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
      }

      if (entries_with_emb.empty())
        return;

      // 从 DuckDB embeddings 重建 HNSW
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
    } catch (const std::exception &e) {
      LOG_WARN(std::string("[DuckDBLTM] Failed to load HNSW: ") + e.what());
    }
  }

  mutable std::mutex mu_;
  fs::path dir_;
  DuckDBConn db_;
  size_t max_elements_;
  uint64_t id_counter_{0};

  // HNSW 索引
  std::unique_ptr<hnswlib::InnerProductSpace> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_index_;
  size_t dim_{0};
  hnswlib::labeltype label_counter_{0};
  std::unordered_map<std::string, hnswlib::labeltype> id_to_label_;
  std::unordered_map<hnswlib::labeltype, std::string> label_to_id_;

  // 元数据缓存（避免 HNSW filter 时反复读 DuckDB）
  std::unordered_map<std::string, MetaRecord> id_meta_;
};

} // namespace agentos::memory
