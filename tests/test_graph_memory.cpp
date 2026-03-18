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
      graph.add_node(memory::GraphNode{.id = "EntityA", .type = "Person", .content = ""});
  auto n2 =
      graph.add_node(memory::GraphNode{.id = "EntityB", .type = "Location", .content = ""});

  EXPECT_TRUE(n1.has_value());
  EXPECT_TRUE(n2.has_value());

  // Test edge addition
  auto e1 = graph.add_edge(memory::GraphEdge{
      .source_id = "EntityA", .target_id = "EntityB", .relation = "visited"});
  EXPECT_TRUE(e1.has_value());

  // Test edge retrieval
  auto edges = graph.get_edges_by_relation("EntityA", "visited");
  ASSERT_TRUE(edges.has_value());
  ASSERT_EQ(edges->size(), 1u);
  EXPECT_EQ(edges.value()[0].target_id, "EntityB");

  // Implicit node creation
  auto e2 = graph.add_edge(memory::GraphEdge{.source_id = "EntityC",
                                             .target_id = "EntityA",
                                             .relation = "friends_with"});
  EXPECT_TRUE(e2.has_value());

  auto edges_c = graph.get_edges("EntityC");
  ASSERT_TRUE(edges_c.has_value());
  EXPECT_EQ(edges_c->size(), 1u);
}

TEST(GraphMemoryTest, KHopSearch) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_khop";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "Alice", .target_id = "Bob", .relation = "knows"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "Bob", .target_id = "Charlie", .relation = "knows"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "Charlie", .target_id = "NewYork", .relation = "lives_in"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "Alice", .target_id = "IceCream", .relation = "likes"});

  // 1-hop search
  auto res1 = graph.k_hop_search("Alice", 1);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(res1->nodes.size(), 3u); // Alice, Bob, IceCream
  EXPECT_EQ(res1->edges.size(), 2u);

  // 2-hop search
  auto res2 = graph.k_hop_search("Alice", 2);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2->nodes.size(), 4u); // Alice, Bob, IceCream, Charlie
  EXPECT_EQ(res2->edges.size(), 3u);

  // 3-hop search
  auto res3 = graph.k_hop_search("Alice", 3);
  ASSERT_TRUE(res3.has_value());
  EXPECT_EQ(res3->nodes.size(), 5u); // Alice, Bob, IceCream, Charlie, NewYork
  EXPECT_EQ(res3->edges.size(), 4u);
}

TEST(GraphMemoryTest, WALPersistence) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_wal";
  std::filesystem::remove_all(test_dir);

  {
    // Write to graph
    memory::LocalGraphMemory graph(test_dir);
    (void)graph.add_node(memory::GraphNode{.id = "Dog", .type = "Animal", .content = ""});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "Dog", .target_id = "Cat", .relation = "chases"});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "Cat", .target_id = "Mouse", .relation = "eats"});
  } // Graph goes out of scope, WAL flushed

  {
    // Re-open graph, data should be restored from WAL
    memory::LocalGraphMemory graph(test_dir);

    auto edges_dog = graph.get_edges("Dog");
    ASSERT_TRUE(edges_dog.has_value());
    ASSERT_EQ(edges_dog->size(), 1u);
    EXPECT_EQ(edges_dog.value()[0].relation, "chases");
    EXPECT_EQ(edges_dog.value()[0].target_id, "Cat");

    auto edges_cat = graph.get_edges("Cat");
    ASSERT_TRUE(edges_cat.has_value());
    ASSERT_EQ(edges_cat->size(), 1u);
    EXPECT_EQ(edges_cat.value()[0].relation, "eats");
    EXPECT_EQ(edges_cat.value()[0].target_id, "Mouse");

    // 2-hop search should work on restored data
    auto res = graph.k_hop_search("Dog", 2);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->edges.size(), 2u);
  }
}

