#pragma once

#include "graph_engine/core/types.hpp"
#include "graph_engine/storage/mmap_file.hpp"
#include "graph_engine/utils/string_dict.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace graph_engine {
namespace core {

namespace fs = std::filesystem;

class ImmutableGraph {
public:
  ImmutableGraph(fs::path graph_dir) : dir_(std::move(graph_dir)) {}

  ~ImmutableGraph() { close(); }

  // Load the graph files into memory (Zero-copy mapping)
  bool load() {
    close();

    if (!entity_dict_.load((dir_ / "entity.dict").string()))
      return false;
    if (!relation_dict_.load((dir_ / "relation.dict").string()))
      return false;

    if (!offsets_file_.open((dir_ / "graph.offsets").string()))
      return false;
    if (!edges_file_.open((dir_ / "graph.edges").string()))
      return false;
    if (!meta_file_.open((dir_ / "snapshot.meta").string()))
      return false;

    // Optional CSC loading (graceful degradation if missing)
    if (csc_offsets_file_.open((dir_ / "graph.csc_offsets").string()) &&
        csc_edges_file_.open((dir_ / "graph.csc_edges").string())) {
      csc_offsets_ = csc_offsets_file_.as_array<uint64_t>();
      csc_edges_ = csc_edges_file_.as_array<Edge>();
    }

    offsets_ = offsets_file_.as_array<uint64_t>();
    edges_ = edges_file_.as_array<Edge>();
    meta_ = meta_file_.as_array<GraphMeta>();

    if (meta_ == nullptr || meta_->num_nodes != entity_dict_.size()) {
      return false; // corrupted meta or mismatch
    }

    return true;
  }

  void close() {
    offsets_file_.close();
    edges_file_.close();
    meta_file_.close();
    csc_offsets_file_.close();
    csc_edges_file_.close();

    offsets_ = nullptr;
    edges_ = nullptr;
    meta_ = nullptr;
    csc_offsets_ = nullptr;
    csc_edges_ = nullptr;
  }

  // High-level Neighborhood API
  std::vector<std::pair<std::string, std::string>>
  get_neighbors(const std::string &src_name) const {
    std::vector<std::pair<std::string, std::string>> result;

    EntityID src_id;
    if (!entity_dict_.find_id(src_name, src_id)) {
      return result; // Node doesn't exist
    }

    if (src_id >= meta_->num_nodes)
      return result;

    uint64_t start_idx = offsets_[src_id];
    uint64_t end_idx = offsets_[src_id + 1];

    for (uint64_t i = start_idx; i < end_idx; ++i) {
      const Edge &edge = edges_[i];
      const std::string &target_str = entity_dict_.get_string(edge.target);
      const std::string &rel_str = relation_dict_.get_string(edge.relation);
      result.push_back({target_str, rel_str});
    }

    return result;
  }

  // High-level Incoming Neighborhood API (Requires CSC Index)
  std::vector<std::pair<std::string, std::string>>
  get_incoming_neighbors(const std::string &dst_name) const {
    std::vector<std::pair<std::string, std::string>> result;

    if (!csc_offsets_ || !csc_edges_)
      return result; // CSC not loaded

    EntityID dst_id;
    if (!entity_dict_.find_id(dst_name, dst_id))
      return result;

    if (dst_id >= meta_->num_nodes)
      return result;

    uint64_t start_idx = csc_offsets_[dst_id];
    uint64_t end_idx = csc_offsets_[dst_id + 1];

    for (uint64_t i = start_idx; i < end_idx; ++i) {
      const Edge &edge = csc_edges_[i]; // Here target is actually the src!
      const std::string &source_str = entity_dict_.get_string(edge.target);
      const std::string &rel_str = relation_dict_.get_string(edge.relation);
      result.push_back({source_str, rel_str});
    }

    return result;
  }

  // Raw O(1) Adjacency Array Access
  std::pair<const Edge *, size_t> get_raw_neighbors(EntityID src_id) const {
    if (src_id >= meta_->num_nodes) {
      return {nullptr, 0};
    }

    uint64_t start = offsets_[src_id];
    uint64_t end = offsets_[src_id + 1];

    return {&edges_[start], end - start};
  }

  const utils::StringDict &get_entity_dict() const { return entity_dict_; }
  const utils::StringDict &get_relation_dict() const { return relation_dict_; }
  const GraphMeta *get_meta() const { return meta_; }

private:
  fs::path dir_;

  utils::StringDict entity_dict_;
  utils::StringDict relation_dict_;

  storage::MmapFile offsets_file_;
  storage::MmapFile edges_file_;
  storage::MmapFile meta_file_;
  storage::MmapFile csc_offsets_file_;
  storage::MmapFile csc_edges_file_;

  const uint64_t *offsets_{nullptr};
  const Edge *edges_{nullptr};
  const GraphMeta *meta_{nullptr};
  const uint64_t *csc_offsets_{nullptr};
  const Edge *csc_edges_{nullptr};
};

} // namespace core
} // namespace graph_engine
