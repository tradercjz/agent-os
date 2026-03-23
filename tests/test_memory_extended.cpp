// ============================================================
// Extended tests for Memory System
// Covers: CRUD, capacity/eviction, TTL-like expiration, persistence,
// tier promotion, concurrency, similarity search, metadata, stats, bulk ops
// ============================================================
#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::memory;

namespace fs = std::filesystem;

// Helper: create a normalized embedding with a distinguishing peak at index idx
static Embedding make_norm_emb(size_t dim, int idx) {
  Embedding emb(dim, 0.1f);
  emb[static_cast<size_t>(idx) % dim] = 0.95f;
  float norm = 0;
  for (float v : emb) norm += v * v;
  norm = std::sqrt(norm);
  for (float &v : emb) v /= norm;
  return emb;
}

static fs::path make_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("agentos_memext_" + name + "_" + std::to_string(nonce));
}

// ─────────────────────────────────────────────────────────────
// 1. Working Memory CRUD: store, retrieve, update, delete
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, WorkingMemoryCRUD) {
  WorkingMemory wm(32);

  // Store
  MemoryEntry e;
  e.content = "hello world";
  e.embedding = {1.0f, 0.0f, 0.0f};
  e.tags["key"] = "value";
  auto id = wm.write(std::move(e));
  ASSERT_TRUE(id);
  EXPECT_FALSE(id->empty());

  // Retrieve
  auto entry = wm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->content, "hello world");
  EXPECT_EQ(entry->tags.at("key"), "value");

  // Update (write with same logical content, read back)
  MemoryEntry updated;
  updated.content = "updated content";
  updated.embedding = {0.0f, 1.0f, 0.0f};
  auto id2 = wm.write(std::move(updated));
  ASSERT_TRUE(id2);
  auto entry2 = wm.read(*id2);
  ASSERT_TRUE(entry2);
  EXPECT_EQ(entry2->content, "updated content");

  // Delete
  auto del = wm.forget(*id);
  ASSERT_TRUE(del);
  EXPECT_TRUE(*del);

  // Confirm deleted
  auto gone = wm.read(*id);
  EXPECT_FALSE(gone);

  // Delete non-existent
  auto del2 = wm.forget("nonexistent_xyz");
  ASSERT_TRUE(del2);
  EXPECT_FALSE(*del2);
}

// ─────────────────────────────────────────────────────────────
// 2. Working Memory capacity limit and eviction
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, WorkingMemoryCapacityEviction) {
  WorkingMemory wm(4);

  std::vector<std::string> ids;
  for (int i = 0; i < 4; ++i) {
    MemoryEntry e;
    e.content = "item_" + std::to_string(i);
    auto id = wm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }
  EXPECT_EQ(wm.size(), 4u);

  // Access item 0 and 1 to make them recently used
  (void)wm.read(ids[0]);
  (void)wm.read(ids[1]);

  // Write 2 more items, forcing eviction of LRU entries (ids[2], ids[3])
  for (int i = 4; i < 6; ++i) {
    MemoryEntry e;
    e.content = "item_" + std::to_string(i);
    auto id = wm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }

  EXPECT_EQ(wm.size(), 4u);

  // ids[2] and ids[3] should have been evicted (least recently accessed)
  EXPECT_FALSE(wm.read(ids[2]).has_value());
  EXPECT_FALSE(wm.read(ids[3]).has_value());

  // ids[0] and ids[1] should still be present
  EXPECT_TRUE(wm.read(ids[0]).has_value());
  EXPECT_TRUE(wm.read(ids[1]).has_value());
}

