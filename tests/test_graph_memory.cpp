// ============================================================
// AgentOS Graph Memory Tests
// ============================================================
#include <agentos/memory/graph_memory.hpp>
#include <agentos/memory/memory.hpp>
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace agentos;

void run_test(std::string_view name, std::function<void()> test_func) {
  std::cout << "[RUN] " << name << "...\n";
  try {
    test_func();
    std::cout << "\033[32m[OK] \033[0m" << name << "\n";
  } catch (const std::exception &e) {
    std::cout << "\033[31m[FAILED] \033[0m" << name << ": " << e.what() << "\n";
    exit(1);
  }
}

void test_basic_graph_operations() {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_basic";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  // Test node addition
  auto n1 =
      graph.add_node(memory::GraphNode{.id = "EntityA", .type = "Person"});
  auto n2 =
      graph.add_node(memory::GraphNode{.id = "EntityB", .type = "Location"});

  assert(n1.has_value());
  assert(n2.has_value());

  // Test edge addition
  auto e1 = graph.add_edge(memory::GraphEdge{
      .source_id = "EntityA", .target_id = "EntityB", .relation = "visited"});
  assert(e1.has_value());

  // Test edge retrieval
  auto edges = graph.get_edges_by_relation("EntityA", "visited");
  assert(edges.has_value() && edges->size() == 1);
  assert(edges.value()[0].target_id == "EntityB");

  // Implicit node creation
  auto e2 = graph.add_edge(memory::GraphEdge{.source_id = "EntityC",
                                             .target_id = "EntityA",
                                             .relation = "friends_with"});
  assert(e2.has_value());

  auto edges_c = graph.get_edges("EntityC");
  assert(edges_c.has_value() && edges_c->size() == 1);
}

void test_k_hop_search() {
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
  assert(res1.has_value());
  assert(res1->nodes.size() == 3); // Alice, Bob, IceCream
  assert(res1->edges.size() == 2);

  // 2-hop search
  auto res2 = graph.k_hop_search("Alice", 2);
  assert(res2.has_value());
  assert(res2->nodes.size() == 4); // Alice, Bob, IceCream, Charlie
  assert(res2->edges.size() == 3);

  // 3-hop search
  auto res3 = graph.k_hop_search("Alice", 3);
  assert(res3.has_value());
  assert(res3->nodes.size() == 5); // Alice, Bob, IceCream, Charlie, NewYork
  assert(res3->edges.size() == 4);
}

void test_wal_persistence() {
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
    assert(edges_dog.has_value() && edges_dog->size() == 1);
    assert(edges_dog.value()[0].relation == "chases");
    assert(edges_dog.value()[0].target_id == "Cat");

    auto edges_cat = graph.get_edges("Cat");
    assert(edges_cat.has_value() && edges_cat->size() == 1);
    assert(edges_cat.value()[0].relation == "eats");
    assert(edges_cat.value()[0].target_id == "Mouse");

    // 2-hop search should work on restored data
    auto res = graph.k_hop_search("Dog", 2);
    assert(res.has_value() && res->edges.size() == 2);
  }
}

void test_high_level_memory_api() {
  std::filesystem::path test_dir = "/tmp/agentos_test_graph_high_level";
  std::filesystem::remove_all(test_dir);

  memory::MemorySystem memory(test_dir);

  auto r1 = memory.add_triplet("CompanyX", "employs", "EmployeeY");
  assert(r1.has_value());

  auto r2 = memory.add_triplet("EmployeeY", "works_on", "ProjectZ");
  assert(r2.has_value());

  auto subgraph = memory.query_graph("CompanyX", 2);
  assert(subgraph.has_value());
  assert(subgraph->edges.size() == 2);

  bool found_employs = false;
  bool found_works_on = false;

  for (const auto &edge : subgraph->edges) {
    if (edge.relation == "employs")
      found_employs = true;
    if (edge.relation == "works_on")
      found_works_on = true;
  }

  assert(found_employs && found_works_on);
}

int main() {
  std::cout << "Starting Graph Memory Tests...\n";

  run_test("Basic Graph Operations", test_basic_graph_operations);
  run_test("k-hop Search", test_k_hop_search);
  run_test("WAL Persistence", test_wal_persistence);
  run_test("High-Level MemorySystem API", test_high_level_memory_api);

  std::cout << "\n\033[1;32mAll Graph Memory Tests Passed!\033[0m\n";
  return 0;
}
