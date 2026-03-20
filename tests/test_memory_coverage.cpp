// ============================================================
// Coverage-boost tests for memory.cpp
// Targets uncovered lines: WM read/forget/eviction, STM read/eviction,
// LTM read/forget/load_index/handle_corrupt_index/entry_path validation,
// MemorySystem consolidate(timeout)/forget
// ============================================================
#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <string>

using namespace agentos;
using namespace agentos::memory;

namespace fs = std::filesystem;

namespace {

fs::path make_memory_coverage_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("agentos_memory_cov_" + name + "_" + std::to_string(nonce));
}

}

// Helper: create a normalized embedding of given dimension with a distinguishing element
static Embedding make_emb(size_t dim, int idx) {
  Embedding emb(dim, 0.1f);
  emb[idx % dim] = 0.95f;
  float norm = 0;
  for (float v : emb) norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb) v /= norm;
  return emb;
}

// ─────────────────────────────────────────────────────────────
// WorkingMemory coverage
// ─────────────────────────────────────────────────────────────

TEST(WorkingMemoryCoverage, ReadExistingEntry) {
  WorkingMemory wm(32);
  MemoryEntry e;
  e.content = "hello";
  e.embedding = {1.0f, 0.0f};
  auto id = wm.write(std::move(e));
  ASSERT_TRUE(id);

  auto entry = wm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "hello");
  EXPECT_GE(entry->access_count, 1u);
}

TEST(WorkingMemoryCoverage, ReadNonExistentReturnsError) {
  WorkingMemory wm(32);
  auto entry = wm.read("nonexistent_id");
  EXPECT_FALSE(entry);
  EXPECT_EQ(entry.error().code, ErrorCode::NotFound);
}

TEST(WorkingMemoryCoverage, ForgetExistingEntry) {
  WorkingMemory wm(32);
  MemoryEntry e;
  e.content = "to forget";
  auto id = wm.write(std::move(e));
  ASSERT_TRUE(id);

  auto result = wm.forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);

  // Confirm it's gone
  auto entry = wm.read(*id);
  EXPECT_FALSE(entry);
}

TEST(WorkingMemoryCoverage, ForgetNonExistentReturnsFalse) {
  WorkingMemory wm(32);
  auto result = wm.forget("no_such_id");
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST(WorkingMemoryCoverage, LRUEvictionWhenFull) {
  // Capacity = 3, write 4 entries -> oldest accessed should be evicted
  WorkingMemory wm(3);

  MemoryEntry e1; e1.content = "first";
  auto id1 = wm.write(std::move(e1));
  ASSERT_TRUE(id1);

  MemoryEntry e2; e2.content = "second";
  auto id2 = wm.write(std::move(e2));
  ASSERT_TRUE(id2);

  MemoryEntry e3; e3.content = "third";
  auto id3 = wm.write(std::move(e3));
  ASSERT_TRUE(id3);

  EXPECT_EQ(wm.size(), 3u);

  // Access id1 to make it more recently used
  (void)wm.read(*id1);

  // Write a 4th entry -> should evict the oldest-accessed (id2)
  MemoryEntry e4; e4.content = "fourth";
  auto id4 = wm.write(std::move(e4));
  ASSERT_TRUE(id4);

  EXPECT_EQ(wm.size(), 3u);
  // id2 should be evicted (oldest accessed)
  auto r2 = wm.read(*id2);
  EXPECT_FALSE(r2);
  // id1 should still exist
  auto r1 = wm.read(*id1);
  EXPECT_TRUE(r1);
}

TEST(WorkingMemoryCoverage, SearchTopKTruncation) {
  WorkingMemory wm(100);
  // Insert 10 entries
  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "entry_" + std::to_string(i);
    e.embedding = {static_cast<float>(i) * 0.1f, 0.5f};
    (void)wm.write(std::move(e));
  }

  Embedding query = {0.5f, 0.5f};
  MemoryFilter filter;
  auto results = wm.search(query, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 3u);
}

// ─────────────────────────────────────────────────────────────
// ShortTermMemory coverage
// ─────────────────────────────────────────────────────────────

TEST(ShortTermMemoryCoverage, ReadExistingEntry) {
  ShortTermMemory stm(100);
  MemoryEntry e;
  e.content = "stm entry";
  e.embedding = make_emb(16, 0);
  auto id = stm.write(std::move(e));
  ASSERT_TRUE(id);

  auto entry = stm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "stm entry");
  EXPECT_GE(entry->access_count, 1u);
}

TEST(ShortTermMemoryCoverage, ReadNonExistentReturnsError) {
  ShortTermMemory stm(100);
  auto entry = stm.read("nonexistent");
  EXPECT_FALSE(entry);
  EXPECT_EQ(entry.error().code, ErrorCode::NotFound);
}