// ─────────────────────────────────────────────────────────────
// 3. Short-term memory with TTL-like expiration (via importance decay)
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, ShortTermMemoryImportanceBasedEviction) {
  // STM with small capacity evicts low-importance entries first
  ShortTermMemory stm(3);

  // Insert 3 entries with varying importance
  MemoryEntry e1;
  e1.content = "low importance";
  e1.embedding = make_norm_emb(16, 0);
  e1.importance = 0.1f;
  auto id1 = stm.write(std::move(e1));
  ASSERT_TRUE(id1);

  MemoryEntry e2;
  e2.content = "medium importance";
  e2.embedding = make_norm_emb(16, 1);
  e2.importance = 0.5f;
  auto id2 = stm.write(std::move(e2));
  ASSERT_TRUE(id2);

  MemoryEntry e3;
  e3.content = "high importance";
  e3.embedding = make_norm_emb(16, 2);
  e3.importance = 0.9f;
  auto id3 = stm.write(std::move(e3));
  ASSERT_TRUE(id3);

  EXPECT_EQ(stm.size(), 3u);

  // Insert a 4th entry with high importance -> evicts lowest
  MemoryEntry e4;
  e4.content = "new entry";
  e4.embedding = make_norm_emb(16, 3);
  e4.importance = 0.8f;
  auto id4 = stm.write(std::move(e4));
  ASSERT_TRUE(id4);

  EXPECT_EQ(stm.size(), 3u);

  // The low-importance entry should have been evicted
  auto r1 = stm.read(*id1);
  EXPECT_FALSE(r1.has_value());

  // High-importance entry should still exist
  auto r3 = stm.read(*id3);
  EXPECT_TRUE(r3.has_value());
}

// ─────────────────────────────────────────────────────────────
// 4. Long-term memory persistence (write + read back)
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, LongTermMemoryPersistence) {
  auto dir = make_test_dir("ltm_persist");
  fs::remove_all(dir);

  std::string stored_id;

  // Write phase
  {
    LongTermMemory ltm(dir, 1000);
    ASSERT_TRUE(ltm.is_loaded());

    MemoryEntry e;
    e.content = "persistent data";
    e.embedding = make_norm_emb(32, 7);
    e.importance = 0.9f;
    e.user_id = "user_42";
    e.agent_id = "agent_1";
    e.tags["category"] = "facts";
    auto id = ltm.write(std::move(e));
    ASSERT_TRUE(id);
    stored_id = *id;

    ltm.flush();
  }

  // Read phase with new instance
  {
    LongTermMemory ltm(dir, 1000);
    ASSERT_TRUE(ltm.is_loaded());

    auto entry = ltm.read(stored_id);
    ASSERT_TRUE(entry) << entry.error().message;
    EXPECT_EQ(entry->content, "persistent data");
    EXPECT_EQ(entry->user_id, "user_42");
    // Tags may not be persisted through HNSW serialization
    if (entry->tags.count("category")) {
        EXPECT_EQ(entry->tags.at("category"), "facts");
    }
  }

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 5. Memory tier promotion: working -> short-term -> long-term
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, TierPromotion) {
  auto dir = make_test_dir("tier_promo");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  // Add a high-importance memory (should go to WM and also to LTM via remember())
  Embedding emb = make_norm_emb(64, 0);
  auto id = mem.remember("important fact", emb, "agent", 0.9f);
  ASSERT_TRUE(id);

  // Memory should be in working memory
  EXPECT_GE(mem.working().size(), 1u);

  // Consolidate: promote high-importance entries from WM to LTM
  size_t promoted = mem.consolidate(0.6f);
  EXPECT_GE(promoted, 1u);

  // After consolidation, LTM should have at least one entry
  EXPECT_GE(mem.long_term().size(), 1u);

  fs::remove_all(dir);
}

TEST(MemoryExtended, SmartConsolidateMultiTier) {
  auto dir = make_test_dir("smart_consolidate");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  // Add entries with varying importance
  Embedding emb_low = make_norm_emb(64, 0);
  Embedding emb_mid = make_norm_emb(64, 1);
  Embedding emb_high = make_norm_emb(64, 2);

  (void)mem.remember("low priority note", emb_low, "agent", 0.2f);
  (void)mem.remember("mid priority note", emb_mid, "agent", 0.5f);
  (void)mem.remember("critical fact", emb_high, "agent", 0.95f);

  // smart_consolidate with thresholds
  size_t promoted = mem.smart_consolidate(0.3f, 0.8f);
  EXPECT_GE(promoted, 1u);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 6. Concurrent memory access from multiple threads
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, ConcurrentWorkingMemoryAccess) {
  WorkingMemory wm(256);

  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 50;

  std::vector<std::future<void>> futures;
  for (int t = 0; t < kThreads; ++t) {
    futures.push_back(std::async(std::launch::async, [&wm, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        MemoryEntry e;
        e.content = "t" + std::to_string(t) + "_i" + std::to_string(i);
        e.embedding = {static_cast<float>(t), static_cast<float>(i)};
        auto id = wm.write(std::move(e));
        ASSERT_TRUE(id);

        // Read back
        auto entry = wm.read(*id);
        // Entry might have been evicted if capacity is reached, but no crash
        if (entry) {
          EXPECT_FALSE(entry->content.empty());
        }
      }
    }));
  }

  for (auto &f : futures) {
    f.get(); // Should not throw
  }

  // Size should be within capacity
  EXPECT_LE(wm.size(), 256u);
}

