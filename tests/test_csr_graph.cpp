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
    if (fs::exists(test_dir))
      fs::remove_all(test_dir);
  }

  void TearDown() override {
    if (fs::exists(test_dir))
      fs::remove_all(test_dir);
  }
};

TEST_F(CSRGraphTest, BasicBuildAndSearch) {
  {
    builder::GraphBuilder builder(test_dir);
    builder.add_edge("Apple", "Steve_Jobs", "founded_by");
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    builder.add_edge("Steve_Jobs", "Pixar", "founded");
    builder.add_edge("Pixar", "Disney", "acquired_by");
    ASSERT_TRUE(builder.build());
  }

  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    auto meta = graph.get_meta();
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(meta->num_nodes, 5);

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

    auto fake = graph.get_neighbors("Microsoft");
    EXPECT_TRUE(fake.empty());
  }
}

TEST_F(CSRGraphTest, TemporalEdgeFiltering) {
  {
    builder::GraphBuilder builder(test_dir);
    // 2020年: Jobs 是 CEO
    builder.add_edge("Apple", "Steve_Jobs", "ceo_of", 1000, 2000);
    // 2021年起: Cook 是 CEO
    builder.add_edge("Apple", "Tim_Cook", "ceo_of", 2001, 0);
    // 永久关系
    builder.add_edge("Apple", "iPhone", "product");
    ASSERT_TRUE(builder.build());
  }

  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    // ts=0: 不过滤，返回全部
    auto all = graph.get_neighbors("Apple", 0);
    EXPECT_EQ(all.size(), 3);

    // ts=1500: Jobs 是 CEO（1000~2000 有效）
    auto at_1500 = graph.get_neighbors("Apple", 1500);
    ASSERT_EQ(at_1500.size(), 2); // Jobs + iPhone
    bool found_jobs = false;
    for (auto &[dst, rel] : at_1500) {
      if (dst == "Steve_Jobs")
        found_jobs = true;
    }
    EXPECT_TRUE(found_jobs);

    // ts=2500: Cook 是 CEO（2001~∞ 有效），Jobs 已失效
    auto at_2500 = graph.get_neighbors("Apple", 2500);
    ASSERT_EQ(at_2500.size(), 2); // Cook + iPhone
    bool found_cook = false;
    for (auto &[dst, rel] : at_2500) {
      if (dst == "Tim_Cook")
        found_cook = true;
    }
    EXPECT_TRUE(found_cook);
  }
}

TEST_F(CSRGraphTest, KHopTraversal) {
  {
    builder::GraphBuilder builder(test_dir);
    builder.add_edge("A", "B", "r1");
    builder.add_edge("B", "C", "r2");
    builder.add_edge("C", "D", "r3");
    builder.add_edge("A", "E", "r4");
    ASSERT_TRUE(builder.build());
  }

  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    // 1-hop from A: sees B, E
    auto sub1 = graph.k_hop("A", 1);
    EXPECT_EQ(sub1.triples.size(), 2);
    EXPECT_EQ(sub1.node_ids.size(), 3); // A, B, E

    // 2-hop from A: sees B→C too
    auto sub2 = graph.k_hop("A", 2);
    EXPECT_EQ(sub2.triples.size(), 3); // A→B, A→E, B→C
    EXPECT_EQ(sub2.node_ids.size(), 4); // A, B, C, E

    // 3-hop from A: full graph reachable
    auto sub3 = graph.k_hop("A", 3);
    EXPECT_EQ(sub3.triples.size(), 4); // + C→D
    EXPECT_EQ(sub3.node_ids.size(), 5); // A, B, C, D, E

    // Non-existent node
    auto sub_fake = graph.k_hop("Z", 2);
    EXPECT_TRUE(sub_fake.triples.empty());
  }
}

TEST_F(CSRGraphTest, KHopWithTemporalFilter) {
  {
    builder::GraphBuilder builder(test_dir);
    builder.add_edge("X", "Y", "active", 100, 200);
    builder.add_edge("X", "Z", "permanent");
    builder.add_edge("Y", "W", "link");
    ASSERT_TRUE(builder.build());
  }

  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    // ts=150: X→Y 有效, X→Z 有效, Y→W 有效
    auto sub = graph.k_hop("X", 2, 150);
    EXPECT_EQ(sub.triples.size(), 3);

    // ts=300: X→Y 已过期, 只有 X→Z
    auto sub2 = graph.k_hop("X", 2, 300);
    EXPECT_EQ(sub2.triples.size(), 1);
    EXPECT_EQ(sub2.triples[0].dst, "Z");
  }
}

TEST_F(CSRGraphTest, IncomingNeighborsCSC) {
  {
    builder::GraphBuilder builder(test_dir);
    builder.add_edge("A", "C", "r1");
    builder.add_edge("B", "C", "r2");
    ASSERT_TRUE(builder.build());
  }

  {
    core::ImmutableGraph graph(test_dir);
    ASSERT_TRUE(graph.load());

    auto incoming = graph.get_incoming_neighbors("C");
    EXPECT_EQ(incoming.size(), 2);
  }
}
