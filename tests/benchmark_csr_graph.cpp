#include "graph_engine/builder/graph_builder.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>

using namespace graph_engine;
namespace fs = std::filesystem;

// A simple utility to format time elapsed
void print_timing(
    const std::string &prefix,
    std::chrono::time_point<std::chrono::high_resolution_clock> start,
    std::chrono::time_point<std::chrono::high_resolution_clock> end) {
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  .count();
  std::cout << "[Benchmark] " << prefix << ": " << diff << " ms\n";
}

int main() {
  const int NUM_NODES = 1'000'000;
  const int NUM_EDGES = 10'000'000;
  fs::path test_dir = "/tmp/agentos_csr_benchmark";

  if (fs::exists(test_dir)) {
    fs::remove_all(test_dir);
  }

  std::cout << "==========================================\n";
  std::cout << "  AgentOS CSR Graph Memory Benchmark\n";
  std::cout << "==========================================\n";

  // ---------------------------------------------------------
  // 1. Offline Graph Assimilation Phase (Building)
  // ---------------------------------------------------------

  std::cout << "Generating random graph with " << NUM_NODES << " nodes and "
            << NUM_EDGES << " edges...\n";

  auto start_build = std::chrono::high_resolution_clock::now();

  builder::GraphBuilder builder(test_dir);

  std::mt19937 gen(42); // fixed seed for reproducibility
  std::uniform_int_distribution<int> node_dist(0, NUM_NODES - 1);
  std::uniform_int_distribution<int> rel_dist(0, 10); // 11 types of relations

  for (int i = 0; i < NUM_EDGES; ++i) {
    int src = node_dist(gen);
    int dst = node_dist(gen);
    int rel = rel_dist(gen);
    builder.add_edge("Node_" + std::to_string(src),
                     "Node_" + std::to_string(dst),
                     "Relation_" + std::to_string(rel));
  }

  auto end_gen = std::chrono::high_resolution_clock::now();
  print_timing("Generate Strings & Raw Edge Additions", start_build, end_gen);

  auto start_compact = std::chrono::high_resolution_clock::now();
  bool built = builder.build();
  if (!built) {
    std::cerr << "Failed to build graph!\n";
    return 1;
  }
  auto end_compact = std::chrono::high_resolution_clock::now();
  print_timing("Sort & Compact into CSR Files", start_compact, end_compact);

  // ---------------------------------------------------------
  // 2. Online Graph Mmap Load Phase (Zero-Copy)
  // ---------------------------------------------------------

  std::cout << "\n--- Online Retrieval Performance ---\n";
  auto start_load = std::chrono::high_resolution_clock::now();

  core::ImmutableGraph graph(test_dir);
  bool loaded = graph.load();
  if (!loaded) {
    std::cerr << "Failed to mmap load graph!\n";
    return 1;
  }

  auto end_load = std::chrono::high_resolution_clock::now();
  auto load_diff = std::chrono::duration_cast<std::chrono::microseconds>(
                       end_load - start_load)
                       .count();
  std::cout << "[Benchmark] mmap() Load 1M Nodes / 10M Edges CSR Graph: "
            << load_diff / 1000.0 << " ms\n";

  // ---------------------------------------------------------
  // 3. Online Zero-Copy Neighborhood Traversal (Simulation)
  // ---------------------------------------------------------

  int num_queries = 10'000;
  uint64_t total_edges_visited = 0;
  auto start_query = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < num_queries; ++i) {
    int src = node_dist(gen);
    // get internal ID for src
    EntityID src_id;
    if (graph.get_entity_dict().find_id("Node_" + std::to_string(src),
                                        src_id)) {
      auto [edges, count] = graph.get_raw_neighbors(src_id);
      total_edges_visited += count;
      // In a real K-hop, we would push edge.target to a queue here.
    }
  }

  auto end_query = std::chrono::high_resolution_clock::now();
  auto query_diff_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           end_query - start_query)
                           .count();

  std::cout << "[Benchmark] 1-Hop Raw Adjacency Read (" << num_queries
            << " random queries): " << query_diff_us / 1000.0 << " ms Total\n";
  std::cout << "[Benchmark] Average latency per node lookup: "
            << static_cast<double>(query_diff_us) / num_queries << " us\n";
  std::cout << "[Benchmark] Total edges accessed: " << total_edges_visited
            << "\n";
  std::cout << "==========================================\n";

  return 0;
}
