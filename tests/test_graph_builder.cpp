#include "graph_engine/builder/graph_builder.hpp"
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace graph_engine;
#include <gtest/gtest.h>
#include <filesystem>
#include "graph_engine/builder/graph_builder.hpp"

namespace fs = std::filesystem;
using namespace graph_engine::builder;

class GraphBuilderTest : public ::testing::Test {
protected:
  fs::path test_dir = "/tmp/agentos_builder_test";

  void SetUp() override {
    if (fs::exists(test_dir))
      fs::remove_all(test_dir);
  }

  void TearDown() override {
    if (fs::exists(test_dir))
      fs::remove_all(test_dir);
  }
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
  EXPECT_TRUE(fs::exists(test_dir / "graph.edges")); // May be empty but file should exist if we try
  EXPECT_TRUE(fs::exists(test_dir / "graph.csc_offsets"));
  EXPECT_TRUE(fs::exists(test_dir / "graph.csc_edges")); // May be empty
  EXPECT_TRUE(fs::exists(test_dir / "entity.dict"));
  EXPECT_TRUE(fs::exists(test_dir / "relation.dict"));
  EXPECT_TRUE(fs::exists(test_dir / "snapshot.meta"));
    void SetUp() override {
        // Create a unique temporary directory for each test
        temp_dir_ = fs::temp_directory_path() / "agentos_graph_builder_test_dir";
        fs::create_directories(temp_dir_);
    }

    void TearDown() override {
        // Clean up the temporary directory after the test
        if (fs::exists(temp_dir_)) {
            fs::remove_all(temp_dir_);
        }
    }

    fs::path temp_dir_;
};

TEST_F(GraphBuilderTest, BuildGraphAndSerialize) {
    GraphBuilder builder(temp_dir_);

    // Add some edges
    builder.add_edge("Alice", "Bob", "knows", 100, 200);
    builder.add_edge("Bob", "Charlie", "follows");
    builder.add_edge("Alice", "Apple", "likes");
    builder.add_edge("Charlie", "Apple", "buys", 300, 400);

    // Verify dictionaries sizes
    auto& entity_dict = builder.get_entity_dict();
    auto& relation_dict = builder.get_relation_dict();

    EXPECT_EQ(entity_dict.size(), 4); // Alice, Bob, Charlie, Apple
    EXPECT_EQ(relation_dict.size(), 4); // knows, follows, likes, buys

    // Build and serialize
    bool result = builder.build();
    EXPECT_TRUE(result);

    // Verify files are created
    EXPECT_TRUE(fs::exists(temp_dir_ / "graph.offsets"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "graph.edges"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "graph.csc_offsets"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "graph.csc_edges"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "entity.dict"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "relation.dict"));
    EXPECT_TRUE(fs::exists(temp_dir_ / "snapshot.meta"));
}