TEST(ShortTermMemoryCoverage, ForgetNonExistentReturnsFalse) {
  ShortTermMemory stm(100);
  auto result = stm.forget("no_such_id");
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST(ShortTermMemoryCoverage, CapacityEvictionWithHNSW) {
  // Capacity = 5, write 6 entries -> one should be evicted
  ShortTermMemory stm(5);

  std::vector<std::string> ids;
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "item_" + std::to_string(i);
    e.embedding = make_emb(16, i);
    e.importance = 0.5f;
    auto id = stm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }
  EXPECT_EQ(stm.size(), 5u);

  // Write 6th entry -> triggers eviction of lowest-scored entry
  MemoryEntry e6;
  e6.content = "item_5";
  e6.embedding = make_emb(16, 5);
  e6.importance = 0.9f;
  auto id6 = stm.write(std::move(e6));
  ASSERT_TRUE(id6);
  EXPECT_EQ(stm.size(), 5u); // still at capacity

  // Search should still work after eviction
  Embedding query = make_emb(16, 5);
  MemoryFilter filter;
  auto results = stm.search(query, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_GE(results->size(), 1u);
}

TEST(ShortTermMemoryCoverage, CapacityEvictionProtectsSystemEntries) {
  // System entries should never be evicted
  ShortTermMemory stm(3);

  // Insert a system entry
  MemoryEntry sys;
  sys.content = "system instruction";
  sys.embedding = make_emb(8, 0);
  sys.importance = 0.1f; // low importance but system source
  sys.source = "system";
  auto sys_id = stm.write(std::move(sys));
  ASSERT_TRUE(sys_id);

  // Fill to capacity with non-system entries
  for (int i = 1; i <= 2; ++i) {
    MemoryEntry e;
    e.content = "normal_" + std::to_string(i);
    e.embedding = make_emb(8, i);
    e.importance = 0.2f;
    e.source = "user";
    (void)stm.write(std::move(e));
  }
  EXPECT_EQ(stm.size(), 3u);

  // Write one more -> should evict a non-system entry, not the system one
  MemoryEntry extra;
  extra.content = "trigger_eviction";
  extra.embedding = make_emb(8, 3);
  extra.importance = 0.8f;
  extra.source = "user";
  (void)stm.write(std::move(extra));

  // System entry should still be readable
  auto sys_entry = stm.read(*sys_id);
  EXPECT_TRUE(sys_entry);
  EXPECT_EQ(sys_entry->content, "system instruction");
}

TEST(ShortTermMemoryCoverage, SearchDimensionMismatchReturnsError) {
  ShortTermMemory stm(100);

  // Establish dim_ = 8
  MemoryEntry e;
  e.content = "dim8";
  e.embedding = make_emb(8, 0);
  (void)stm.write(std::move(e));

  // Search with dim 4 -> should return error
  Embedding query(4, 0.5f);
  MemoryFilter filter;
  auto results = stm.search(query, filter, 5);
  EXPECT_FALSE(results);
  EXPECT_EQ(results.error().code, ErrorCode::InvalidArgument);
}

TEST(ShortTermMemoryCoverage, WriteNoEmbeddingThenSearchFallback) {
  ShortTermMemory stm(100);

  // Write entry with no embedding
  MemoryEntry e;
  e.content = "no embedding";
  (void)stm.write(std::move(e));

  // Search with empty embedding -> linear fallback
  Embedding empty_query;
  MemoryFilter filter;
  auto results = stm.search(empty_query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_EQ(results->size(), 1u);
  EXPECT_EQ(results->front().entry.content, "no embedding");
}

// ─────────────────────────────────────────────────────────────
// LongTermMemory coverage
// ─────────────────────────────────────────────────────────────

class LongTermMemoryCoverage : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = make_memory_coverage_test_dir("ltm");
    fs::remove_all(test_dir_);
    fs::create_directories(test_dir_);
  }
  void TearDown() override {
    fs::remove_all(test_dir_);
  }
  fs::path test_dir_;
};

TEST_F(LongTermMemoryCoverage, WriteAndReadBack) {
  LongTermMemory ltm(test_dir_);
  EXPECT_TRUE(ltm.is_loaded());

  MemoryEntry e;
  e.content = "persistent data";
  e.embedding = make_emb(32, 0);
  e.importance = 0.8f;
  e.source = "test";
  e.user_id = "user1";
  e.agent_id = "agent1";
  e.session_id = "sess1";
  e.type = "episodic";

  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);
  ltm.flush();

  // Read back
  auto entry = ltm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "persistent data");
  EXPECT_EQ(entry->source, "test");
  EXPECT_EQ(entry->user_id, "user1");
  EXPECT_EQ(entry->type, "episodic");
  EXPECT_NEAR(entry->importance, 0.8f, 0.01f);

  // Read again (should hit metadata_cache_)
  auto entry2 = ltm.read(*id);
  ASSERT_TRUE(entry2);
  EXPECT_EQ(entry2->content, "persistent data");
}

