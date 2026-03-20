#include "agentos/memory/sqlite_store.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::memory;
namespace fs = std::filesystem;

class SQLiteStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "agentos_sqlite_test";
    fs::remove_all(test_dir_);
    store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);
  }

  void TearDown() override { fs::remove_all(test_dir_); }

  fs::path test_dir_;
  std::unique_ptr<SQLiteLongTermMemory> store_;
};

// ── CRUD 基础测试 ─────────────────────────────────
TEST_F(SQLiteStoreTest, WriteAndReadEntry) {
  MemoryEntry entry;
  entry.content = "Hello from SQLite";
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
  EXPECT_EQ(read_result->content, "Hello from SQLite");
  EXPECT_EQ(read_result->user_id, "user_1");
  EXPECT_EQ(read_result->agent_id, "agent_1");
  EXPECT_EQ(read_result->type, "episodic");
  EXPECT_FLOAT_EQ(read_result->importance, 0.9f);
}

TEST_F(SQLiteStoreTest, ForgetEntry) {
  MemoryEntry entry;
  entry.content = "Ephemeral memory";

  auto id = store_->write(entry);
  ASSERT_TRUE(id);
  EXPECT_EQ(store_->size(), 1u);

  auto result = store_->forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);

  EXPECT_EQ(store_->size(), 0u);

  // Read after forget should fail
  auto read = store_->read(*id);
  EXPECT_FALSE(read);
}

TEST_F(SQLiteStoreTest, SizeReflectsEntries) {
  EXPECT_EQ(store_->size(), 0u);

  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "Memory #" + std::to_string(i);
    (void)store_->write(e);
  }

  EXPECT_EQ(store_->size(), 5u);
}

TEST_F(SQLiteStoreTest, GetAllReturnsAllEntries) {
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
TEST_F(SQLiteStoreTest, EmbeddingPersistence) {
  Embedding emb(128, 0.5f);
  // Normalize
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

  // Read back and verify embedding
  auto read = store_->read(*id);
  ASSERT_TRUE(read);
  ASSERT_EQ(read->embedding.size(), 128u);

  for (size_t i = 0; i < emb.size(); ++i) {
    EXPECT_NEAR(read->embedding[i], emb[i], 1e-5f);
  }
}

// ── HNSW 语义检索测试 ───────────────────────────
TEST_F(SQLiteStoreTest, HNSWSemanticSearch) {
  // 创建两个不同方向的 embedding
  Embedding emb1(128, 0.1f);
  emb1[0] = 0.99f;
  Embedding emb2(128, 0.1f);
  emb2[1] = 0.99f;

  // Normalize
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

  // Query close to emb1
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
TEST_F(SQLiteStoreTest, ScopeFilterInSearch) {
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

  // Filter for user_a only
  MemoryFilter filter;
  filter.user_id = "user_a";

  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ(results->front().entry.user_id, "user_a");
}

// ── 跨进程持久化测试（模拟重启）───────────────
TEST_F(SQLiteStoreTest, PersistenceAcrossRestarts) {
  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  // 写入数据
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
  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);

  // 数据应该仍然存在
  EXPECT_EQ(store_->size(), 2u);

  auto all = store_->get_all();
  EXPECT_EQ(all.size(), 2u);

  // 验证数据内容
  bool found1 = false, found2 = false;
  for (const auto &e : all) {
    if (e.content == "Persistent memory 1")
      found1 = true;
    if (e.content == "Persistent memory 2")
      found2 = true;
  }
  EXPECT_TRUE(found1);
  EXPECT_TRUE(found2);

  // 验证 HNSW 索引也恢复了（能做向量检索）
  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Persistent memory 1");
}

// ── MemorySystem 集成测试 ──────────────────────
TEST_F(SQLiteStoreTest, MemorySystemWithSQLiteBackend) {
  // 使用自定义构造器注入 SQLite 后端
  auto sqlite_ltm = std::make_unique<SQLiteLongTermMemory>(test_dir_);
  MemorySystem mem(test_dir_, std::move(sqlite_ltm));

  // 写入高重要性记忆（应写入 LTM）
  Embedding emb(128, 0.5f);
  float norm = 0;
  for (float v : emb)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb)
    v /= norm;

  auto id = mem.add_episodic("Important event", emb, "user_1", "session_1", 0.9f);
  ASSERT_TRUE(id);

  // LTM 应该有条目（importance 0.9 > 0.7 阈值）
  EXPECT_GE(mem.long_term().size(), 1u);
  EXPECT_EQ(mem.long_term().name(), "SQLiteLongTermMemory");

  // 召回应该有结果
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

// ── 名称检查 ──────────────────────────────────
TEST_F(SQLiteStoreTest, NameIsSQLiteLongTermMemory) {
  EXPECT_EQ(store_->name(), "SQLiteLongTermMemory");
}

// ── MemorySystem::LTMBackend::SQLite 枚举构造器测试 ────
TEST_F(SQLiteStoreTest, MemorySystemSQLiteEnumCreatesCorrectBackend) {
  // 使用枚举值构造（而非手动注入）
  auto sqlite_dir = fs::temp_directory_path() / "agentos_sqlite_enum_test";
  fs::remove_all(sqlite_dir);

  {
    MemorySystem mem(sqlite_dir, MemorySystem::LTMBackend::SQLite);
    EXPECT_EQ(mem.long_term().name(), "SQLiteLongTermMemory");

    // 写入高重要性记忆
    Embedding emb(64, 0.1f);
    float norm = 0;
    for (float v : emb)
      norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : emb)
      v /= norm;

    (void)mem.remember("SQLite enum test data", emb, "agent", 0.9f);
    EXPECT_GE(mem.long_term().size(), 1u);
  }

  // 验证 FileBased 枚举仍正常工作
  {
    auto file_dir = fs::temp_directory_path() / "agentos_file_enum_test";
    fs::remove_all(file_dir);
    MemorySystem mem(file_dir, MemorySystem::LTMBackend::FileBased);
    EXPECT_EQ(mem.long_term().name(), "LongTermMemory");
    fs::remove_all(file_dir);
  }

  fs::remove_all(sqlite_dir);
}

