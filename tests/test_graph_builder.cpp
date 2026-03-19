#include "graph_engine/builder/graph_builder.hpp"
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace graph_engine;
using namespace graph_engine::builder;

class GraphBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = fs::temp_directory_path() / "agentos_graph_builder_test";
        if (fs::exists(test_dir)) fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        if (fs::exists(test_dir)) fs::remove_all(test_dir);
    }

    fs::path test_dir;
};

TEST_F(GraphBuilderTest, BasicAddEdge) {
    GraphBuilder builder(test_dir);

    builder.add_edge("Apple", "Steve_Jobs", "founded_by");
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    builder.add_edge("Steve_Jobs", "Pixar", "founded");

    auto& entity_dict = builder.get_entity_dict();
    auto& relation_dict = builder.get_relation_dict();

    EXPECT_EQ(entity_dict.size(), 4u);
    EXPECT_EQ(relation_dict.size(), 3u);

    EntityID eid;
    EXPECT_TRUE(entity_dict.find_id("Apple", eid));
    EXPECT_TRUE(entity_dict.find_id("Steve_Jobs", eid));
    EXPECT_TRUE(entity_dict.find_id("Tim_Cook", eid));
    EXPECT_TRUE(entity_dict.find_id("Pixar", eid));

    EXPECT_TRUE(relation_dict.find_id("founded_by", eid));
    EXPECT_TRUE(relation_dict.find_id("ceo_of", eid));
    EXPECT_TRUE(relation_dict.find_id("founded", eid));
}

TEST_F(GraphBuilderTest, BuildCSRAndCSC) {
    GraphBuilder builder(test_dir);

    builder.add_edge("Apple", "Steve_Jobs", "founded_by");
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    builder.add_edge("Steve_Jobs", "Pixar", "founded");

    ASSERT_TRUE(builder.build());

    EXPECT_TRUE(fs::exists(test_dir / "graph.offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.edges"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_edges"));
    EXPECT_TRUE(fs::exists(test_dir / "entity.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "relation.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "snapshot.meta"));
}

TEST_F(GraphBuilderTest, BuildEmptyGraph) {
    GraphBuilder builder(test_dir);

    ASSERT_TRUE(builder.build());

    EXPECT_TRUE(fs::exists(test_dir / "graph.offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.edges"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_edges"));
    EXPECT_TRUE(fs::exists(test_dir / "entity.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "relation.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "snapshot.meta"));
}

TEST_F(GraphBuilderTest, BuildGraphAndSerializeLarge) {
    GraphBuilder builder(test_dir);

    // Add some edges
    builder.add_edge("Alice", "Bob", "knows", 100, 200);
    builder.add_edge("Bob", "Charlie", "follows");
    builder.add_edge("Alice", "Apple", "likes");
    builder.add_edge("Charlie", "Apple", "buys", 300, 400);

    // Verify dictionaries sizes
    auto& entity_dict = builder.get_entity_dict();
    auto& relation_dict = builder.get_relation_dict();

    EXPECT_EQ(entity_dict.size(), 4u); // Alice, Bob, Charlie, Apple
    EXPECT_EQ(relation_dict.size(), 4u); // knows, follows, likes, buys

    // Build and serialize
    bool result = builder.build();
    EXPECT_TRUE(result);

    // Verify files are created
    EXPECT_TRUE(fs::exists(test_dir / "graph.offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.edges"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_offsets"));
    EXPECT_TRUE(fs::exists(test_dir / "graph.csc_edges"));
    EXPECT_TRUE(fs::exists(test_dir / "entity.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "relation.dict"));
    EXPECT_TRUE(fs::exists(test_dir / "snapshot.meta"));
}