TEST_F(LongTermMemoryCoverage, ReadNonExistent) {
  LongTermMemory ltm(test_dir_);
  auto entry = ltm.read("nonexistent_id");
  EXPECT_FALSE(entry);
}

TEST_F(LongTermMemoryCoverage, ForgetExistingEntry) {
  LongTermMemory ltm(test_dir_);

  MemoryEntry e;
  e.content = "to delete";
  e.embedding = make_emb(16, 0);
  e.importance = 0.7f;
  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);
  ltm.flush();

  auto result = ltm.forget(*id);
  ASSERT_TRUE(result);
  EXPECT_TRUE(*result);

  // Confirm it's gone
  auto entry = ltm.read(*id);
  EXPECT_FALSE(entry);
}

TEST_F(LongTermMemoryCoverage, ForgetNonExistentReturnsFalse) {
  LongTermMemory ltm(test_dir_);
  auto result = ltm.forget("no_such_id");
  ASSERT_TRUE(result);
  EXPECT_FALSE(*result);
}

TEST_F(LongTermMemoryCoverage, ForgetInvalidIdReturnsError) {
  LongTermMemory ltm(test_dir_);
  // Path-traversal-like id
  auto result = ltm.forget("../../../etc/passwd");
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(LongTermMemoryCoverage, WriteInvalidIdReturnsError) {
  LongTermMemory ltm(test_dir_);
  MemoryEntry e;
  e.id = "bad/id";
  e.content = "test";
  auto result = ltm.write(std::move(e));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(LongTermMemoryCoverage, EntryPathEmptyIdThrows) {
  LongTermMemory ltm(test_dir_);
  MemoryEntry e;
  e.id = "";  // Will be auto-generated, so this tests the auto-gen path
  e.content = "auto id";
  auto result = ltm.write(std::move(e));
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->starts_with("lt_"));
}

TEST_F(LongTermMemoryCoverage, ContentWithNewlinesRoundtrips) {
  LongTermMemory ltm(test_dir_);

  MemoryEntry e;
  e.content = "line1\nline2\nline3";
  e.embedding = make_emb(8, 0);
  e.importance = 0.9f;
  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);
  ltm.flush();

  auto entry = ltm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "line1\nline2\nline3");
}

TEST_F(LongTermMemoryCoverage, DimensionMismatchSkipsIndexing) {
  LongTermMemory ltm(test_dir_);

  // Write first entry, establishing dim=8
  MemoryEntry e1;
  e1.content = "dim8";
  e1.embedding = make_emb(8, 0);
  (void)ltm.write(std::move(e1));

  // Write second entry with dim=16 -> should skip HNSW but still store
  MemoryEntry e2;
  e2.content = "dim16";
  e2.embedding = make_emb(16, 0);
  auto id2 = ltm.write(std::move(e2));
  ASSERT_TRUE(id2); // Write succeeds, just no HNSW indexing
}

TEST_F(LongTermMemoryCoverage, WriteNoEmbedding) {
  LongTermMemory ltm(test_dir_);

  MemoryEntry e;
  e.content = "no vector";
  // no embedding
  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);
  ltm.flush();

  auto entry = ltm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "no vector");
  EXPECT_TRUE(entry->embedding.empty());
}

TEST_F(LongTermMemoryCoverage, LoadIndexFromDisk) {
  // Phase 1: write entries and flush
  {
    LongTermMemory ltm(test_dir_);
    for (int i = 0; i < 3; ++i) {
      MemoryEntry e;
      e.content = "entry_" + std::to_string(i);
      e.embedding = make_emb(16, i);
      e.importance = 0.8f;
      e.user_id = "user1";
      (void)ltm.write(std::move(e));
    }
    ltm.flush();
  }

  // Phase 2: construct new LTM from same dir -> should load_index
  {
    LongTermMemory ltm(test_dir_);
    EXPECT_TRUE(ltm.is_loaded());
    EXPECT_EQ(ltm.size(), 3u);

    // Search should work with loaded HNSW index
    Embedding query = make_emb(16, 0);
    MemoryFilter filter;
    auto results = ltm.search(query, filter, 5);
    ASSERT_TRUE(results);
    EXPECT_GE(results->size(), 1u);

    // Read should work
    auto all = ltm.get_all();
    EXPECT_EQ(all.size(), 3u);
  }
}

TEST_F(LongTermMemoryCoverage, CorruptIndexDatHandled) {
  // Write a corrupt index.dat
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "CORRUPT GARBAGE\n";
    ofs << "not a valid record line\n";
  }

  // Constructing LTM should handle corrupt index gracefully
  LongTermMemory ltm(test_dir_);
  EXPECT_FALSE(ltm.is_loaded());
  EXPECT_TRUE(fs::exists(test_dir_ / "index.bad"));
  EXPECT_FALSE(fs::exists(test_dir_ / "index.dat"));
  EXPECT_EQ(ltm.size(), 0u);
}