// ── Helper: normalize embedding in-place ────────────────
static void normalize_emb(Embedding &e) {
  float norm = 0;
  for (float v : e)
    norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : e)
    v /= norm;
}

// ── Forget entry that has an HNSW embedding (covers markDelete path) ──
TEST_F(SQLiteStoreTest, ForgetEntryWithEmbedding) {
  Embedding emb(128, 0.1f);
  emb[0] = 0.99f;
  normalize_emb(emb);

  MemoryEntry entry;
  entry.content = "Memory with embedding";
  entry.embedding = emb;
  entry.user_id = "user_1";

  auto id = store_->write(entry);
  ASSERT_TRUE(id);
  EXPECT_EQ(store_->size(), 1u);

  // Forget should remove from both SQLite and HNSW
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
TEST_F(SQLiteStoreTest, SearchEmptyEmbeddingFallsBackToLinearScan) {
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
  // Linear scan returns results ordered by importance DESC
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "Important thing");
}

// ── Linear scan with all filter fields (agent_id, session_id, type) ──
TEST_F(SQLiteStoreTest, LinearScanWithAllFilters) {
  // Write entries without embeddings so HNSW is not built
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

  // Empty embedding -> linear fallback; all filter fields set
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
TEST_F(SQLiteStoreTest, SearchDimMismatchReturnsError) {
  Embedding emb128(128, 0.1f);
  emb128[0] = 0.99f;
  normalize_emb(emb128);

  MemoryEntry e;
  e.content = "Some memory";
  e.embedding = emb128;
  e.user_id = "user_1";
  (void)store_->write(e);

  // Query with different dimension
  Embedding query64(64, 0.5f);
  normalize_emb(query64);

  MemoryFilter filter;
  auto results = store_->search(query64, filter, 5);
  EXPECT_FALSE(results); // Should return error
}

// ── Search with top_k smaller than results triggers resize ──
TEST_F(SQLiteStoreTest, SearchResultsTruncatedToTopK) {
  auto normalize = [](Embedding &e) {
    float norm = 0;
    for (float v : e) norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : e) v /= norm;
  };

  // Write 5 entries with slightly different embeddings
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

  // Ask for only 2 results
  auto results = store_->search(query, filter, 2);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 2u);
}