TEST(GraphMemoryTest, WALReplayDeduplicatesEdges) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_dedup";
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    // 添加同一条边两次（不同 weight）
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r1", .weight = 1.0f});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r1", .weight = 2.0f});
    // 不同 relation 的边应保留
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "r2", .weight = 3.0f});

    auto edges = graph.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 2u); // r1（覆盖后1条）+ r2（1条）
    // r1 应被覆盖为 weight=2.0
    for (const auto &e : *edges) {
      if (e.relation == "r1") {
        EXPECT_FLOAT_EQ(e.weight, 2.0f);
      }
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
  (void)graph.add_node(memory::GraphNode{.id = "X", .type = "Person", .content = "old"});

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
    ASSERT_EQ(edges->nodes.size(), 1u);
    EXPECT_EQ(edges->nodes[0].content, "newer");
    EXPECT_EQ(edges->nodes[0].type, "Company");
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, DeleteNode) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_delnode";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r1"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "B", .target_id = "C", .relation = "r2"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "C", .target_id = "B", .relation = "r3"});

  // Delete B — should remove outgoing edges from B and incoming edges to B
  auto r = graph.delete_node("B");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(*r);

  // A's edges to B should be gone
  auto edges_a = graph.get_edges("A");
  ASSERT_TRUE(edges_a.has_value());
  EXPECT_EQ(edges_a->size(), 0u);

  // B's outgoing edges should be gone
  auto edges_b = graph.get_edges("B");
  ASSERT_TRUE(edges_b.has_value());
  EXPECT_EQ(edges_b->size(), 0u);

  // C's edge to B should be gone
  auto edges_c = graph.get_edges("C");
  ASSERT_TRUE(edges_c.has_value());
  EXPECT_EQ(edges_c->size(), 0u);

  // Non-existent returns false
  auto r2 = graph.delete_node("NonExistent");
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(*r2);

  // WAL persistence
  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges = graph2.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 0u);
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, DeleteEdge) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_deledge";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r1"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r2"});

  // Delete one edge
  auto r = graph.delete_edge("A", "B", "r1");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(*r);

  auto edges = graph.get_edges("A");
  ASSERT_TRUE(edges.has_value());
  ASSERT_EQ(edges->size(), 1u);
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
    ASSERT_EQ(edges2->size(), 1u);
    EXPECT_EQ(edges2.value()[0].relation, "r2");
  }
  std::filesystem::remove_all(test_dir);
}

TEST(GraphMemoryTest, CleanupBefore) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_cleanup";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  // Edge with end_ts=100 (expired)
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "old",
      .weight = 1.0f, .start_ts = 10, .end_ts = 100});
  // Edge with end_ts=UINT64_MAX (indefinite)
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "C", .relation = "current"});

  auto removed = graph.cleanup_before(200);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 1u);

  auto edges = graph.get_edges("A");
  ASSERT_TRUE(edges.has_value());
  EXPECT_EQ(edges->size(), 1u);
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
  EXPECT_EQ(subgraph->edges.size(), 2u);

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

// ── Coverage boost tests ────────────────────────────────

// Test corrupt WAL line (bad CRC) is skipped during replay
TEST(GraphMemoryTest, CorruptWALLineSkipped) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_corrupt_wal";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  // Write a WAL with one valid and one corrupted line
  {
    std::ofstream wal(test_dir / "graph_wal.log");
    // Valid node line (compute correct CRC manually via LocalGraphMemory internals)
    // We'll write a line with correct CRC, and one with wrong CRC
    wal << "N,GoodNode,Person,,12345|CRC:99999999\n"; // wrong CRC
    wal << "N,ValidNode,Company,,67890|CRC:0\n";       // also wrong CRC
    wal.close();
  }

  // Load — both lines should be skipped due to CRC mismatch
  memory::LocalGraphMemory graph(test_dir);
  auto edges = graph.get_edges("GoodNode");
  ASSERT_TRUE(edges.has_value());
  EXPECT_EQ(edges->size(), 0u);

  // After loading corrupt WAL, graph should still be functional
  auto r = graph.add_node(memory::GraphNode{.id = "Fresh", .type = "Test"});
  EXPECT_TRUE(r.has_value());

  std::filesystem::remove_all(test_dir);
}

// Test WAL with legacy format (no CRC suffix) is accepted
TEST(GraphMemoryTest, LegacyWALFormatAccepted) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_legacy_wal";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  // Write legacy WAL without CRC
  {
    std::ofstream wal(test_dir / "graph_wal.log");
    wal << "N,LegacyNode,Person,,12345\n"; // no |CRC: suffix
    wal.close();
  }

  memory::LocalGraphMemory graph(test_dir);
  // Legacy line should be accepted
  auto res = graph.k_hop_search("LegacyNode", 0);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->nodes.size(), 1u);
  EXPECT_EQ(res->nodes[0].id, "LegacyNode");

  std::filesystem::remove_all(test_dir);
}

// Test k_hop_search with nonexistent start node returns error
TEST(GraphMemoryTest, KHopSearchNonexistentNode) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_khop_missing";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  auto res = graph.k_hop_search("NonExistent", 1);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrorCode::NotFound);

  std::filesystem::remove_all(test_dir);
}