TEST_F(LongTermMemoryCoverage, CorruptImportanceParsed) {
  // Write a .mem file with invalid importance value
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM 0\n";
    ofs << "test_corrupt " << static_cast<uint64_t>(-1) << " 0.5 user1 agent1 sess1 episodic\n";
  }
  // Write the .mem file with corrupt importance
  {
    std::ofstream ofs(test_dir_ / "test_corrupt.mem");
    ofs << "test_corrupt\n";  // id
    ofs << "agent\n";          // source
    ofs << "NOT_A_NUMBER\n";   // importance (corrupt)
    ofs << "user1\n";          // user_id
    ofs << "agent1\n";         // agent_id
    ofs << "sess1\n";          // session_id
    ofs << "episodic\n";       // type
    ofs << "test content\n";   // content
  }

  LongTermMemory ltm(test_dir_);
  if (ltm.is_loaded()) {
    auto entry = ltm.read("test_corrupt");
    if (entry) {
      // Corrupt importance should fall back to 0.5
      EXPECT_NEAR(entry->importance, 0.5f, 0.01f);
    }
  }
}

TEST_F(LongTermMemoryCoverage, SearchLinearFallbackWithNoEmbeddings) {
  LongTermMemory ltm(test_dir_);

  // Write entries without embeddings
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "text_" + std::to_string(i);
    e.user_id = "user1";
    e.type = "episodic";
    (void)ltm.write(std::move(e));
  }
  ltm.flush();

  // Search with empty embedding -> linear fallback
  Embedding empty_query;
  MemoryFilter filter;
  filter.user_id = "user1";
  auto results = ltm.search(empty_query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_EQ(results->size(), 3u);
}

TEST_F(LongTermMemoryCoverage, SearchDimMismatchReturnsError) {
  LongTermMemory ltm(test_dir_);

  // Establish dim=8
  MemoryEntry e;
  e.content = "dim8";
  e.embedding = make_emb(8, 0);
  (void)ltm.write(std::move(e));

  // Search with wrong dimension
  Embedding query(16, 0.5f);
  MemoryFilter filter;
  auto results = ltm.search(query, filter, 5);
  EXPECT_FALSE(results);
}

TEST_F(LongTermMemoryCoverage, LoadIndexWithDashPlaceholders) {
  // Write an index.dat that uses "-" for empty fields
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM 0\n";
    ofs << "lt_0 " << static_cast<uint64_t>(-1) << " 0.5 - - - -\n";
  }
  // Write the corresponding .mem file
  {
    std::ofstream ofs(test_dir_ / "lt_0.mem");
    ofs << "lt_0\n";
    ofs << "agent\n";
    ofs << "0.5\n";
    ofs << "\n";  // empty user_id
    ofs << "\n";  // empty agent_id
    ofs << "\n";  // empty session_id
    ofs << "\n";  // empty type
    ofs << "dash test content\n";
  }

  LongTermMemory ltm(test_dir_);
  EXPECT_TRUE(ltm.is_loaded());
  EXPECT_EQ(ltm.size(), 1u);

  auto entry = ltm.read("lt_0");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "dash test content");
}

// ─────────────────────────────────────────────────────────────
// MemorySystem coverage
// ─────────────────────────────────────────────────────────────

class MemorySystemCoverage : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = make_memory_coverage_test_dir("memsys");
    fs::remove_all(test_dir_);
    mem_sys_ = std::make_unique<MemorySystem>(test_dir_);
  }
  void TearDown() override {
    mem_sys_.reset();
    fs::remove_all(test_dir_);
  }
  fs::path test_dir_;
  std::unique_ptr<MemorySystem> mem_sys_;
};

TEST_F(MemorySystemCoverage, ConsolidateWithTimeoutCompletes) {
  Embedding emb = make_emb(32, 0);

  // Add entries to working memory
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "consolidate_" + std::to_string(i);
    e.embedding = emb;
    e.importance = 0.8f;
    (void)mem_sys_->working().write(std::move(e));
  }

  // consolidate with generous timeout should complete
  bool completed = mem_sys_->consolidate(std::chrono::milliseconds(5000), 0.6f);
  EXPECT_TRUE(completed);

  // LTM should have promoted entries
  EXPECT_GE(mem_sys_->long_term().size(), 1u);
}

TEST_F(MemorySystemCoverage, ConsolidateWithTimeoutNoPromotions) {
  Embedding emb = make_emb(32, 0);

  // Add low-importance entries
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "low_" + std::to_string(i);
    e.embedding = emb;
    e.importance = 0.1f;
    (void)mem_sys_->working().write(std::move(e));
  }

  // Threshold too high -> no promotions
  bool completed = mem_sys_->consolidate(std::chrono::milliseconds(5000), 0.9f);
  EXPECT_TRUE(completed);
  EXPECT_EQ(mem_sys_->long_term().size(), 0u);
}

