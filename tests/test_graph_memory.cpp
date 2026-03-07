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

TEST(GraphMemoryTest, WALReplayDeduplicatesEdges) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_dedup";
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    // 添加同一条边两次（不同 weight）
    graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r1", .weight = 1.0f});
    graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r1", .weight = 2.0f});
    // 不同 relation 的边应保留
    graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r2", .weight = 3.0f});

    auto edges = graph.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 2u); // r1（覆盖后1条）+ r2（1条）
    // r1 应被覆盖为 weight=2.0
    for (const auto &e : *edges) {
      if (e.relation == "r1")
        EXPECT_FLOAT_EQ(e.weight, 2.0f);
    }
  }

  {
    // 重新从 WAL 加载，确认去重生效
    memory::LocalGraphMemory graph(test_dir);
    auto edges = graph.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 2u); // WAL 去重后仍为 2 条
  }

  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, UpdateNode) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_update";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  graph.add_node(memory::GraphNode{.id = "X", .type = "Person", .content = "old"});

  // Update content only
  auto r1 = graph.update_node("X", "new content");
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);

  // Update content and type
  auto r2 = graph.update_node("X", "newer", "Company");
  ASSERT_TRUE(r2.has_value());
  EXPECT_TRUE(*r2);

  // Non-existent node returns false
  auto r3 = graph.update_node("NonExistent", "data");
  ASSERT_TRUE(r3.has_value());
  EXPECT_FALSE(*r3);

  // Verify via WAL persistence
  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges = graph2.k_hop_search("X", 0);
    ASSERT_TRUE(edges.has_value());
    ASSERT_EQ(edges->nodes.size(), 1);
    EXPECT_EQ(edges->nodes[0].content, "newer");
    EXPECT_EQ(edges->nodes[0].type, "Company");
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, DeleteNode) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_delnode";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r1"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "B", .target_id = "C", .relation = "r2"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "C", .target_id = "B", .relation = "r3"});

  // Delete B — should remove outgoing edges from B and incoming edges to B
  auto r = graph.delete_node("B");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(*r);

  // A's edges to B should be gone
  auto edges_a = graph.get_edges("A");
  ASSERT_TRUE(edges_a.has_value());
  EXPECT_EQ(edges_a->size(), 0);

  // B's outgoing edges should be gone
  auto edges_b = graph.get_edges("B");
  ASSERT_TRUE(edges_b.has_value());
  EXPECT_EQ(edges_b->size(), 0);

  // C's edge to B should be gone
  auto edges_c = graph.get_edges("C");
  ASSERT_TRUE(edges_c.has_value());
  EXPECT_EQ(edges_c->size(), 0);

  // Non-existent returns false
  auto r2 = graph.delete_node("NonExistent");
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(*r2);

  // WAL persistence
  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges = graph2.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 0);
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, DeleteEdge) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_deledge";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r1"});
  graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r2"});

  // Delete one edge
  auto r = graph.delete_edge("A", "B", "r1");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(*r);

  auto edges = graph.get_edges("A");
  ASSERT_TRUE(edges.has_value());
  ASSERT_EQ(edges->size(), 1);
  EXPECT_EQ(edges.value()[0].relation, "r2");

  // Non-existent edge
  auto r2 = graph.delete_edge("A", "B", "r_nonexistent");
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(*r2);

  // WAL persistence
  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges2 = graph2.get_edges("A");
    ASSERT_TRUE(edges2.has_value());
    ASSERT_EQ(edges2->size(), 1);
    EXPECT_EQ(edges2.value()[0].relation, "r2");
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, CleanupBefore) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_cleanup";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  // Edge with end_ts=100 (expired)
  graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "old",
      .weight = 1.0f, .start_ts = 10, .end_ts = 100});
  // Edge with end_ts=UINT64_MAX (indefinite)
  graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "C", .relation = "current"});

  auto removed = graph.cleanup_before(200);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 1);

  auto edges = graph.get_edges("A");
  ASSERT_TRUE(edges.has_value());
  EXPECT_EQ(edges->size(), 1);
  EXPECT_EQ(edges.value()[0].relation, "current");

  std::filesystem::remove_all(test_dir);
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