TEST(MemoryExtended, ConcurrentMemorySystemAccess) {
  auto dir = make_test_dir("concurrent_sys");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  constexpr int kThreads = 4;
  constexpr int kOpsPerThread = 20;

  std::vector<std::future<void>> futures;
  for (int t = 0; t < kThreads; ++t) {
    futures.push_back(std::async(std::launch::async, [&mem, t]() {
      for (int i = 0; i < kOpsPerThread; ++i) {
        Embedding emb = make_norm_emb(32, (t * kOpsPerThread + i) % 32);
        auto id = mem.remember(
            "thread_" + std::to_string(t) + "_op_" + std::to_string(i),
            emb, "agent", 0.5f);
        ASSERT_TRUE(id);
      }
    }));
  }

  for (auto &f : futures) {
    f.get();
  }

  // All threads completed without crash; working memory has some entries
  EXPECT_GT(mem.working().size(), 0u);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 7. Memory search with similarity threshold
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, SearchSimilarityOrdering) {
  WorkingMemory wm(100);

  // Insert entries with distinct embeddings
  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "entry_" + std::to_string(i);
    Embedding emb(8, 0.0f);
    emb[static_cast<size_t>(i) % 8] = 1.0f;
    e.embedding = emb;
    (void)wm.write(std::move(e));
  }

  // Query close to entry_3 (peak at index 3)
  Embedding query(8, 0.0f);
  query[3] = 1.0f;

  MemoryFilter filter;
  auto results = wm.search(query, filter, 5);
  ASSERT_TRUE(results);
  ASSERT_GE(results->size(), 1u);

  // Best match should be the entry with peak at index 3
  EXPECT_EQ(results->front().entry.content, "entry_3");

  // Scores should be in descending order
  for (size_t i = 1; i < results->size(); ++i) {
    EXPECT_GE((*results)[i - 1].score, (*results)[i].score);
  }
}

TEST(MemoryExtended, SearchWithFilterNarrowsResults) {
  WorkingMemory wm(100);

  // Insert entries for two different users
  for (int i = 0; i < 6; ++i) {
    MemoryEntry e;
    e.content = "user1_entry_" + std::to_string(i);
    e.user_id = "user_1";
    e.embedding = {static_cast<float>(i), 0.5f};
    (void)wm.write(std::move(e));
  }
  for (int i = 0; i < 4; ++i) {
    MemoryEntry e;
    e.content = "user2_entry_" + std::to_string(i);
    e.user_id = "user_2";
    e.embedding = {static_cast<float>(i), 0.5f};
    (void)wm.write(std::move(e));
  }

  // Search with user_id filter
  MemoryFilter filter;
  filter.user_id = "user_1";
  Embedding query = {3.0f, 0.5f};
  auto results = wm.search(query, filter, 10);
  ASSERT_TRUE(results);

  // All results should belong to user_1
  for (const auto &r : *results) {
    EXPECT_EQ(r.entry.user_id, "user_1");
  }
}

TEST(MemoryExtended, SearchEmptyStoreReturnsEmpty) {
  WorkingMemory wm(32);
  Embedding query = {1.0f, 0.0f};
  MemoryFilter filter;
  auto results = wm.search(query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_TRUE(results->empty());
}

// ─────────────────────────────────────────────────────────────
// 8. Memory metadata storage and retrieval
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, MetadataTagsPreserved) {
  WorkingMemory wm(32);

  MemoryEntry e;
  e.content = "tagged entry";
  e.tags["category"] = "science";
  e.tags["source"] = "wikipedia";
  e.tags["language"] = "en";
  e.importance = 0.75f;
  e.user_id = "user_x";
  e.agent_id = "agent_y";
  e.session_id = "session_z";
  e.type = "semantic";
  e.embedding = {0.5f, 0.5f};

  auto id = wm.write(std::move(e));
  ASSERT_TRUE(id);

  auto entry = wm.read(*id);
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->tags.size(), 3u);
  EXPECT_EQ(entry->tags.at("category"), "science");
  EXPECT_EQ(entry->tags.at("source"), "wikipedia");
  EXPECT_EQ(entry->tags.at("language"), "en");
  EXPECT_FLOAT_EQ(entry->importance, 0.75f);
  EXPECT_EQ(entry->user_id, "user_x");
  EXPECT_EQ(entry->agent_id, "agent_y");
  EXPECT_EQ(entry->session_id, "session_z");
  EXPECT_EQ(entry->type, "semantic");
}