// Test k_hop_search with temporal filtering
TEST(GraphMemoryTest, KHopTemporalFilter) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_khop_temporal";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  // Edge valid from ts=100 to ts=200
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "r1",
      .weight = 1.0f, .start_ts = 100, .end_ts = 200});
  // Edge valid from ts=300 to indefinite
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "C", .relation = "r2",
      .weight = 1.0f, .start_ts = 300, .end_ts = UINT64_MAX});

  // Query at ts=150: should only see A->B
  auto res1 = graph.k_hop_search("A", 1, 150);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(res1->edges.size(), 1u);
  EXPECT_EQ(res1->edges[0].target_id, "B");

  // Query at ts=400: should only see A->C
  auto res2 = graph.k_hop_search("A", 1, 400);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2->edges.size(), 1u);
  EXPECT_EQ(res2->edges[0].target_id, "C");

  std::filesystem::remove_all(test_dir);
}

// Test save_snapshot produces a compacted WAL
TEST(GraphMemoryTest, SaveSnapshotCompactsWAL) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_snapshot";
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    (void)graph.add_node(memory::GraphNode{.id = "N1", .type = "T1"});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "N1", .target_id = "N2", .relation = "rel"});

    graph.save_snapshot();
  }

  // Verify data survives snapshot compaction
  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges = graph2.get_edges("N1");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);
    EXPECT_EQ(edges.value()[0].target_id, "N2");
  }

  std::filesystem::remove_all(test_dir);
}

// Test cleanup_before with no expired edges
TEST(GraphMemoryTest, CleanupBeforeNoExpired) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_cleanup_none";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  // All edges indefinite (end_ts = UINT64_MAX)
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "X", .target_id = "Y", .relation = "r1"});

  auto removed = graph.cleanup_before(999999);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(*removed, 0u);

  auto edges = graph.get_edges("X");
  ASSERT_TRUE(edges.has_value());
  EXPECT_EQ(edges->size(), 1u);

  std::filesystem::remove_all(test_dir);
}

// Test cleanup_before persists after WAL reload
TEST(GraphMemoryTest, CleanupBeforePersistsAfterReload) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_cleanup_persist";
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "B", .relation = "expired",
        .weight = 1.0f, .start_ts = 10, .end_ts = 50});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "A", .target_id = "C", .relation = "active"});
    auto removed = graph.cleanup_before(100);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 1u);
    graph.save_snapshot(); // compact WAL
  }

  {
    memory::LocalGraphMemory graph2(test_dir);
    auto edges = graph2.get_edges("A");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);
    EXPECT_EQ(edges.value()[0].relation, "active");
  }

  std::filesystem::remove_all(test_dir);
}

// Test escape/unescape roundtrip with special characters
TEST(GraphMemoryTest, EscapeSpecialCharsInWAL) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_escape";
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    // Node with commas, newlines, backslashes in content
    (void)graph.add_node(memory::GraphNode{
        .id = "special", .type = "Type", .content = "a,b\\c\nd"});
  }

  {
    memory::LocalGraphMemory graph2(test_dir);
    auto res = graph2.k_hop_search("special", 0);
    ASSERT_TRUE(res.has_value());
    ASSERT_EQ(res->nodes.size(), 1u);
    EXPECT_EQ(res->nodes[0].content, "a,b\\c\nd");
  }

  std::filesystem::remove_all(test_dir);
}

// Test pending WAL file is cleaned up on load
TEST(GraphMemoryTest, PendingWALCleanedUp) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_pending";
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);

  // Create a pending file (simulating interrupted write)
  {
    std::ofstream pending(test_dir / "graph_wal.log.pending");
    pending << "N,Ghost,Phantom,,0\n";
    pending.close();
    // Also create a main WAL
    std::ofstream wal(test_dir / "graph_wal.log");
    wal.close();
  }

  // Loading should remove the pending file
  memory::LocalGraphMemory graph(test_dir);
  EXPECT_FALSE(std::filesystem::exists(test_dir / "graph_wal.log.pending"));

  // Ghost node should NOT be in graph (pending was discarded)
  auto res = graph.k_hop_search("Ghost", 0);
  EXPECT_FALSE(res.has_value()); // not found

  std::filesystem::remove_all(test_dir);
}

// Test get_edges_by_relation with multiple relations
TEST(GraphMemoryTest, GetEdgesByRelationFilters) {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_edges_rel";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "B", .relation = "likes"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "C", .relation = "hates"});
  (void)graph.add_edge(memory::GraphEdge{
      .source_id = "A", .target_id = "D", .relation = "likes"});

  auto likes = graph.get_edges_by_relation("A", "likes");
  ASSERT_TRUE(likes.has_value());
  EXPECT_EQ(likes->size(), 2u);

  auto hates = graph.get_edges_by_relation("A", "hates");
  ASSERT_TRUE(hates.has_value());
  EXPECT_EQ(hates->size(), 1u);

  // Non-existent relation
  auto none = graph.get_edges_by_relation("A", "unknown");
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(none->size(), 0u);

  // Non-existent node
  auto missing = graph.get_edges_by_relation("Z", "likes");
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ(missing->size(), 0u);

  std::filesystem::remove_all(test_dir);
}
