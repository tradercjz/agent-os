#include "agentos/memory/duckdb_store.hpp"
#include <filesystem>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::memory;
namespace fs = std::filesystem;

class DuckDBStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(info, nullptr);
    test_dir_ = fs::temp_directory_path() /
                fs::path("agentos_duckdb_test_" +
                         std::string(info->test_suite_name()) + "_" +
                         std::string(info->name()));
    fs::remove_all(test_dir_);
    store_ = std::make_unique<DuckDBLongTermMemory>(test_dir_);
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  fs::path test_dir_;
  std::unique_ptr<DuckDBLongTermMemory> store_;
};

// ── CRUD 基础测试 ─────────────────────────────────
TEST_F(DuckDBStoreTest, WriteAndReadEntry) {
  MemoryEntry entry;
  entry.content = "Hello from DuckDB";
  entry.source = "agent_test";
  entry.user_id = "user_1";
  entry.agent_id = "agent_1";
  entry.session_id = "session_1";
  entry.type = "episodic";
  entry.importance = 0.9f;

  auto result = store_->write(entry);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->starts_with("lt_"));

  // Read back
  auto read_result = store_->read(*result);
  ASSERT_TRUE(read_result);
  EXPECT_EQ(read_result->content, "Hello from DuckDB");
  EXPECT_EQ(read_result->user_id, "user_1");
  EXPECT_EQ(read_result->agent_id, "agent_1");
  EXPECT_EQ(read_result->type, "episodic");
  EXPECT_NEAR(read_result->importance, 0.9f, 0.01f);
}

TEST_F(DuckDBStoreTest, ForgetEntry) {
  MemoryEntry entry;
  entry.content = "Ephemeral memory";

  auto id = store_->write(entry);
  ASSERT_TRUE(id);
  EXPECT_EQ(store_->size(), 1u);

  auto result = store_->forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);

  EXPECT_EQ(store_->size(), 0u);

  auto read = store_->read(*id);
  EXPECT_FALSE(read);
}

TEST_F(DuckDBStoreTest, SizeReflectsEntries) {
  EXPECT_EQ(store_->size(), 0u);

  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "Memory #" + std::to_string(i);
    (void)store_->write(e);
  }

  EXPECT_EQ(store_->size(), 5u);
}

TEST_F(DuckDBStoreTest, GetAllReturnsAllEntries) {
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.user_id = "user_x";
    (void)store_->write(e);
  }

  auto all = store_->get_all();
  EXPECT_EQ(all.size(), 3u);
}

// ── Embedding 向量存取测试 ───────────────────────
TEST_F(DuckDBStoreTest, EmbeddingPersistence) {
  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  MemoryEntry entry;
  entry.content = "Embedded memory";
  entry.embedding = emb;

  auto id = store_->write(entry);
  ASSERT_TRUE(id);

  auto read = store_->read(*id);
  ASSERT_TRUE(read);
  ASSERT_EQ(read->embedding.size(), 128u);

  for (size_t i = 0; i < emb.size(); ++i) {
    EXPECT_NEAR(read->embedding[i], emb[i], 1e-5f);
  }
}

// ── HNSW 语义检索测试 ───────────────────────────
TEST_F(DuckDBStoreTest, HNSWSemanticSearch) {
  Embedding emb1(128, 0.1f);
  emb1[0] = 0.99f;
  Embedding emb2(128, 0.1f);
  emb2[1] = 0.99f;

  auto normalize = [](Embedding &e) {
    float norm = 0;
    for (float v : e)
      norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : e)
      v /= norm;
  };
  normalize(emb1);
  normalize(emb2);

  MemoryEntry e1;
  e1.content = "Memory about cats";
  e1.embedding = emb1;
  e1.user_id = "user_1";
  e1.importance = 0.8f;

  MemoryEntry e2;
  e2.content = "Memory about dogs";
  e2.embedding = emb2;
  e2.user_id = "user_1";
  e2.importance = 0.9f;

  (void)store_->write(e1);
  (void)store_->write(e2);

  Embedding query(128, 0.1f);
  query[0] = 0.95f;
  normalize(query);

  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Memory about cats");
}

