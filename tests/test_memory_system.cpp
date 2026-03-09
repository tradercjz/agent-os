#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::memory;

class MemorySystemTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "agentos_test_memory";
    std::filesystem::remove_all(test_dir_);
    mem_sys_ = std::make_unique<MemorySystem>(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
  std::unique_ptr<MemorySystem> mem_sys_;
};

TEST_F(MemorySystemTest, BasicHNSWInsertionAndSearch) {
  Embedding emb1(128, 0.1f);
  Embedding emb2(128, 0.9f);

  // Test write
  auto w1 =
      mem_sys_->add_episodic("Memory 1", emb1, "user_1", "session_1", 0.8f);
  ASSERT_TRUE(w1);

  auto w2 =
      mem_sys_->add_episodic("Memory 2", emb2, "user_1", "session_1", 0.9f);
  ASSERT_TRUE(w2);

  // Test search - Query close to emb2
  Embedding query(128, 0.85f);
  MemoryFilter filter;
  filter.user_id = "user_1";

  auto results = mem_sys_->recall(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1);

  // The closest should be Memory 2
  EXPECT_EQ(results->front().entry.content, "Memory 2");

  // Test exact matching retrieval
  auto results2 = mem_sys_->recall(emb1, filter, 5);
  ASSERT_TRUE(results2);
  ASSERT_GE(results2->size(), 1);
  // Find Memory 1
  bool found = false;
  for (auto &r : *results2) {
    if (r.entry.content == "Memory 1")
      found = true;
  }
  EXPECT_TRUE(found);
}

TEST_F(MemorySystemTest, ConsolidatePromotesImportantMemories) {
  Embedding emb(128, 0.5f);
  // Add a high-importance memory
  (void)mem_sys_->add_episodic("Important fact", emb, "user_1", "s1", 0.9f);
  // Add a low-importance memory
  (void)mem_sys_->add_episodic("Trivial fact", emb, "user_1", "s1", 0.3f);

  // Consolidate with threshold 0.6 — should promote 1
  size_t promoted = mem_sys_->consolidate(0.6f);
  EXPECT_GE(promoted, 1);
}

TEST_F(MemorySystemTest, RememberAndRecallById) {
  Embedding emb(128, 0.2f);
  auto id = mem_sys_->remember("Test content", emb, "agent_1", 0.7f);
  ASSERT_TRUE(id);
  EXPECT_FALSE(id->empty());
}

TEST_F(MemorySystemTest, EmptyEmbeddingGracefulHandling) {
  Embedding empty_emb;
  // adding text without embedding
  auto w1 =
      mem_sys_->add_episodic("Text only", empty_emb, "user_x", "session_x");
  ASSERT_TRUE(w1);

  auto res = mem_sys_->recall(empty_emb, {}, 5);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->size(),
            2); // 1 from WM + 1 from STM（importance=0.5 < 0.7 阈值，不写入 LTM）
  EXPECT_EQ(res->front().entry.content, "Text only");
}

// ── HNSW Compaction after heavy forget() ─────────────────────

TEST(ShortTermMemoryTest, HNSWCompactionAfterManyDeletes) {
  memory::ShortTermMemory stm(1000);

  // Write 20 entries with embeddings
  std::vector<std::string> ids;
  for (int i = 0; i < 20; ++i) {
    memory::MemoryEntry e;
    e.content = "mem_" + std::to_string(i);
    memory::Embedding emb(64, 0.1f);
    emb[i % 64] = 0.99f;
    // normalize
    float norm = 0;
    for (float v : emb) norm += v * v;
    norm = std::sqrt(norm);
    for (float &v : emb) v /= norm;
    e.embedding = emb;

    auto id = stm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }

  EXPECT_EQ(stm.size(), 20);

  // Delete 15 entries (>50% threshold triggers compaction)
  for (int i = 0; i < 15; ++i) {
    (void)stm.forget(ids[i]);
  }

  EXPECT_EQ(stm.size(), 5);

  // Remaining entries should still be searchable
  memory::MemoryFilter filter;
  memory::Embedding query(64, 0.1f);
  query[15 % 64] = 0.95f;
  float qnorm = 0;
  for (float v : query) qnorm += v * v;
  qnorm = std::sqrt(qnorm);
  for (float &v : query) v /= qnorm;

  auto results = stm.search(query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_GE(results->size(), 1);
}

// ── Const accessor tests ─────────────────────────────────────

TEST(MemorySystemConstTest, ConstAccessorsWork) {
  auto temp_dir = std::filesystem::temp_directory_path() / "agentos_const_test";
  std::filesystem::remove_all(temp_dir);

  const memory::MemorySystem mem(temp_dir);
  // These should compile with const ref
  EXPECT_EQ(mem.long_term().size(), 0);
  EXPECT_EQ(mem.long_term().name(), "LongTermMemory");

  std::filesystem::remove_all(temp_dir);
}
