#include <agentos/news/relation_extractor.hpp>
#include <gtest/gtest.h>

using namespace agentos::news;

// ── Tests for inline defined functionality in relation_extractor.hpp ──────────────────

TEST(RelationExtractorTest, PairHashComputesSameHashForSamePairs) {
    PairHash hasher;
    std::pair<EntityType, EntityType> p1{EntityType::PERSON, EntityType::COMPANY};
    std::pair<EntityType, EntityType> p2{EntityType::PERSON, EntityType::COMPANY};

    // Same pairs should have same hash
    EXPECT_EQ(hasher(p1), hasher(p2));
}

TEST(RelationExtractorTest, PairHashComputesDifferentHashForDifferentPairs) {
    PairHash hasher;
    std::pair<EntityType, EntityType> p1{EntityType::PERSON, EntityType::COMPANY};
    std::pair<EntityType, EntityType> p2{EntityType::COMPANY, EntityType::PERSON};

    // Different pairs should likely have different hashes
    EXPECT_NE(hasher(p1), hasher(p2));
}

TEST(RelationExtractorTest, RelationPatternStructInitialization) {
    auto pos_extractor = [](const std::smatch& /* m */) -> std::pair<size_t, size_t> {
        return {0, 0};
    };

    RelationPattern pattern(RelationType::WORKS_FOR, "works for", 0.85, pos_extractor);
    EXPECT_EQ(pattern.type, RelationType::WORKS_FOR);
    EXPECT_DOUBLE_EQ(pattern.confidence_weight, 0.85);
    EXPECT_TRUE(pattern.position_extractor != nullptr);
}

TEST(RelationExtractorTest, RelationPatternStructDefaultParams) {
    RelationPattern pattern(RelationType::FOUNDED_BY, "founder");
    EXPECT_EQ(pattern.type, RelationType::FOUNDED_BY);
    EXPECT_DOUBLE_EQ(pattern.confidence_weight, 0.8);
    EXPECT_TRUE(pattern.position_extractor == nullptr);
}