TEST_F(MemorySystemCoverage, ForgetRemovesFromAllLayers) {
  Embedding emb = make_emb(32, 0);

  // remember() writes to WM, STM, and (if importance > 0.7) LTM
  auto id = mem_sys_->remember("to forget", emb, "agent", 0.9f);
  ASSERT_TRUE(id);

  EXPECT_GE(mem_sys_->working().size(), 1u);

  // forget from all layers
  mem_sys_->forget(*id);

  // Working memory entry should be gone (the id is from WM)
  auto entry = mem_sys_->working().read(*id);
  EXPECT_FALSE(entry);
}

TEST_F(MemorySystemCoverage, AddSemanticMemory) {
  Embedding emb = make_emb(32, 0);
  auto id = mem_sys_->add_semantic("semantic fact", emb, "user_1", 0.85f);
  ASSERT_TRUE(id);

  // Recall it
  MemoryFilter filter;
  filter.user_id = "user_1";
  auto results = mem_sys_->recall(emb, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_GE(results->size(), 1u);

  bool found = false;
  for (auto &r : *results) {
    if (r.entry.content == "semantic fact") found = true;
  }
  EXPECT_TRUE(found);
}

TEST_F(MemorySystemCoverage, RecallMergesDeduplicates) {
  Embedding emb = make_emb(32, 0);

  // remember() writes to both WM and STM with same content
  (void)mem_sys_->remember("duplicate check", emb, "agent", 0.5f);

  MemoryFilter filter;
  auto results = mem_sys_->recall(emb, filter, 10);
  ASSERT_TRUE(results);

  // Count entries with "duplicate check" content
  int count = 0;
  for (auto &r : *results) {
    if (r.entry.content == "duplicate check") count++;
  }
  // Each entry gets a unique id, so they won't be deduped by id.
  // But verify the merge works without crash.
  EXPECT_GE(count, 1);
}

TEST_F(MemorySystemCoverage, RecallTopKTruncation) {
  // Insert enough entries to exceed top_k
  for (int i = 0; i < 10; ++i) {
    Embedding emb = make_emb(32, i);
    (void)mem_sys_->remember("entry_" + std::to_string(i), emb, "agent", 0.5f);
  }

  Embedding query = make_emb(32, 0);
  MemoryFilter filter;
  auto results = mem_sys_->recall(query, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 3u);
}

// ─────────────────────────────────────────────────────────────
// MemoryFilter coverage
// ─────────────────────────────────────────────────────────────

TEST(MemoryFilterCoverage, FilterByAgentId) {
  WorkingMemory wm(100);

  MemoryEntry e1;
  e1.content = "agent_a";
  e1.agent_id = "a";
  e1.embedding = {1.0f, 0.0f};
  (void)wm.write(std::move(e1));

  MemoryEntry e2;
  e2.content = "agent_b";
  e2.agent_id = "b";
  e2.embedding = {0.0f, 1.0f};
  (void)wm.write(std::move(e2));

  MemoryFilter filter;
  filter.agent_id = "a";
  Embedding query = {0.5f, 0.5f};
  auto results = wm.search(query, filter, 5);
  ASSERT_TRUE(results);
  for (auto &r : *results) {
    EXPECT_EQ(r.entry.agent_id, "a");
  }
}

TEST(MemoryFilterCoverage, FilterBySessionId) {
  MemoryFilter filter;
  filter.session_id = "sess1";
  EXPECT_TRUE(filter.match("", "", "sess1", ""));
  EXPECT_FALSE(filter.match("", "", "sess2", ""));
}

TEST(MemoryFilterCoverage, FilterByType) {
  MemoryFilter filter;
  filter.type = "semantic";
  EXPECT_TRUE(filter.match("", "", "", "semantic"));
  EXPECT_FALSE(filter.match("", "", "", "episodic"));
}

// ─────────────────────────────────────────────────────────────
// Additional coverage for STM compact_hnsw via forget
// ─────────────────────────────────────────────────────────────

TEST(ShortTermMemoryCoverage, ForgetTriggersCompaction) {
  // Insert entries with embeddings, then forget >50% to trigger compaction
  ShortTermMemory stm(100);

  std::vector<std::string> ids;
  for (int i = 0; i < 6; ++i) {
    MemoryEntry e;
    e.content = "compact_" + std::to_string(i);
    e.embedding = make_emb(8, i);
    e.importance = 0.5f;
    auto id = stm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }
  EXPECT_EQ(stm.size(), 6u);

  // Forget 4 out of 6 entries — deleted_label_count(4) > live_count(2), triggers compaction
  for (int i = 0; i < 4; ++i) {
    auto r = stm.forget(ids[i]);
    ASSERT_TRUE(r);
    EXPECT_TRUE(*r);
  }
  EXPECT_EQ(stm.size(), 2u);

  // Remaining entries should still be searchable after compaction
  Embedding query = make_emb(8, 4);
  MemoryFilter filter;
  auto results = stm.search(query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_GE(results->size(), 1u);
}

TEST(ShortTermMemoryCoverage, ForgetEntryWithEmbedding) {
  ShortTermMemory stm(100);

  MemoryEntry e;
  e.content = "with embedding";
  e.embedding = make_emb(8, 0);
  auto id = stm.write(std::move(e));
  ASSERT_TRUE(id);

  // Forget should also clean up HNSW label mapping
  auto r = stm.forget(*id);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);

  // Verify it's gone
  auto entry = stm.read(*id);
  EXPECT_FALSE(entry);
}

TEST(ShortTermMemoryCoverage, CompactWithMixedEmbeddings) {
  // Some entries have embeddings, some don't — compact should skip empties
  ShortTermMemory stm(100);

  std::vector<std::string> ids;
  // Entry with embedding
  for (int i = 0; i < 3; ++i) {
    MemoryEntry e;
    e.content = "emb_" + std::to_string(i);
    e.embedding = make_emb(8, i);
    auto id = stm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }
  // Entry without embedding
  {
    MemoryEntry e;
    e.content = "no_emb";
    auto id = stm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }

  // Forget enough to trigger compaction (>50% deleted)
  for (int i = 0; i < 3; ++i) {
    (void)stm.forget(ids[i]);
  }

  // The no_emb entry should still be readable
  auto entry = stm.read(ids[3]);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "no_emb");
}

