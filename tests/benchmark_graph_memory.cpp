// ============================================================
// AgentOS Graph Memory Benchmark
// ============================================================
#include <agentos/memory/graph_memory.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <vector>

using namespace agentos;

// Simple stopwatch for benchmarking
class Stopwatch {
public:
  Stopwatch() : start_time_(std::chrono::high_resolution_clock::now()) {}

  void reset() { start_time_ = std::chrono::high_resolution_clock::now(); }

  double elapsed_ms() const {
    auto end_time = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end_time - start_time_)
        .count();
  }

private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
};

int main() {
  std::cout << "==========================================\n";
  std::cout << "  AgentOS Graph Memory Benchmark\n";
  std::cout << "==========================================\n";

  std::filesystem::path test_dir = "/tmp/agentos_benchmark_graph";
  std::filesystem::remove_all(test_dir);

  memory::LocalGraphMemory graph(test_dir);

  const int NUM_NODES = 10000;
  const int NUM_EDGES = 50000;

  std::cout << "Generating random graph with " << NUM_NODES << " nodes and "
            << NUM_EDGES << " edges...\n";

  std::vector<std::string> node_ids;
  node_ids.reserve(NUM_NODES);
  for (int i = 0; i < NUM_NODES; ++i) {
    node_ids.push_back("Node_" + std::to_string(i));
  }

  Stopwatch sw;

  // Benchmark Node Insertion
  for (int i = 0; i < NUM_NODES; ++i) {
    graph.add_node(
        memory::GraphNode{.id = node_ids[i], .type = "Entity", .content = ""});
  }
  double insert_nodes_time = sw.elapsed_ms();
  std::cout << "[Benchmark] Insert " << NUM_NODES
            << " nodes: " << insert_nodes_time << " ms ("
            << (NUM_NODES / (insert_nodes_time / 1000.0)) << " ops/sec)\n";

  // Connect nodes pseudo-randomly
  std::mt19937 rng(42); // Fixed seed for reproducibility
  std::uniform_int_distribution<int> node_dist(0, NUM_NODES - 1);

  std::vector<std::string> relations = {"knows", "likes", "follows",
                                        "created_by"};
  std::uniform_int_distribution<int> rel_dist(0, relations.size() - 1);

  sw.reset();
  for (int i = 0; i < NUM_EDGES; ++i) {
    int src = node_dist(rng);
    int dst = node_dist(rng);
    if (src == dst)
      continue; // Skip self loops for simplicity

    graph.add_edge(memory::GraphEdge{.source_id = node_ids[src],
                                     .target_id = node_ids[dst],
                                     .relation = relations[rel_dist(rng)],
                                     .weight = 1.0f,
                                     .start_ts = 0,
                                     .end_ts = 0});
  }
  double insert_edges_time = sw.elapsed_ms();
  std::cout << "[Benchmark] Insert " << NUM_EDGES
            << " edges: " << insert_edges_time << " ms ("
            << (NUM_EDGES / (insert_edges_time / 1000.0)) << " ops/sec)\n";

  // Force a snapshot to benchmark compaction
  sw.reset();
  graph.save_snapshot();
  double snapshot_time = sw.elapsed_ms();
  std::cout << "[Benchmark] Save Snapshot (Compact WAL): " << snapshot_time
            << " ms\n";

  // Find a dense node for realistic k-hop search testing
  // node_0 will likely have average connections, let's just pick a few for
  // average
  std::cout << "\n--- Retrieval Performance ---\n";

  struct SearchTarget {
    std::string id;
    std::string description;
  };
  std::vector<SearchTarget> targets = {{"Node_0", "Average Node (0)"},
                                       {"Node_500", "Average Node (500)"},
                                       {"Node_1000", "Average Node (1000)"}};

  for (int k = 1; k <= 3; ++k) {
    double total_time = 0;
    size_t total_nodes_found = 0;
    size_t total_edges_found = 0;

    for (const auto &target : targets) {
      sw.reset();
      auto res = graph.k_hop_search(target.id, k);
      total_time += sw.elapsed_ms();

      if (res) {
        total_nodes_found += res->nodes.size();
        total_edges_found += res->edges.size();
      }
    }

    double avg_time = total_time / targets.size();
    size_t avg_nodes = total_nodes_found / targets.size();
    size_t avg_edges = total_edges_found / targets.size();

    std::cout << "[Benchmark] " << k << "-Hop Search average: " << avg_time
              << " ms\n";
    std::cout << "            -> Avg Nodes retrieved: " << avg_nodes
              << " | Avg Edges retrieved: " << avg_edges << "\n";
  }

  std::cout << "==========================================\n";
  return 0;
}
