#include "graph_engine/builder/graph_builder.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace graph_engine;

class CSRGraphTest : public ::testing::Test {
protected:
  fs::path test_dir = "/tmp/agentos_csr_test";

  void SetUp() override {
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
  }

  void TearDown() override {
    if (fs::exists(test_dir)) {
      // fs::remove_all(test_dir); // Un-comment to clean up
    }
  }
};

TEST_F(CSRGraphTest, BasicBuildAndSearch) {
  // 1. Build Phase (Offline)
  {
    builder::GraphBuilder builder(test_dir);

    builder.add_edge("Apple", "Steve_Jobs", "founded_by");
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    builder.add_edge("Steve_Jobs", "Pixar", "founded");
    builder.add_edge("Pixar", "Disney", "acquired_by");

    ASSERT_TRUE(builder.build());
  }

  // 2. Query Phase (Online)
  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    auto meta = graph.get_meta();
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->num_nodes, 5); // Apple, Steve_Jobs, Tim_Cook, Pixar, Disney
                                   // = 5 nodes Disney. Let's check.
    // get_neighbors:
    auto apple_neighbors = graph.get_neighbors("Apple");
    EXPECT_EQ(apple_neighbors.size(), 2);

    bool has_jobs = false, has_cook = false;
    for (auto &[dst, rel] : apple_neighbors) {
      if (dst == "Steve_Jobs" && rel == "founded_by")
        has_jobs = true;
      if (dst == "Tim_Cook" && rel == "ceo_of")
        has_cook = true;
    }
    EXPECT_TRUE(has_jobs);
    EXPECT_TRUE(has_cook);

    auto jobs_neighbors = graph.get_neighbors("Steve_Jobs");
    EXPECT_EQ(jobs_neighbors.size(), 1);
    EXPECT_EQ(jobs_neighbors[0].first, "Pixar");
    EXPECT_EQ(jobs_neighbors[0].second, "founded");

    auto fake_neighbors = graph.get_neighbors("Microsoft");
    EXPECT_TRUE(fake_neighbors.empty());
  }
}
