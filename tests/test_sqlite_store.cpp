#include "agentos/memory/sqlite_store.hpp"
#include <filesystem>
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
  EXPECT_EQ(store_->size(), 1);

  auto result = store_->forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);

  EXPECT_EQ(store_->size(), 0);

  // Read after forget should fail
  auto read = store_->read(*id);
  EXPECT_FALSE(read);
}

TEST_F(SQLiteStoreTest, SizeReflectsEntries) {
  EXPECT_EQ(store_->size(), 0);

  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "Memory #" + std::to_string(i);
    store_->write(e);
  }

  EXPECT_EQ(store_->size(), 5);
}

TEST_F(SQLiteStoreTest, GetAllReturnsAllEntries) {
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "Item " + std::to_string(i);
    e.user_id = "user_x";
    store_->write(e);
  }

  auto all = store_->get_all();
  EXPECT_EQ(all.size(), 3);
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
  ASSERT_EQ(read->embedding.size(), 128);

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

  store_->write(e1);
  store_->write(e2);

  // Query close to emb1
  Embedding query(128, 0.1f);
  query[0] = 0.95f;
  normalize(query);

  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = store_->search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1);
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

  store_->write(e1);
  store_->write(e2);

  // Filter for user_a only
  MemoryFilter filter;
  filter.user_id = "user_a";

  auto results = store_->search(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_EQ(results->size(), 1);
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
    store_->write(e1);

    MemoryEntry e2;
    e2.content = "Persistent memory 2";
    e2.user_id = "user_1";
    e2.importance = 0.6f;
    store_->write(e2);
  }

  EXPECT_EQ(store_->size(), 2);

  // 销毁并重新创建（模拟重启）
  store_.reset();
  store_ = std::make_unique<SQLiteLongTermMemory>(test_dir_);

  // 数据应该仍然存在
  EXPECT_EQ(store_->size(), 2);

  auto all = store_->get_all();
  EXPECT_EQ(all.size(), 2);

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
  ASSERT_GE(results->size(), 1);
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
  EXPECT_GE(mem.long_term().size(), 1);
  EXPECT_EQ(mem.long_term().name(), "SQLiteLongTermMemory");

  // 召回应该有结果
  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = mem.recall(emb, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1);

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

    mem.remember("SQLite enum test data", emb, "agent", 0.9f);
    EXPECT_GE(mem.long_term().size(), 1);
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