// ── Scope 过滤测试 ─────────────────────────────
TEST_F(DuckDBStoreTest, ScopeFilterInSearch) {
  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  MemoryEntry e1;
  e1.content = "User A's memory";
  e1.embedding = emb;
  e1.user_id = "user_a";

  MemoryEntry e2;
  e2.content = "User B's memory";
  e2.embedding = emb;
  e2.user_id = "user_b";

  (void)store_->write(e1);
  (void)store_->write(e2);

  MemoryFilter filter;
  filter.user_id = "user_a";

  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ(results->front().entry.user_id, "user_a");
}

// ── 跨进程持久化测试（模拟重启）───────────────
TEST_F(DuckDBStoreTest, PersistenceAcrossRestarts) {
  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  {
    MemoryEntry e1;
    e1.content = "Persistent memory 1";
    e1.embedding = emb;
    e1.user_id = "user_1";
    e1.importance = 0.8f;
    (void)store_->write(e1);

    MemoryEntry e2;
    e2.content = "Persistent memory 2";
    e2.user_id = "user_1";
    e2.importance = 0.6f;
    (void)store_->write(e2);
  }

  EXPECT_EQ(store_->size(), 2u);

  // 销毁并重新创建（模拟重启）
  store_.reset();
  store_ = std::make_unique<DuckDBLongTermMemory>(test_dir_);

  EXPECT_EQ(store_->size(), 2u);

  auto all = store_->get_all();
  EXPECT_EQ(all.size(), 2u);

  bool found1 = false, found2 = false;
  for (const auto &e : all) {
    if (e.content == "Persistent memory 1")
      found1 = true;
    if (e.content == "Persistent memory 2")
      found2 = true;
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);

  // HNSW 索引也恢复了
  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Persistent memory 1");
}

// ── SQL 结构化查询测试 ──────────────────────────
TEST_F(DuckDBStoreTest, SQLQueryBasic) {
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.user_id = (i < 3) ? "user_a" : "user_b";
    e.type = (i % 2 == 0) ? "episodic" : "semantic";
    e.importance = 0.1f * (i + 1);
    (void)store_->write(e);
  }

  // Basic SELECT
  auto qr = store_->sql_query("SELECT id, content FROM entries ORDER BY id");
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.columns.size(), 2u);
  EXPECT_EQ(qr.rows.size(), 5u);

  // Filter query
  auto qr2 = store_->sql_query(
      "SELECT content FROM entries WHERE user_id = 'user_a'");
  ASSERT_TRUE(qr2.ok());
  EXPECT_EQ(qr2.rows.size(), 3u);

  // Aggregation
  auto qr3 = store_->sql_query(
      "SELECT user_id, COUNT(*) as cnt FROM entries GROUP BY user_id");
  ASSERT_TRUE(qr3.ok());
  EXPECT_EQ(qr3.rows.size(), 2u);
}

TEST_F(DuckDBStoreTest, SQLQueryRejectsDML) {
  auto qr = store_->sql_query("DELETE FROM entries");
  EXPECT_FALSE(qr.ok());
  EXPECT_EQ(qr.error, "Only SELECT queries are allowed");

  auto qr2 = store_->sql_query("DROP TABLE entries");
  EXPECT_FALSE(qr2.ok());

  auto qr3 = store_->sql_query("INSERT INTO entries (id) VALUES ('x')");
  EXPECT_FALSE(qr3.ok());
}

TEST_F(DuckDBStoreTest, QueryByFilterRejectsInjection) {
  MemoryEntry e;
  e.content = "test";
  (void)store_->write(e);

  // Semicolon injection
  auto qr = store_->query_by_filter("1=1; DROP TABLE entries --");
  EXPECT_FALSE(qr.ok());

  // DDL keyword injection
  auto qr2 = store_->query_by_filter("1=1", "id; DELETE FROM entries --");
  EXPECT_FALSE(qr2.ok());

  // NUL byte
  std::string with_nul = "type='x'";
  with_nul += '\0';
  with_nul += "OR 1=1";
  auto qr3 = store_->query_by_filter(with_nul);
  EXPECT_FALSE(qr3.ok());

  // Valid queries should still work
  auto qr4 = store_->query_by_filter("importance > 0.0");
  EXPECT_TRUE(qr4.ok());
  EXPECT_EQ(qr4.rows.size(), 1u);
}