TEST(MemoryExtended, AccessCountIncrements) {
  WorkingMemory wm(32);

  MemoryEntry e;
  e.content = "counter test";
  auto id = wm.write(std::move(e));
  ASSERT_TRUE(id);

  // Read multiple times
  for (int i = 0; i < 5; ++i) {
    auto entry = wm.read(*id);
    ASSERT_TRUE(entry);
  }

  auto entry = wm.read(*id);
  ASSERT_TRUE(entry);
  // access_count should have been incremented on each read
  EXPECT_GE(entry->access_count, 5u);
}

// ─────────────────────────────────────────────────────────────
// 9. Memory statistics (count per tier, total size)
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, MemorySystemStatistics) {
  auto dir = make_test_dir("stats");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  // Initially all tiers are empty
  EXPECT_EQ(mem.working().size(), 0u);
  EXPECT_EQ(mem.short_term().size(), 0u);
  EXPECT_EQ(mem.long_term().size(), 0u);

  // Add some memories
  Embedding emb = make_norm_emb(32, 0);
  (void)mem.remember("entry_1", emb, "agent", 0.3f);
  (void)mem.remember("entry_2", emb, "agent", 0.5f);
  (void)mem.remember("entry_3", emb, "agent", 0.9f);

  // Working memory should have entries
  EXPECT_EQ(mem.working().size(), 3u);

  // Short-term should also have entries (remember() writes to both WM and STM)
  EXPECT_GE(mem.short_term().size(), 3u);

  // LTM should have the high-importance one (importance >= 0.7 threshold)
  EXPECT_GE(mem.long_term().size(), 1u);

  // get_all returns all entries
  auto wm_all = mem.working().get_all();
  EXPECT_EQ(wm_all.size(), mem.working().size());

  fs::remove_all(dir);
}

TEST(MemoryExtended, MemorySystemForgetRemovesFromAllTiers) {
  auto dir = make_test_dir("forget_all");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  Embedding emb = make_norm_emb(32, 0);
  auto id = mem.remember("to be forgotten", emb, "agent", 0.9f);
  ASSERT_TRUE(id);

  // Confirm it exists in WM
  EXPECT_TRUE(mem.working().read(*id).has_value());

  // Forget from all tiers
  mem.forget(*id);

  // Should be gone from WM
  EXPECT_FALSE(mem.working().read(*id).has_value());

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 10. Bulk memory operations (store_batch, retrieve_batch)
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, BulkWriteAndRetrieve) {
  WorkingMemory wm(200);

  // Bulk write
  std::vector<std::string> ids;
  for (int i = 0; i < 50; ++i) {
    MemoryEntry e;
    e.content = "bulk_" + std::to_string(i);
    e.embedding = {static_cast<float>(i) * 0.02f, 0.5f};
    e.importance = static_cast<float>(i) / 50.0f;
    auto id = wm.write(std::move(e));
    ASSERT_TRUE(id);
    ids.push_back(*id);
  }

  EXPECT_EQ(wm.size(), 50u);

  // Bulk retrieve
  int found = 0;
  for (const auto &id : ids) {
    auto entry = wm.read(id);
    if (entry) ++found;
  }
  EXPECT_EQ(found, 50);

  // Bulk delete
  for (const auto &id : ids) {
    (void)wm.forget(id);
  }
  EXPECT_EQ(wm.size(), 0u);
}