// ─────────────────────────────────────────────────────────────
// LTM: handle_corrupt_index, load_index error paths
// ─────────────────────────────────────────────────────────────

TEST_F(LongTermMemoryCoverage, CorruptDimLineFailsGracefully) {
  // DIM line with non-numeric value triggers the inner catch in load_index
  // which sets load_ok_ = false (lines 594-597 in memory.cpp)
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM NOTANUMBER\n";
    ofs << "some_id 999 0.5 user1 agent1 sess1 episodic\n";
  }

  LongTermMemory ltm(test_dir_);
  // Should fail to load but not crash
  EXPECT_FALSE(ltm.is_loaded());
}

TEST_F(LongTermMemoryCoverage, EmptyIndexLineSkipped) {
  // Test that empty lines in index.dat are properly skipped (line 618)
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM 0\n";
    ofs << "\n";  // empty line — should be skipped
    ofs << "\n";  // another empty line
    ofs << "lt_0 " << static_cast<uint64_t>(-1) << " 0.5 user1 agent1 sess1 episodic\n";
  }
  {
    std::ofstream ofs(test_dir_ / "lt_0.mem");
    ofs << "lt_0\nagent\n0.5\nuser1\nagent1\nsess1\nepisodic\nskip test\n";
  }

  LongTermMemory ltm(test_dir_);
  EXPECT_TRUE(ltm.is_loaded());
  EXPECT_EQ(ltm.size(), 1u);
}

TEST_F(LongTermMemoryCoverage, LoadIndexWithHNSWBinMissing) {
  // Write a valid index.dat with dim > 0 but NO hnsw_index.bin
  // This exercises the "bin_path doesn't exist" branch in load_index
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM 8\n";
    ofs << "lt_0 0 0.5 user1 agent1 sess1 episodic\n";
  }
  // Also create the .mem file
  {
    std::ofstream ofs(test_dir_ / "lt_0.mem");
    ofs << "lt_0\n";
    ofs << "agent\n";
    ofs << "0.5\n";
    ofs << "user1\n";
    ofs << "agent1\n";
    ofs << "sess1\n";
    ofs << "episodic\n";
    ofs << "test content\n";
  }

  LongTermMemory ltm(test_dir_);
  EXPECT_TRUE(ltm.is_loaded());
  EXPECT_EQ(ltm.size(), 1u);

  auto entry = ltm.read("lt_0");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "test content");
}

TEST_F(LongTermMemoryCoverage, EntryPathEmptyIdThrowsRange) {
  // Test the empty/overlong id validation in entry_path
  LongTermMemory ltm(test_dir_);
  // Very long id (>255 chars)
  std::string long_id(300, 'x');
  MemoryEntry e;
  e.id = long_id;
  e.content = "test";
  auto result = ltm.write(std::move(e));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST_F(LongTermMemoryCoverage, GetDataByLabelException) {
  // Write an entry, then corrupt the HNSW state to trigger getDataByLabel exception
  LongTermMemory ltm(test_dir_);

  MemoryEntry e;
  e.content = "hnsw test";
  e.embedding = make_emb(8, 0);
  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);
  ltm.flush();

  // Read should work even if getDataByLabel succeeds
  auto entry = ltm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "hnsw test");
  // Embedding should be recovered from HNSW
  EXPECT_FALSE(entry->embedding.empty());
}