TEST_F(DuckDBStoreTest, BlobEdgeCases) {
  // Empty embedding should store and read back as empty
  MemoryEntry e1;
  e1.content = "No embedding";
  auto id1 = store_->write(e1);
  ASSERT_TRUE(id1);
  auto r1 = store_->read(*id1);
  ASSERT_TRUE(r1);
  EXPECT_TRUE(r1->embedding.empty());

  // Single-float embedding
  MemoryEntry e2;
  e2.content = "Tiny embedding";
  e2.embedding = {1.0f};
  auto id2 = store_->write(e2);
  ASSERT_TRUE(id2);
  auto r2 = store_->read(*id2);
  ASSERT_TRUE(r2);
  ASSERT_EQ(r2->embedding.size(), 1u);
  EXPECT_NEAR(r2->embedding[0], 1.0f, 1e-5f);
}

TEST_F(DuckDBStoreTest, AggregateQuery) {
  for (int i = 0; i < 6; ++i) {
    MemoryEntry e;
    e.content = "M" + std::to_string(i);
    e.type = (i < 4) ? "episodic" : "semantic";
    e.importance = 0.5f + 0.1f * i;
    (void)store_->write(e);
  }

  auto qr = store_->aggregate("type");
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.rows.size(), 2u);
}

// ── MemorySystem DuckDB 后端集成测试 ──────────────
TEST_F(DuckDBStoreTest, MemorySystemWithDuckDBBackend) {
  auto duckdb_ltm = std::make_unique<DuckDBLongTermMemory>(test_dir_);
  MemorySystem mem(test_dir_, std::move(duckdb_ltm));

  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  auto id =
      mem.add_episodic("Important event", emb, "user_1", "session_1", 0.9f);
  ASSERT_TRUE(id);

  EXPECT_GE(mem.long_term().size(), 1u);
  EXPECT_EQ(mem.long_term().name(), "DuckDBLongTermMemory");

  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = mem.recall(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);

  bool found = false;
  for (const auto &r : *results) {
    if (r.entry.content == "Important event")
      found = true;
  }
  EXPECT_TRUE(found);
}

TEST_F(DuckDBStoreTest, NameIsDuckDBLongTermMemory) {
  EXPECT_EQ(store_->name(), "DuckDBLongTermMemory");
}

TEST_F(DuckDBStoreTest, MemorySystemDuckDBEnumCreatesCorrectBackend) {
  auto duckdb_dir = fs::temp_directory_path() / "agentos_duckdb_enum_test";
  fs::remove_all(duckdb_dir);

  {
    MemorySystem mem(duckdb_dir, MemorySystem::LTMBackend::DuckDB);
    EXPECT_EQ(mem.long_term().name(), "DuckDBLongTermMemory");

    Embedding emb(64, 0.1f);
    float norm = 0;
    for (float v : emb)
      norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : emb)
      v /= norm;

    (void)mem.remember("DuckDB enum test data", emb, "agent", 0.9f);
    EXPECT_GE(mem.long_term().size(), 1u);
  }

  fs::remove_all(duckdb_dir);
}

// ── Helper: normalize embedding in-place ────────────────
static void normalize_emb_duck(Embedding &e) {
  float norm = 0;
  for (float v : e)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : e)
    v /= norm;
}

// ── Forget entry that has an HNSW embedding (covers markDelete path) ──
TEST_F(DuckDBStoreTest, ForgetEntryWithEmbedding) {
  Embedding emb(128, 0.1f);
  emb[0] = 0.99f;
  normalize_emb_duck(emb);

  MemoryEntry entry;
  entry.content = "Memory with embedding";
  entry.embedding = emb;
  entry.user_id = "user_1";

  auto id = store_->write(entry);
  ASSERT_TRUE(id);
  EXPECT_EQ(store_->size(), 1u);

  // Forget should remove from both DuckDB and HNSW
  auto result = store_->forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);
  EXPECT_EQ(store_->size(), 0u);

  // Search should return empty results after forget
  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_EQ(results->size(), 0u);
}

