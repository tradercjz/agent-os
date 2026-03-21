// ============================================================
// Tests for SharedMemory — thread-safe multi-agent key-value store
// ============================================================
#include <agentos/memory/shared_memory.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace agentos::memory;

TEST(SharedMemoryTest, PutAndGet) {
    SharedMemory sm;
    sm.put("key1", "value1", 10);
    auto v = sm.get("key1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "value1");
}

TEST(SharedMemoryTest, GetMissing) {
    SharedMemory sm;
    EXPECT_FALSE(sm.get("missing").has_value());
}

TEST(SharedMemoryTest, Remove) {
    SharedMemory sm;
    sm.put("k", "v");
    EXPECT_TRUE(sm.remove("k"));
    EXPECT_FALSE(sm.contains("k"));
}

TEST(SharedMemoryTest, RemoveNonexistent) {
    SharedMemory sm;
    EXPECT_FALSE(sm.remove("nonexistent"));
}

TEST(SharedMemoryTest, Keys) {
    SharedMemory sm;
    sm.put("a", "1");
    sm.put("b", "2");
    auto k = sm.keys();
    EXPECT_EQ(k.size(), 2u);
}

TEST(SharedMemoryTest, Contains) {
    SharedMemory sm;
    EXPECT_FALSE(sm.contains("x"));
    sm.put("x", "y");
    EXPECT_TRUE(sm.contains("x"));
}

TEST(SharedMemoryTest, Size) {
    SharedMemory sm;
    EXPECT_EQ(sm.size(), 0u);
    sm.put("a", "1");
    sm.put("b", "2");
    EXPECT_EQ(sm.size(), 2u);
}

TEST(SharedMemoryTest, Clear) {
    SharedMemory sm;
    sm.put("a", "1");
    sm.put("b", "2");
    sm.clear();
    EXPECT_EQ(sm.size(), 0u);
    EXPECT_FALSE(sm.contains("a"));
}

TEST(SharedMemoryTest, Overwrite) {
    SharedMemory sm;
    sm.put("key", "old", 1);
    sm.put("key", "new", 2);
    auto v = sm.get("key");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "new");
    auto e = sm.get_entry("key");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->writer, 2u);
}

TEST(SharedMemoryTest, EntryMetadata) {
    SharedMemory sm;
    sm.put("k", "v", 42);
    auto e = sm.get_entry("k");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->writer, 42u);
    EXPECT_EQ(e->value, "v");
}

TEST(SharedMemoryTest, EntryMissing) {
    SharedMemory sm;
    EXPECT_FALSE(sm.get_entry("missing").has_value());
}

TEST(SharedMemoryTest, ConcurrentAccess) {
    SharedMemory sm;
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < 100; ++j) {
                sm.put("key" + std::to_string(i * 100 + j), "val");
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(sm.size(), 1000u);
}
