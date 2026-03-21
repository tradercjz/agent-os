#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::memory;

// Helper: create a test embedding of given dimension filled with a seed value
static Embedding make_test_embedding(size_t dim, float seed) {
    Embedding emb(dim, seed / (dim * seed));  // Normalize roughly
    return emb;
}

class SmartConsolidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        ASSERT_NE(info, nullptr);
        test_dir_ = std::filesystem::temp_directory_path() /
                    std::filesystem::path("agentos_smart_consolidation_" +
                                          std::string(info->name()));
        std::filesystem::remove_all(test_dir_);
    }
    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }
    std::filesystem::path test_dir_;
};

TEST_F(SmartConsolidationTest, HighImportancePromotesToLTM) {
    MemorySystem ms(test_dir_);
    MemoryEntry entry;
    entry.id = "important";
    entry.content = "critical fact";
    entry.importance = 0.9f;
    entry.embedding = make_test_embedding(128, 1.0f);
    (void)ms.working().write(entry);

    size_t promoted = ms.smart_consolidate(0.4f, 0.7f);
    EXPECT_GE(promoted, 1u);
    // Should be in long-term
    auto r = ms.long_term().read("important");
    EXPECT_TRUE(r.has_value());
}

TEST_F(SmartConsolidationTest, MediumImportancePromotesToSTM) {
    MemorySystem ms(test_dir_);
    MemoryEntry entry;
    entry.id = "medium";
    entry.content = "moderately important";
    entry.importance = 0.5f;
    entry.embedding = make_test_embedding(128, 2.0f);
    (void)ms.working().write(entry);

    ms.smart_consolidate(0.4f, 0.7f);
    auto r = ms.short_term().read("medium");
    EXPECT_TRUE(r.has_value());
}

TEST_F(SmartConsolidationTest, LowImportanceStaysInWorking) {
    MemorySystem ms(test_dir_);
    MemoryEntry entry;
    entry.id = "trivial";
    entry.content = "not important";
    entry.importance = 0.1f;
    entry.embedding = make_test_embedding(128, 3.0f);
    (void)ms.working().write(entry);

    ms.smart_consolidate(0.4f, 0.7f);
    auto r = ms.working().read("trivial");
    EXPECT_TRUE(r.has_value());
}

TEST_F(SmartConsolidationTest, FrequencyBoostsScore) {
    MemorySystem ms(test_dir_);
    MemoryEntry entry;
    entry.id = "accessed";
    entry.content = "frequently accessed";
    entry.importance = 0.35f;  // below l1_threshold alone
    entry.access_count = 20;   // but frequency bonus pushes it above
    entry.embedding = make_test_embedding(128, 4.0f);
    (void)ms.working().write(entry);

    ms.smart_consolidate(0.4f, 0.7f);
    // With access_count=20, freq_bonus = min(0.3, log(21)*0.1) ~= 0.3
    // score ~= 0.35 + 0.3 = 0.65 -> promotes to STM
    auto r = ms.short_term().read("accessed");
    EXPECT_TRUE(r.has_value());
}

TEST_F(SmartConsolidationTest, ZeroEntriesReturnsZero) {
    MemorySystem ms(test_dir_);
    size_t promoted = ms.smart_consolidate(0.4f, 0.7f);
    EXPECT_EQ(promoted, 0u);
}

TEST_F(SmartConsolidationTest, MultipleEntriesSortCorrectly) {
    MemorySystem ms(test_dir_);

    // High importance -> LTM
    MemoryEntry high;
    high.id = "high";
    high.content = "high importance";
    high.importance = 0.8f;
    high.embedding = make_test_embedding(128, 1.0f);
    (void)ms.working().write(high);

    // Medium importance -> STM
    MemoryEntry med;
    med.id = "med";
    med.content = "medium importance";
    med.importance = 0.5f;
    med.embedding = make_test_embedding(128, 2.0f);
    (void)ms.working().write(med);

    // Low importance -> stays in WM
    MemoryEntry low;
    low.id = "low";
    low.content = "low importance";
    low.importance = 0.1f;
    low.embedding = make_test_embedding(128, 3.0f);
    (void)ms.working().write(low);

    size_t promoted = ms.smart_consolidate(0.4f, 0.7f);
    EXPECT_EQ(promoted, 2u);

    EXPECT_TRUE(ms.long_term().read("high").has_value());
    EXPECT_TRUE(ms.short_term().read("med").has_value());
    EXPECT_TRUE(ms.working().read("low").has_value());
}