// ── Search with empty embedding triggers linear scan fallback ──
TEST_F(DuckDBStoreTest, SearchEmptyEmbeddingFallsBackToLinearScan) {
  MemoryEntry e1;
  e1.content = "Important thing";
  e1.user_id = "user_1";
  e1.importance = 0.9f;
  (void)store_->write(e1);

  MemoryEntry e2;
  e2.content = "Less important";
  e2.user_id = "user_1";
  e2.importance = 0.3f;
  (void)store_->write(e2);

  // Empty embedding -> linear scan fallback
  Embedding empty_emb;
  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(empty_emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Important thing");
}

// ── Linear scan with all filter fields (agent_id, session_id, type) ──
TEST_F(DuckDBStoreTest, LinearScanWithAllFilters) {
  MemoryEntry e1;
  e1.content = "Match all filters";
  e1.user_id = "user_1";
  e1.agent_id = "agent_1";
  e1.session_id = "sess_1";
  e1.type = "episodic";
  e1.importance = 0.9f;
  (void)store_->write(e1);

  MemoryEntry e2;
  e2.content = "Wrong agent";
  e2.user_id = "user_1";
  e2.agent_id = "agent_2";
  e2.session_id = "sess_1";
  e2.type = "episodic";
  e2.importance = 0.8f;
  (void)store_->write(e2);

  MemoryEntry e3;
  e3.content = "Wrong type";
  e3.user_id = "user_1";
  e3.agent_id = "agent_1";
  e3.session_id = "sess_1";
  e3.type = "semantic";
  e3.importance = 0.7f;
  (void)store_->write(e3);

  Embedding empty_emb;
  MemoryFilter filter;
  filter.user_id = "user_1";
  filter.agent_id = "agent_1";
  filter.session_id = "sess_1";
  filter.type = "episodic";

  auto results = store_->search(empty_emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Match all filters");
}

// ── Search with dimension mismatch ──
TEST_F(DuckDBStoreTest, SearchDimMismatchReturnsError) {
  Embedding emb128(128, 0.1f);
  emb128[0] = 0.99f;
  normalize_emb_duck(emb128);

  MemoryEntry e;
  e.content = "Some memory";
  e.embedding = emb128;
  e.user_id = "user_1";
  (void)store_->write(e);

  Embedding query64(64, 0.5f);
  normalize_emb_duck(query64);

  MemoryFilter filter;
  auto results = store_->search(query64, filter, 5);
  EXPECT_FALSE(results);
}

// ── Search with top_k smaller than results triggers resize ──
TEST_F(DuckDBStoreTest, SearchResultsTruncatedToTopK) {
  auto normalize = [](Embedding &e) {
    float norm = 0;
    for (float v : e) norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : e) v /= norm;
  };

  for (int i = 0; i < 5; ++i) {
    Embedding emb(64, 0.1f);
    emb[i % 64] = 0.99f;
    normalize(emb);

    MemoryEntry e;
    e.content = "Entry " + std::to_string(i);
    e.embedding = emb;
    e.user_id = "user_1";
    e.importance = 0.5f + 0.1f * i;
    (void)store_->write(e);
  }

  Embedding query(64, 0.1f);
  query[0] = 0.95f;
  normalize(query);

  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(query, filter, 2);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 2u);
}

