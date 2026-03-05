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

TEST_F(MemorySystemTest, EmptyEmbeddingGracefulHandling) {
  Embedding empty_emb;
  // adding text without embedding
  auto w1 =
      mem_sys_->add_episodic("Text only", empty_emb, "user_x", "session_x");
  ASSERT_TRUE(w1);

  auto res = mem_sys_->recall(empty_emb, {}, 5);
  ASSERT_TRUE(res);
  ASSERT_EQ(res->size(),
            2); // 1 from STM, 1 from LTM since default importance = 0.8
  EXPECT_EQ(res->front().entry.content, "Text only");
}
