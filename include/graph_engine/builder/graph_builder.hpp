#pragma once

#include "graph_engine/core/types.hpp"
#include "graph_engine/utils/string_dict.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace graph_engine {
namespace builder {

namespace fs = std::filesystem;

class GraphBuilder {
public:
  GraphBuilder(fs::path output_dir) : output_dir_(std::move(output_dir)) {
    fs::create_directories(output_dir_);
  }

  // Add a triplet to the graph building queue
  void add_edge(const std::string &src, const std::string &dst,
                const std::string &relation) {
    EntityID u = entity_dict_.get_or_insert(src);
    EntityID v = entity_dict_.get_or_insert(dst);
    RelationID r =
        static_cast<RelationID>(relation_dict_.get_or_insert(relation));

    raw_edges_.push_back({u, v, r});
  }

  // Build the underlying CSR arrays and flush everything to disk.
  bool build() {
    // 1. Sort the raw edges by source node ID
    std::sort(raw_edges_.begin(), raw_edges_.end(),
              [](const RawEdge &a, const RawEdge &b) {
                if (a.src != b.src)
                  return a.src < b.src;
                if (a.dst != b.dst)
                  return a.dst < b.dst;
                return a.rel < b.rel;
              });

    uint32_t num_nodes = static_cast<uint32_t>(entity_dict_.size());
    uint64_t num_edges = raw_edges_.size();

    std::vector<uint64_t> offsets(num_nodes + 1, 0);
    std::vector<Edge> edges;
    edges.reserve(num_edges);

    // 2. Build the CSR topology
    EntityID current_src = 0;
    for (size_t i = 0; i < num_edges; ++i) {
      const auto &current_edge = raw_edges_[i];

      // If we skipped some nodes, fill their offsets with the current edge
      // index
      while (current_src < current_edge.src) {
        current_src++;
        offsets[current_src] = i;
      }

      edges.push_back({current_edge.dst, current_edge.rel});
    }

    // Fill the remaining offsets up to num_nodes
    while (current_src < num_nodes) {
      current_src++;
      offsets[current_src] = num_edges; // tail offset
    }

    // --- CSC (Incoming Edges) Phase ---
    // 3. Sort the raw edges by destination node ID for CSC
    std::sort(raw_edges_.begin(), raw_edges_.end(),
              [](const RawEdge &a, const RawEdge &b) {
                if (a.dst != b.dst)
                  return a.dst < b.dst;
                if (a.src != b.src)
                  return a.src < b.src;
                return a.rel < b.rel;
              });

    std::vector<uint64_t> csc_offsets(num_nodes + 1, 0);
    std::vector<Edge> csc_edges;
    csc_edges.reserve(num_edges);

    EntityID current_dst = 0;
    for (size_t i = 0; i < num_edges; ++i) {
      const auto &current_edge = raw_edges_[i];

      while (current_dst < current_edge.dst) {
        current_dst++;
        csc_offsets[current_dst] = i;
      }

      // For CSC, the elements we store are the source of the edge
      csc_edges.push_back({current_edge.src, current_edge.rel});
    }

    while (current_dst < num_nodes) {
      current_dst++;
      csc_offsets[current_dst] = num_edges;
    }

    // 4. Serialize outputs

    // Fill the remaining offsets up to num_nodes
    while (current_src < num_nodes) {
      current_src++;
      offsets[current_src] = num_edges; // tail offset
    }

    // 4. Serialize outputs
    if (!save_array(output_dir_ / "graph.offsets", offsets))
      return false;
    if (!save_array(output_dir_ / "graph.edges", edges))
      return false;
    if (!save_array(output_dir_ / "graph.csc_offsets", csc_offsets))
      return false;
    if (!save_array(output_dir_ / "graph.csc_edges", csc_edges))
      return false;
    if (!entity_dict_.save((output_dir_ / "entity.dict").string()))
      return false;
    if (!relation_dict_.save((output_dir_ / "relation.dict").string()))
      return false;

    // 4. Serialize Meta
    GraphMeta meta{num_nodes, num_edges, 1, 0};
    if (!save_array(output_dir_ / "snapshot.meta",
                    std::vector<GraphMeta>{meta}))
      return false;

    return true;
  }

  utils::StringDict &get_entity_dict() { return entity_dict_; }
  utils::StringDict &get_relation_dict() { return relation_dict_; }

private:
  struct RawEdge {
    EntityID src;
    EntityID dst;
    RelationID rel;
  };

  fs::path output_dir_;
  utils::StringDict entity_dict_;
  utils::StringDict relation_dict_;
  std::vector<RawEdge> raw_edges_;

  template <typename T>
  bool save_array(const fs::path &path, const std::vector<T> &arr) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return false;
    if (!arr.empty()) {
      out.write(reinterpret_cast<const char *>(arr.data()),
                arr.size() * sizeof(T));
    }
    return out.good();
  }
};

} // namespace builder
} // namespace graph_engine