// ── HNSW search with agent_id/session_id/type filters ──
TEST_F(DuckDBStoreTest, HNSWSearchWithMultipleFilterFields) {
  auto normalize = [](Embedding &e) {
    float norm = 0;
    for (float v : e) norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : e) v /= norm;
  };

  Embedding emb(64, 0.1f);
  emb[0] = 0.99f;
  normalize(emb);

  MemoryEntry e1;
  e1.content = "Target entry";
  e1.embedding = emb;
  e1.user_id = "user_1";
  e1.agent_id = "agent_A";
  e1.session_id = "sess_X";
  e1.type = "episodic";
  e1.importance = 0.9f;
  (void)store_->write(e1);

  Embedding emb2(64, 0.1f);
  emb2[1] = 0.99f;
  normalize(emb2);

  MemoryEntry e2;
  e2.content = "Wrong agent entry";
  e2.embedding = emb2;
  e2.user_id = "user_1";
  e2.agent_id = "agent_B";
  e2.session_id = "sess_X";
  e2.type = "episodic";
  e2.importance = 0.9f;
  (void)store_->write(e2);

  Embedding query(64, 0.1f);
  query[0] = 0.95f;
  normalize(query);

  MemoryFilter filter;
  filter.user_id = "user_1";
  filter.agent_id = "agent_A";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.agent_id, "agent_A");

  MemoryFilter filter2;
  filter2.session_id = "sess_X";
  auto results2 = store_->search(query, filter2, 5);
  ASSERT_TRUE(results2);
  ASSERT_GE(results2->size(), 1u);

  MemoryFilter filter3;
  filter3.type = "episodic";
  auto results3 = store_->search(query, filter3, 5);
  ASSERT_TRUE(results3);
  ASSERT_GE(results3->size(), 1u);
}

// ── Write with pre-set ID ──
TEST_F(DuckDBStoreTest, WriteWithCustomId) {
  MemoryEntry entry;
  entry.id = "custom_id_123";
  entry.content = "Custom ID entry";

  auto result = store_->write(entry);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, "custom_id_123");

  auto read = store_->read("custom_id_123");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->content, "Custom ID entry");
}

// ── Overwrite existing entry (DELETE + INSERT path) ──
TEST_F(DuckDBStoreTest, OverwriteExistingEntry) {
  Embedding emb(64, 0.1f);
  emb[0] = 0.99f;
  normalize_emb_duck(emb);

  MemoryEntry entry;
  entry.id = "overwrite_me";
  entry.content = "Version 1";
  entry.embedding = emb;
  (void)store_->write(entry);

  entry.content = "Version 2";
  (void)store_->write(entry);

  EXPECT_EQ(store_->size(), 1u);
  auto read = store_->read("overwrite_me");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->content, "Version 2");
}

// ── Read non-existent entry ──
TEST_F(DuckDBStoreTest, ReadNonExistentReturnsError) {
  auto result = store_->read("nonexistent_id");
  EXPECT_FALSE(result);
}

// ── Forget non-existent entry ──
TEST_F(DuckDBStoreTest, ForgetNonExistentReturnsFalseOrTrue) {
  // DuckDB DELETE of non-existent row should not error
  auto result = store_->forget("nonexistent_id");
  ASSERT_TRUE(result);
}

// ── SQL query: too short query ──
TEST_F(DuckDBStoreTest, SQLQueryTooShort) {
  auto qr = store_->sql_query("SEL");
  EXPECT_FALSE(qr.ok());
  EXPECT_EQ(qr.error, "Query too short");
}

// ── SQL query: case-insensitive SELECT check ──
TEST_F(DuckDBStoreTest, SQLQueryCaseInsensitiveSelect) {
  MemoryEntry e;
  e.content = "test";
  (void)store_->write(e);

  auto qr = store_->sql_query("  SELECT COUNT(*) FROM entries");
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.rows.size(), 1u);
}

// ── SQL query: error in DuckDB execution ──
TEST_F(DuckDBStoreTest, SQLQueryInvalidSQL) {
  auto qr = store_->sql_query("SELECT nonexistent_col FROM entries");
  EXPECT_FALSE(qr.ok());
}

// ── query_by_filter with valid filter ──
TEST_F(DuckDBStoreTest, QueryByFilterValid) {
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.user_id = (i < 3) ? "user_a" : "user_b";
    e.type = (i % 2 == 0) ? "episodic" : "semantic";
    e.importance = 0.1f * (i + 1);
    (void)store_->write(e);
  }

  auto qr = store_->query_by_filter("user_id = 'user_a'", "importance DESC", 10);
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.rows.size(), 3u);
}

// ── query_by_filter with ordering ──
TEST_F(DuckDBStoreTest, QueryByFilterOrdering) {
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.importance = 0.1f * (i + 1);
    (void)store_->write(e);
  }

  auto qr = store_->query_by_filter("1=1", "importance ASC", 10);
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.rows.size(), 3u);
}

