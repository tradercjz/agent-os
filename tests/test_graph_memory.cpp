// ============================================================
// AgentOS Graph Memory Tests
// ============================================================
#include <agentos/memory/graph_memory.hpp>
#include <agentos/memory/memory.hpp>
#include <filesystem>
#include <gtest/gtest.h>

using namespace agentos;

TEST(GraphMemoryTest, BasicGraphOperations) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_basic";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  // Test node addition
  auto n1 =
      graph.add_node(memory::GraphNode{.id = "EntityA", .type = "Person"});
  auto n2 =
      graph.add_node(memory::GraphNode{.id = "EntityB", .type = "Location"});

  EXPECT_TRUE(n1.has_value());
  EXPECT_TRUE(n2.has_value());

  // Test edge addition
  auto e1 = graph.add_edge(memory::GraphEdge{
      .source_id = "EntityA", .target_id = "EntityB", .relation = "visited"});
  EXPECT_TRUE(e1.has_value());

  // Test edge retrieval
  auto edges = graph.get_edges_by_relation("EntityA", "visited");
  ASSERT_TRUE(edges.has_value());
  ASSERT_EQ(edges->size(), 1);
  EXPECT_EQ(edges.value()[0].target_id, "EntityB");

  // Implicit node creation
  auto e2 = graph.add_edge(memory::GraphEdge{.source_id = "EntityC",
                                             .target_id = "EntityA",
                                             .relation = "friends_with"});
  EXPECT_TRUE(e2.has_value());

  auto edges_c = graph.get_edges("EntityC");
  ASSERT_TRUE(edges_c.has_value());
  EXPECT_EQ(edges_c->size(), 1);
}

TEST(GraphMemoryTest, KHopSearch) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_khop";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  graph.add_edge(memory::GraphEdge{
      .source_id = "Alice", .target_id = "Bob", .relation = "knows"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "Bob", .target_id = "Charlie", .relation = "knows"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "Charlie", .target_id = "NewYork", .relation = "lives_in"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "Alice", .target_id = "IceCream", .relation = "likes"});

  // 1-hop search
  auto res1 = graph.k_hop_search("Alice", 1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(res1->nodes.size(), 3); // Alice, Bob, IceCream
  EXPECT_EQ(res1->edges.size(), 2);

  // 2-hop search
  auto res2 = graph.k_hop_search("Alice", 2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2->nodes.size(), 4); // Alice, Bob, IceCream, Charlie
  EXPECT_EQ(res2->edges.size(), 3);

  // 3-hop search
  auto res3 = graph.k_hop_search("Alice", 3);
  ASSERT_TRUE(res3.has_value());
  EXPECT_EQ(res3->nodes.size(), 5); // Alice, Bob, IceCream, Charlie, NewYork
  EXPECT_EQ(res3->edges.size(), 4);
}

TEST(GraphMemoryTest, WALPersistence) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_wal";
  std::filesystem::remove_all(test_dir);

  {
    // Write to graph
    memory::LocalGraphMemory graph(test_dir);
    graph.add_node(memory::GraphNode{.id = "Dog", .type = "Animal"});
    graph.add_edge(memory::GraphEdge{
        .source_id = "Dog", .target_id = "Cat", .relation = "chases"});
    graph.add_edge(memory::GraphEdge{
        .source_id = "Cat", .target_id = "Mouse", .relation = "eats"});
  } // Graph goes out of scope, WAL flushed

  {
    // Re-open graph, data should be restored from WAL
    memory::LocalGraphMemory graph(test_dir);

    auto edges_dog = graph.get_edges("Dog");
    ASSERT_TRUE(edges_dog.has_value());
    ASSERT_EQ(edges_dog->size(), 1);
    EXPECT_EQ(edges_dog.value()[0].relation, "chases");
    EXPECT_EQ(edges_dog.value()[0].target_id, "Cat");

    auto edges_cat = graph.get_edges("Cat");
    ASSERT_TRUE(edges_cat.has_value());
    ASSERT_EQ(edges_cat->size(), 1);
    EXPECT_EQ(edges_cat.value()[0].relation, "eats");
    EXPECT_EQ(edges_cat.value()[0].target_id, "Mouse");

    // 2-hop search should work on restored data
    auto res = graph.k_hop_search("Dog", 2);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->edges.size(), 2);
  }
}

TEST(GraphMemoryTest, HighLevelMemoryAPI) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_high_level";
  std::filesystem::remove_all(test_dir);

  memory::MemorySystem memory(test_dir);

  auto r1 = memory.add_triplet("CompanyX", "employs", "EmployeeY");
  EXPECT_TRUE(r1.has_value());

  auto r2 = memory.add_triplet("EmployeeY", "works_on", "ProjectZ");
  EXPECT_TRUE(r2.has_value());

  auto subgraph = memory.query_graph("CompanyX", 2);
  ASSERT_TRUE(subgraph.has_value());
  EXPECT_EQ(subgraph->edges.size(), 2);

  bool found_employs = false;
  bool found_works_on = false;

  for (const auto &edge : subgraph->edges) {
    if (edge.relation == "employs")
      found_employs = true;
    if (edge.relation == "works_on")
      found_works_on = true;
  }

  EXPECT_TRUE(found_employs);
  EXPECT_TRUE(found_works_on);
}