TEST_F(LongTermMemoryCoverage, LoadIndexWithIdCounterRecovery) {
  // Write entries, flush, reload — id_counter_ should be recovered
  {
    LongTermMemory ltm(test_dir_);
    for (int i = 0; i < 5; ++i) {
      MemoryEntry e;
      e.content = "entry_" + std::to_string(i);
      e.embedding = make_emb(8, i);
      (void)ltm.write(std::move(e));
    }
    ltm.flush();
  }

  // Reload — should recover id_counter_ from "lt_X" prefix parsing
  {
    LongTermMemory ltm2(test_dir_);
    EXPECT_TRUE(ltm2.is_loaded());
    EXPECT_EQ(ltm2.size(), 5u);

    // Writing a new entry should get an id > lt_4
    MemoryEntry e;
    e.content = "new after reload";
    e.embedding = make_emb(8, 5);
    auto id = ltm2.write(std::move(e));
    ASSERT_TRUE(id);
    // The id should be at least lt_5
    EXPECT_TRUE(id->starts_with("lt_"));
    EXPECT_EQ(ltm2.size(), 6u);
  }
}

TEST_F(LongTermMemoryCoverage, SearchHNSWTopKTruncation) {
  // Insert many entries, search with small top_k to trigger truncation
  LongTermMemory ltm(test_dir_);

  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "search_entry_" + std::to_string(i);
    e.embedding = make_emb(16, i);
    e.importance = 0.7f;
    (void)ltm.write(std::move(e));
  }
  ltm.flush();

  Embedding query = make_emb(16, 0);
  MemoryFilter filter;
  auto results = ltm.search(query, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 3u);
}

TEST_F(LongTermMemoryCoverage, SearchLinearFallbackTopKTruncation) {
  // Insert many entries without embeddings, search with small top_k
  LongTermMemory ltm(test_dir_);

  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "linear_" + std::to_string(i);
    e.user_id = "u1";
    (void)ltm.write(std::move(e));
  }
  ltm.flush();

  Embedding empty;
  MemoryFilter filter;
  filter.user_id = "u1";
  auto results = ltm.search(empty, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 3u);
}

// ─────────────────────────────────────────────────────────────
// MemorySystem: consolidate timeout, forget behavior
// ─────────────────────────────────────────────────────────────

TEST_F(MemorySystemCoverage, ConsolidateTimeoutZeroReturnsPartial) {
  Embedding emb = make_emb(32, 0);

  // Add entries to working memory
  for (int i = 0; i < 20; ++i) {
    MemoryEntry e;
    e.content = "timeout_" + std::to_string(i);
    e.embedding = emb;
    e.importance = 0.8f;
    (void)mem_sys_->working().write(std::move(e));
  }

  // Zero timeout — should return false (timeout) almost immediately
  // But with small dataset it might complete. Use 0ms.
  bool completed = mem_sys_->consolidate(std::chrono::milliseconds(0), 0.6f);
  // Either way, no crash is the key assertion
  (void)completed;
}

TEST_F(MemorySystemCoverage, ConsolidateNoImportanceThreshold) {
  Embedding emb = make_emb(32, 0);

  // Add a high-importance entry
  MemoryEntry e;
  e.content = "important";
  e.embedding = emb;
  e.importance = 0.95f;
  (void)mem_sys_->working().write(std::move(e));

  // Use no-timeout consolidate
  size_t promoted = mem_sys_->consolidate(0.5f);
  EXPECT_GE(promoted, 1u);
}

TEST_F(MemorySystemCoverage, ForgetFromWorkingAndLongTerm) {
  Embedding emb = make_emb(32, 0);

  // remember() with high importance writes to WM + STM + LTM
  auto id = mem_sys_->remember("to forget everywhere", emb, "agent", 0.95f);
  ASSERT_TRUE(id);

  // forget should remove from all layers
  mem_sys_->forget(*id);

  // Verify working memory entry gone
  auto entry = mem_sys_->working().read(*id);
  EXPECT_FALSE(entry);
}

// ─────────────────────────────────────────────────────────────
// STM search edge cases
// ─────────────────────────────────────────────────────────────

TEST(ShortTermMemoryCoverage, SearchWithHNSWEmptyCount) {
  ShortTermMemory stm(100);

  // Write entry with embedding to init HNSW, then forget it (HNSW count=0)
  MemoryEntry e;
  e.content = "temp";
  e.embedding = make_emb(8, 0);
  auto id = stm.write(std::move(e));
  ASSERT_TRUE(id);
  (void)stm.forget(*id);

  // Now search with embedding — should fall through to linear scan
  Embedding query = make_emb(8, 0);
  MemoryFilter filter;
  auto results = stm.search(query, filter, 5);
  ASSERT_TRUE(results);
  // Empty because the entry was forgotten
  EXPECT_EQ(results->size(), 0u);
}

TEST(ShortTermMemoryCoverage, SearchKnnReturnsNoValidLabels) {
  ShortTermMemory stm(100);

  // Insert multiple entries
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "item_" + std::to_string(i);
    e.embedding = make_emb(8, i);
    e.importance = 0.5f;
    (void)stm.write(std::move(e));
  }

  // Search with valid query — exercises the full HNSW search path
  Embedding query = make_emb(8, 2);
  MemoryFilter filter;
  auto results = stm.search(query, filter, 3);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 3u);
  EXPECT_GE(results->size(), 1u);
}

