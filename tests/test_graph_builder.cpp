#include <gtest/gtest.h>
#include <filesystem>
#include "graph_engine/builder/graph_builder.hpp"

namespace fs = std::filesystem;
using namespace graph_engine::builder;

class GraphBuilderTest : public ::testing::Test {
protected:
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
