#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::memory;

class ShortTermPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = (std::filesystem::temp_directory_path() / "test_stm_persist").string();
        std::filesystem::create_directories(test_dir_);
    }
    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
    std::string test_dir_;

    Embedding make_embedding(float seed) {
        Embedding e(128, 0.0f);
        for (size_t i = 0; i < e.size(); ++i) {
            e[i] = std::sin(seed + static_cast<float>(i) * 0.1f);
        }
        // Normalize
        float norm = 0;
        for (auto v : e) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 0) for (auto& v : e) v /= norm;
        return e;
    }
};

TEST_F(ShortTermPersistenceTest, SaveAndLoadRoundTrip) {
    // Write entries
    {
        ShortTermMemory stm(512);
        for (int i = 0; i < 5; ++i) {
            MemoryEntry entry;
            entry.id = "entry_" + std::to_string(i);
            entry.content = "content " + std::to_string(i);
            entry.importance = 0.5f + static_cast<float>(i) * 0.1f;
            entry.embedding = make_embedding(static_cast<float>(i));
            auto r = stm.write(entry);
            ASSERT_TRUE(r.has_value());
        }
        auto sr = stm.save(test_dir_);
        ASSERT_TRUE(sr.has_value());
    }

    // Load into new instance
    {
        ShortTermMemory stm2(512);
        auto lr = stm2.load(test_dir_);
        ASSERT_TRUE(lr.has_value());
        EXPECT_EQ(stm2.size(), 5u);

        // Verify entries readable
        for (int i = 0; i < 5; ++i) {
            auto r = stm2.read("entry_" + std::to_string(i));
            ASSERT_TRUE(r.has_value());
            EXPECT_EQ(r->content, "content " + std::to_string(i));
        }

        // Verify search works
        auto results = stm2.search(make_embedding(0.0f), MemoryFilter{}, 3);
        ASSERT_TRUE(results.has_value());
        EXPECT_GE(results->size(), 1u);
    }
}

TEST_F(ShortTermPersistenceTest, LoadMissingFilesStartsFresh) {
    ShortTermMemory stm(512);
    auto r = stm.load("/nonexistent_dir_12345");
    ASSERT_TRUE(r.has_value()); // no error
    EXPECT_EQ(stm.size(), 0u);
}

TEST_F(ShortTermPersistenceTest, DirtyFlagTracking) {
    ShortTermMemory stm(512);

    // Initially not dirty -- save should be a no-op
    auto sr = stm.save(test_dir_);
    ASSERT_TRUE(sr.has_value());
    // No files created (nothing to save)
    EXPECT_FALSE(std::filesystem::exists(test_dir_ + "/stm_hnsw.bin"));

    // Write makes it dirty
    MemoryEntry entry;
    entry.id = "test";
    entry.content = "hello";
    entry.embedding = make_embedding(1.0f);
    (void)stm.write(entry);

    // Now save should create files
    sr = stm.save(test_dir_);
    ASSERT_TRUE(sr.has_value());
    EXPECT_TRUE(std::filesystem::exists(test_dir_ + "/stm_hnsw.bin"));
    EXPECT_TRUE(std::filesystem::exists(test_dir_ + "/stm_metadata.jsonl"));
}