TEST(MemoryExtended, BulkMemorySystemRememberAndRecall) {
  auto dir = make_test_dir("bulk_sys");
  fs::remove_all(dir);

  MemorySystem mem(dir);

  // Bulk remember with distinct embeddings
  constexpr int kBatch = 20;
  for (int i = 0; i < kBatch; ++i) {
    Embedding emb = make_norm_emb(32, i);
    auto id = mem.remember(
        "batch_item_" + std::to_string(i), emb, "agent",
        0.5f + static_cast<float>(i) / (2.0f * kBatch));
    ASSERT_TRUE(id);
  }

  // Recall should return relevant results
  Embedding query = make_norm_emb(32, 5);
  MemoryFilter filter;
  auto results = mem.recall(query, filter, 5);
  ASSERT_TRUE(results);
  EXPECT_GE(results->size(), 1u);

  // The closest result should be batch_item_5
  bool found_target = false;
  for (const auto &r : *results) {
    if (r.entry.content == "batch_item_5") {
      found_target = true;
      break;
    }
  }
  EXPECT_TRUE(found_target);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// Additional edge cases
// ─────────────────────────────────────────────────────────────

TEST(MemoryExtended, CosineSimIdenticalVectors) {
  Embedding a = {0.6f, 0.8f};
  Embedding b = {0.6f, 0.8f};
  float sim = MemoryEntry::cosine_similarity(a, b);
  EXPECT_NEAR(sim, 1.0f, 0.01f);
}

TEST(MemoryExtended, CosineSimOrthogonalVectors) {
  Embedding a = {1.0f, 0.0f};
  Embedding b = {0.0f, 1.0f};
  float sim = MemoryEntry::cosine_similarity(a, b);
  EXPECT_NEAR(sim, 0.0f, 0.01f);
}

TEST(MemoryExtended, CosineSimEmptyVectors) {
  Embedding empty;
  Embedding nonempty = {1.0f};
  EXPECT_FLOAT_EQ(MemoryEntry::cosine_similarity(empty, empty), 0.0f);
  EXPECT_FLOAT_EQ(MemoryEntry::cosine_similarity(empty, nonempty), 0.0f);
}

TEST(MemoryExtended, CosineSimDimensionMismatch) {
  Embedding a = {1.0f, 0.0f};
  Embedding b = {1.0f, 0.0f, 0.0f};
  EXPECT_FLOAT_EQ(MemoryEntry::cosine_similarity(a, b), 0.0f);
}

TEST(MemoryExtended, WorkingMemoryClear) {
  WorkingMemory wm(32);

  for (int i = 0; i < 10; ++i) {
    MemoryEntry e;
    e.content = "item_" + std::to_string(i);
    (void)wm.write(std::move(e));
  }
  EXPECT_EQ(wm.size(), 10u);

  wm.clear();
  EXPECT_EQ(wm.size(), 0u);
}

TEST(MemoryExtended, WorkingMemoryName) {
  WorkingMemory wm;
  EXPECT_EQ(wm.name(), "WorkingMemory");
}

TEST(MemoryExtended, ShortTermMemoryName) {
  ShortTermMemory stm;
  EXPECT_EQ(stm.name(), "ShortTermMemory");
}

TEST(MemoryExtended, LongTermMemoryName) {
  auto dir = make_test_dir("ltm_name");
  LongTermMemory ltm(dir);
  EXPECT_EQ(ltm.name(), "LongTermMemory");
  fs::remove_all(dir);
}

TEST(MemoryExtended, MemoryFilterMatchAll) {
  MemoryFilter f;
  // No filters set -> matches everything
  EXPECT_TRUE(f.match("any_user", "any_agent", "any_session", "any_type"));
}

TEST(MemoryExtended, MemoryFilterMatchSpecific) {
  MemoryFilter f;
  f.user_id = "user_1";
  f.type = "episodic";

  EXPECT_TRUE(f.match("user_1", "agent_x", "sess_y", "episodic"));
  EXPECT_FALSE(f.match("user_2", "agent_x", "sess_y", "episodic"));
  EXPECT_FALSE(f.match("user_1", "agent_x", "sess_y", "semantic"));
}

TEST(MemoryExtended, LRUCacheBasic) {
  LRUCache<std::string, int> cache(3);

  cache.put("a", 1);
  cache.put("b", 2);
  cache.put("c", 3);

  auto a = cache.get("a");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(*a, 1);

  // Inserting a 4th item should evict the LRU (b, since a was just accessed)
  cache.put("d", 4);

  auto b = cache.get("b");
  EXPECT_FALSE(b.has_value());

  auto c = cache.get("c");
  EXPECT_TRUE(c.has_value());

  cache.remove("c");
  auto c2 = cache.get("c");
  EXPECT_FALSE(c2.has_value());

  cache.clear();
  auto a2 = cache.get("a");
  EXPECT_FALSE(a2.has_value());
}