// ── Search with HNSW filter that filters agent_id / session_id / type ──
TEST_F(SQLiteStoreTest, HNSWSearchWithMultipleFilterFields) {
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

  // Filter by agent_id
  MemoryFilter filter;
  filter.user_id = "user_1";
  filter.agent_id = "agent_A";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);
  EXPECT_EQ(results->front().entry.agent_id, "agent_A");

  // Filter by session_id
  MemoryFilter filter2;
  filter2.session_id = "sess_X";
  auto results2 = store_->search(query, filter2, 5);
  ASSERT_TRUE(results2);
  ASSERT_GE(results2->size(), 1u);

  // Filter by type
  MemoryFilter filter3;
  filter3.type = "episodic";
  auto results3 = store_->search(query, filter3, 5);
  ASSERT_TRUE(results3);
  ASSERT_GE(results3->size(), 1u);
}

// ── Write with pre-set ID (covers the non-empty id path) ──
TEST_F(SQLiteStoreTest, WriteWithCustomId) {
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

// ── Overwrite existing entry (INSERT OR REPLACE path + HNSW markDelete) ──
TEST_F(SQLiteStoreTest, OverwriteExistingEntry) {
  Embedding emb(64, 0.1f);
  emb[0] = 0.99f;
  normalize_emb(emb);

  MemoryEntry entry;
  entry.id = "overwrite_me";
  entry.content = "Version 1";
  entry.embedding = emb;
  (void)store_->write(entry);

  // Overwrite with same ID
  entry.content = "Version 2";
  (void)store_->write(entry);

  EXPECT_EQ(store_->size(), 1u);
  auto read = store_->read("overwrite_me");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->content, "Version 2");
}

// ── Read non-existent entry ──
TEST_F(SQLiteStoreTest, ReadNonExistentReturnsError) {
  auto result = store_->read("nonexistent_id");
  EXPECT_FALSE(result);
}

// ── Forget non-existent entry ──
TEST_F(SQLiteStoreTest, ForgetNonExistentReturnsFalse) {
  auto result = store_->forget("nonexistent_id");
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

// ── Empty embedding stored and read back correctly ──
TEST_F(SQLiteStoreTest, EmptyEmbeddingPersistence) {
  MemoryEntry entry;
  entry.content = "No embedding";
  auto id = store_->write(entry);
  ASSERT_TRUE(id);

  auto read = store_->read(*id);
  ASSERT_TRUE(read);
  EXPECT_TRUE(read->embedding.empty());
}

// ── Single-float embedding (blob edge case) ──
TEST_F(SQLiteStoreTest, SingleFloatEmbedding) {
  MemoryEntry entry;
  entry.content = "Tiny embedding";
  entry.embedding = {1.0f};
  auto id = store_->write(entry);
  ASSERT_TRUE(id);

  auto read = store_->read(*id);
  ASSERT_TRUE(read);
  ASSERT_EQ(read->embedding.size(), 1u);
  EXPECT_NEAR(read->embedding[0], 1.0f, 1e-5f);
}

// ── Migration from file-based format ──
TEST_F(SQLiteStoreTest, MigrateFromFiles) {
  // First, destroy the current store
  store_.reset();

  // Create the old-format files in the test directory
  auto idx_path = test_dir_ / "index.dat";
  auto mem_path = test_dir_ / "lt_999.mem";

  // Write an index.dat file with the old format
  {
    std::ofstream ofs(idx_path);
    ofs << "128\n";  // DIM line
    ofs << "lt_999 0 0.85 user_migrate agent_m sess_m episodic\n";
  }

  // Write a .mem file
  {
    std::ofstream ofs(mem_path);
    ofs << "lt_999\n";          // id
    ofs << "migration_source\n"; // source
    ofs << "0.85\n";            // importance
    ofs << "user_migrate\n";    // user_id
    ofs << "agent_m\n";         // agent_id
    ofs << "sess_m\n";          // session_id
    ofs << "episodic\n";        // type
    ofs << "Hello\\nWorld\n";   // content with escaped newline
  }

  // Re-create the store, which should trigger migration
  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);

  // Verify migration happened
  EXPECT_GE(store_->size(), 1u);

  auto read = store_->read("lt_999");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->source, "migration_source");
  EXPECT_EQ(read->user_id, "user_migrate");
  EXPECT_EQ(read->agent_id, "agent_m");
  EXPECT_EQ(read->session_id, "sess_m");
  EXPECT_EQ(read->type, "episodic");
  // Content should have the newline unescaped
  EXPECT_EQ(read->content, "Hello\nWorld");

  // Old files should be cleaned up
  EXPECT_FALSE(fs::exists(idx_path));
  EXPECT_FALSE(fs::exists(mem_path));
}

