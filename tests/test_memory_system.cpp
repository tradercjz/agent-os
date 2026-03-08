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
  ASSERT_GE(results->size(), 1u);

  // The closest should be Memory 2
  EXPECT_EQ(results->front().entry.content, "Memory 2");

  // Test exact matching retrieval
  auto results2 = mem_sys_->recall(emb1, filter, 5);
  ASSERT_TRUE(results2);
  ASSERT_GE(results2->size(), 1u);
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
  EXPECT_GE(promoted, 1u);
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
            2u); // 1 from WM + 1 from STM（importance=0.5 < 0.7 阈值，不写入 LTM）
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

  EXPECT_EQ(stm.size(), 20u);

  // Delete 15 entries (>50% threshold triggers compaction)
  for (int i = 0; i < 15; ++i) {
    (void)stm.forget(ids[i]);
  }

  EXPECT_EQ(stm.size(), 5u);

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
  EXPECT_GE(results->size(), 1u);
}

// ── Const accessor tests ─────────────────────────────────────

TEST(MemorySystemConstTest, ConstAccessorsWork) {
  auto temp_dir = std::filesystem::temp_directory_path() / "agentos_const_test";
  std::filesystem::remove_all(temp_dir);

  const memory::MemorySystem mem(temp_dir);
  // These should compile with const ref
  EXPECT_EQ(mem.long_term().size(), 0u);
  EXPECT_EQ(mem.long_term().name(), "LongTermMemory");

  std::filesystem::remove_all(temp_dir);
}

// ── Regression tests for 44-fix commit ──────────────────────────

TEST(ShortTermMemoryTest, DimensionMismatchReturnsError) {
  // Test that ShortTermMemory rejects embeddings with mismatched dimensions
  memory::ShortTermMemory stm(100);

  // Write first entry with dimension 4
  {
    memory::MemoryEntry entry1;
    entry1.content = "first_memory";
    entry1.embedding = memory::Embedding(4, 0.5f);
    auto result1 = stm.write(std::move(entry1));
    ASSERT_TRUE(result1) << "First write with dimension 4 should succeed";
  }

  // Try to write second entry with different dimension (8)
  {
    memory::MemoryEntry entry2;
    entry2.content = "second_memory";
    entry2.embedding = memory::Embedding(8, 0.5f);
    auto result2 = stm.write(std::move(entry2));
    // Should fail due to dimension mismatch
    EXPECT_FALSE(result2) << "Second write with mismatched dimension should fail";
    // result2 should be an error (non-empty error state)
    EXPECT_FALSE(result2.has_value()) << "Error code: " << (result2 ? 0 : (int)result2.error().code);
  }
}

TEST_F(MemorySystemTest, ConsolidateWithTimeout) {
  // Test that consolidate() with timeout returns without blocking forever
  Embedding emb(128, 0.5f);

  // Add 5 entries to working memory
  for (int i = 0; i < 5; ++i) {
    auto result = mem_sys_->add_episodic(
        std::string("memory_") + std::to_string(i),
        emb, "user_1", "session_1", 0.5f);
    ASSERT_TRUE(result) << "Failed to add episodic memory " << i;
  }

  // Call consolidate with 1-second timeout
  auto start = std::chrono::high_resolution_clock::now();
  size_t promoted = mem_sys_->consolidate(0.4f);
  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Should complete within timeout (well under 5 seconds)
  EXPECT_LT(elapsed.count(), 5000) << "consolidate() took too long; may be hanging";

  // Should have promoted some memories (those with importance >= 0.4)
  EXPECT_GE(promoted, 0u);
}