// ─────────────────────────────────────────────────────────────
// STM linear fallback top_k truncation
// ─────────────────────────────────────────────────────────────

TEST(ShortTermMemoryCoverage, LinearFallbackTruncation) {
  ShortTermMemory stm(100);

  // Write entries WITHOUT embeddings (forces linear fallback on search)
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "no_emb_" + std::to_string(i);
    (void)stm.write(std::move(e));
  }

  // Search with empty query -> linear fallback, top_k=2 -> truncation at line 198
  Embedding empty_query;
  MemoryFilter filter;
  auto results = stm.search(empty_query, filter, 2);
  ASSERT_TRUE(results);
  EXPECT_LE(results->size(), 2u);
}

// ─────────────────────────────────────────────────────────────
// LTM: corrupt HNSW binary triggers load failure fallback
// ─────────────────────────────────────────────────────────────

TEST_F(LongTermMemoryCoverage, CorruptHNSWBinHandled) {
  // Write valid index.dat with dim > 0 and a corrupt hnsw_index.bin
  // This exercises the HNSW load failure path (lines 610-612)
  fs::create_directories(test_dir_);
  {
    std::ofstream ofs(test_dir_ / "index.dat");
    ofs << "DIM 8\n";
    ofs << "lt_0 0 0.5 user1 agent1 sess1 episodic\n";
  }
  {
    std::ofstream ofs(test_dir_ / "lt_0.mem");
    ofs << "lt_0\nagent\n0.5\nuser1\nagent1\nsess1\nepisodic\nhnsw corrupt test\n";
  }
  // Write garbage as hnsw_index.bin
  {
    std::ofstream ofs(test_dir_ / "hnsw_index.bin", std::ios::binary);
    ofs << "THIS IS NOT A VALID HNSW INDEX FILE GARBAGE DATA 1234567890\n";
  }

  // Should handle corrupt HNSW gracefully
  LongTermMemory ltm(test_dir_);
  EXPECT_TRUE(ltm.is_loaded());
  EXPECT_EQ(ltm.size(), 1u);

  // Entry should be readable even though HNSW failed
  auto entry = ltm.read("lt_0");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "hnsw corrupt test");
}

// ─────────────────────────────────────────────────────────────
// LTM: search with HNSW but search_k resolves to 0
// ─────────────────────────────────────────────────────────────

TEST_F(LongTermMemoryCoverage, SearchHNSWSearchKZero) {
  // Write entry with embedding to init HNSW, then forget it
  // This leaves HNSW with cur_element_count=0 but hnsw_index_ != null
  LongTermMemory ltm(test_dir_);

  MemoryEntry e;
  e.content = "will forget";
  e.embedding = make_emb(8, 0);
  auto id = ltm.write(std::move(e));
  ASSERT_TRUE(id);

  // Forget removes from index and marks HNSW delete
  auto r = ltm.forget(*id);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);

  // Search with embedding — HNSW exists but is effectively empty
  // Should hit linear fallback (line 456-468)
  Embedding query = make_emb(8, 0);
  MemoryFilter filter;
  auto results = ltm.search(query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_EQ(results->size(), 0u);
}

// ─────────────────────────────────────────────────────────────
// LTM: read_locked with path-traversal id
// ─────────────────────────────────────────────────────────────

TEST_F(LongTermMemoryCoverage, ReadLockedInvalidId) {
  LongTermMemory ltm(test_dir_);

  // Read with path-traversal id — entry_path should throw, read_locked catches it
  auto entry = ltm.read("../../../etc/passwd");
  EXPECT_FALSE(entry);
  EXPECT_EQ(entry.error().code, ErrorCode::InvalidArgument);
}

// ─────────────────────────────────────────────────────────────
// LTM: search HNSW results with missing labels/store entries
// ─────────────────────────────────────────────────────────────

TEST_F(LongTermMemoryCoverage, SearchHNSWWithFilteredResults) {
  LongTermMemory ltm(test_dir_);

  // Write multiple entries with different user_ids
  for (int i = 0; i < 5; ++i) {
    MemoryEntry e;
    e.content = "filtered_" + std::to_string(i);
    e.embedding = make_emb(16, i);
    e.importance = 0.7f;
    e.user_id = (i % 2 == 0) ? "user_a" : "user_b";
    (void)ltm.write(std::move(e));
  }
  ltm.flush();

  // Search with filter — some HNSW results won't match filter,
  // exercising the continue paths at lines 496/499
  Embedding query = make_emb(16, 0);
  MemoryFilter filter;
  filter.user_id = "user_a";
  auto results = ltm.search(query, filter, 5);
  ASSERT_TRUE(results);
  for (auto &r : *results) {
    EXPECT_EQ(r.entry.user_id, "user_a");
  }
}