// ── Migration with dash placeholders ──
TEST_F(SQLiteStoreTest, MigrateFromFilesWithDashPlaceholders) {
  store_.reset();

  auto idx_path = test_dir_ / "index.dat";
  auto mem_path = test_dir_ / "lt_500.mem";

  {
    std::ofstream ofs(idx_path);
    ofs << "128\n";
    ofs << "lt_500 0 0.5 - - - -\n"; // all dash placeholders
  }

  {
    std::ofstream ofs(mem_path);
    ofs << "lt_500\n";
    ofs << "src\n";
    ofs << "0.5\n";
    ofs << "-\n";
    ofs << "-\n";
    ofs << "-\n";
    ofs << "-\n";
    ofs << "dash content\n";
  }

  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);
  EXPECT_GE(store_->size(), 1u);

  auto read = store_->read("lt_500");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->content, "dash content");
}

// ── Migration skips entries with missing .mem files ──
TEST_F(SQLiteStoreTest, MigrateSkipsMissingMemFile) {
  store_.reset();

  auto idx_path = test_dir_ / "index.dat";

  {
    std::ofstream ofs(idx_path);
    ofs << "128\n";
    ofs << "lt_missing 0 0.5 user - - -\n"; // no corresponding .mem file
  }

  // Should not crash; migration should skip the entry
  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);
  EXPECT_EQ(store_->size(), 0u);

  // index.dat should still exist since no entries were migrated
  // (migrated == 0, so the cleanup block is skipped)
  EXPECT_TRUE(fs::exists(idx_path));
}

// ── Migration with bad importance in .mem file (exception path) ──
TEST_F(SQLiteStoreTest, MigrateWithBadImportanceString) {
  store_.reset();

  auto idx_path = test_dir_ / "index.dat";
  auto mem_path = test_dir_ / "lt_700.mem";

  {
    std::ofstream ofs(idx_path);
    ofs << "128\n";
    ofs << "lt_700 0 0.75 user_1 - - -\n";
  }

  {
    std::ofstream ofs(mem_path);
    ofs << "lt_700\n";
    ofs << "src\n";
    ofs << "not_a_number\n";  // bad importance -> catch path, fallback to 0.75
    ofs << "user_1\n";
    ofs << "-\n";
    ofs << "-\n";
    ofs << "-\n";
    ofs << "content here\n";
  }

  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);
  EXPECT_GE(store_->size(), 1u);

  auto read = store_->read("lt_700");
  ASSERT_TRUE(read);
  EXPECT_NEAR(read->importance, 0.75f, 0.01f);
}

TEST_F(SQLiteStoreTest, MigrateSkipsMalformedIndexRow) {
  store_.reset();

  auto idx_path = test_dir_ / "index.dat";
  auto mem_path = test_dir_ / "lt_bad.mem";

  {
    std::ofstream ofs(idx_path);
    ofs << "128\n";
    ofs << "lt_bad 0 0.75 only_user\n";  // malformed: missing fields
  }

  {
    std::ofstream ofs(mem_path);
    ofs << "lt_bad\n";
    ofs << "src\n";
    ofs << "0.75\n";
    ofs << "user_1\n";
    ofs << "agent_1\n";
    ofs << "sess_1\n";
    ofs << "episodic\n";
    ofs << "should not migrate\n";
  }

  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);

  EXPECT_EQ(store_->size(), 0u);
  auto read = store_->read("lt_bad");
  EXPECT_FALSE(read);
  EXPECT_TRUE(fs::exists(idx_path));
  EXPECT_TRUE(fs::exists(mem_path));
}

// ── Search: no HNSW index built, entries without embeddings ──
TEST_F(SQLiteStoreTest, SearchNoHNSWFallsBackToLinear) {
  // Write entries without embeddings -- HNSW never gets built
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "No-emb " + std::to_string(i);
    e.user_id = "user_1";
    e.importance = 0.5f + 0.1f * i;
    (void)store_->write(e);
  }

  // Use non-empty embedding for query, but HNSW has 0 elements
  Embedding query(64, 0.5f);
  normalize_emb(query);

  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  // Linear fallback returns results ordered by importance
  ASSERT_GE(results->size(), 1u);
}