// ── aggregate with unsafe SQL (DDL injection) ──
TEST_F(DuckDBStoreTest, AggregateRejectsUnsafeGroupBy) {
  auto qr = store_->aggregate("type; DROP TABLE entries --");
  EXPECT_FALSE(qr.ok());
  EXPECT_EQ(qr.error, "Unsafe SQL fragment detected");
}

// ── aggregate with unsafe agg parameter ──
TEST_F(DuckDBStoreTest, AggregateRejectsUnsafeAgg) {
  auto qr = store_->aggregate("type", "COUNT(*); DELETE FROM entries");
  EXPECT_FALSE(qr.ok());
  EXPECT_EQ(qr.error, "Unsafe SQL fragment detected");
}

// ── query_by_filter: NUL byte injection in order_by ──
TEST_F(DuckDBStoreTest, QueryByFilterRejectsNulInOrderBy) {
  std::string bad_order = "importance DESC";
  bad_order += '\0';
  bad_order += "; DROP TABLE entries";
  auto qr = store_->query_by_filter("1=1", bad_order);
  EXPECT_FALSE(qr.ok());
}

// ── Search: no HNSW index built, entries without embeddings ──
TEST_F(DuckDBStoreTest, SearchNoHNSWFallsBackToLinear) {
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "No-emb " + std::to_string(i);
    e.user_id = "user_1";
    e.importance = 0.5f + 0.1f * i;
    (void)store_->write(e);
  }

  Embedding query(64, 0.5f);
  normalize_emb_duck(query);

  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
}

// ── SQL query: valid aggregation with multiple columns ──
TEST_F(DuckDBStoreTest, SQLQueryMultiColumnAgg) {
  for (int i = 0; i < 6; ++i) {
    MemoryEntry e;
    e.content = "M" + std::to_string(i);
    e.type = (i < 4) ? "episodic" : "semantic";
    e.importance = 0.5f + 0.1f * i;
    (void)store_->write(e);
  }

  auto qr = store_->sql_query(
      "SELECT type, COUNT(*) as cnt, AVG(importance) as avg_imp "
      "FROM entries GROUP BY type ORDER BY cnt DESC");
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.columns.size(), 3u);
  EXPECT_EQ(qr.rows.size(), 2u);
}

// ── validate_sql_fragment edge cases via query_by_filter ──
TEST_F(DuckDBStoreTest, QueryByFilterRejectsDDLKeywords) {
  // Various DDL/DML keywords
  EXPECT_FALSE(store_->query_by_filter("DROP TABLE entries").ok());
  EXPECT_FALSE(store_->query_by_filter("alter table entries").ok());
  EXPECT_FALSE(store_->query_by_filter("CREATE INDEX idx").ok());
  EXPECT_FALSE(store_->query_by_filter("INSERT INTO entries").ok());
  EXPECT_FALSE(store_->query_by_filter("UPDATE entries SET x=1").ok());
  EXPECT_FALSE(store_->query_by_filter("TRUNCATE TABLE entries").ok());
  EXPECT_FALSE(store_->query_by_filter("exec sp_something").ok());
  EXPECT_FALSE(store_->query_by_filter("EXECUTE something").ok());
}

// ── Empty embedding stored and read back correctly ──
TEST_F(DuckDBStoreTest, EmptyEmbeddingReadBack) {
  MemoryEntry e;
  e.content = "No embedding here";
  auto id = store_->write(e);
  ASSERT_TRUE(id);

  auto read = store_->read(*id);
  ASSERT_TRUE(read);
  EXPECT_TRUE(read->embedding.empty());
}

// ── query_by_filter with limit ──
TEST_F(DuckDBStoreTest, QueryByFilterWithLimit) {
  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.importance = 0.1f * (i + 1);
    (void)store_->write(e);
  }

  auto qr = store_->query_by_filter("1=1", "importance DESC", 3);
  ASSERT_TRUE(qr.ok());
  EXPECT_EQ(qr.rows.size(), 3u);
}
