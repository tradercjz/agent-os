#pragma once

#include "graph_engine/core/types.hpp"
#include "graph_engine/storage/mmap_file.hpp"
#include "graph_engine/utils/string_dict.hpp"
#include <filesystem>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace graph_engine {
namespace core {

namespace fs = std::filesystem;

// K-hop / 子图抽取结果
struct SubgraphResult {
  struct Triple {
    std::string src;
    std::string relation;
    std::string dst;
    uint64_t start_ts;
    uint64_t end_ts;
  };
  std::vector<EntityID> node_ids;
  std::vector<Triple> triples;
};

class ImmutableGraph {
public:
  ImmutableGraph(fs::path graph_dir) : dir_(std::move(graph_dir)) {}

  ~ImmutableGraph() { close(); }

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

    // CSC 可选
    if (csc_offsets_file_.open((dir_ / "graph.csc_offsets").string()) &&
        csc_edges_file_.open((dir_ / "graph.csc_edges").string())) {
      csc_offsets_ = csc_offsets_file_.as_array<uint64_t>();
      csc_edges_ = csc_edges_file_.as_array<Edge>();
    }

    offsets_ = offsets_file_.as_array<uint64_t>();
    edges_ = edges_file_.as_array<Edge>();
    meta_ = meta_file_.as_array<GraphMeta>();

    if (meta_ == nullptr || meta_->num_nodes != entity_dict_.size()) {
      return false;
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

  // ── 1-hop 出边邻居（带可选时间过滤）─────────────────
  std::vector<std::pair<std::string, std::string>>
  get_neighbors(const std::string &src_name, uint64_t ts = 0) const {
    std::vector<std::pair<std::string, std::string>> result;

    EntityID src_id;
    if (!entity_dict_.find_id(src_name, src_id))
      return result;
    if (src_id >= meta_->num_nodes)
      return result;

    uint64_t start_idx = offsets_[src_id];
    uint64_t end_idx = offsets_[src_id + 1];

    for (uint64_t i = start_idx; i < end_idx; ++i) {
      const Edge &edge = edges_[i];
      if (!edge.is_active_at(ts))
        continue;
      result.push_back({entity_dict_.get_string(edge.target),
                         relation_dict_.get_string(edge.relation)});
    }

    return result;
  }

  // ── 1-hop 入边邻居（需要 CSC 索引）──────────────────
  std::vector<std::pair<std::string, std::string>>
  get_incoming_neighbors(const std::string &dst_name, uint64_t ts = 0) const {
    std::vector<std::pair<std::string, std::string>> result;

    if (!csc_offsets_ || !csc_edges_)
      return result;

    EntityID dst_id;
    if (!entity_dict_.find_id(dst_name, dst_id))
      return result;
    if (dst_id >= meta_->num_nodes)
      return result;

    uint64_t start_idx = csc_offsets_[dst_id];
    uint64_t end_idx = csc_offsets_[dst_id + 1];

    for (uint64_t i = start_idx; i < end_idx; ++i) {
      const Edge &edge = csc_edges_[i]; // target 存的是 source
      if (!edge.is_active_at(ts))
        continue;
      result.push_back({entity_dict_.get_string(edge.target),
                         relation_dict_.get_string(edge.relation)});
    }

    return result;
  }

  // ── K-hop BFS 子图抽取（带时间窗口过滤）─────────────
  SubgraphResult k_hop(const std::string &start_name, int k,
                       uint64_t ts = 0) const {
    SubgraphResult result;
    EntityID start_id;
    if (!entity_dict_.find_id(start_name, start_id))
      return result;
    return k_hop(start_id, k, ts);
  }

  SubgraphResult k_hop(EntityID start_id, int k, uint64_t ts = 0) const {
    SubgraphResult result;
    if (start_id >= meta_->num_nodes)
      return result;

    std::unordered_set<EntityID> visited;
    std::queue<std::pair<EntityID, int>> queue;
    queue.push({start_id, 0});
    visited.insert(start_id);

    while (!queue.empty()) {
      auto [node, depth] = queue.front();
      queue.pop();

      if (depth >= k)
        continue;

      uint64_t s = offsets_[node];
      uint64_t e = offsets_[node + 1];

      for (uint64_t i = s; i < e; ++i) {
        const Edge &edge = edges_[i];
        if (!edge.is_active_at(ts))
          continue;

        result.triples.push_back(
            {entity_dict_.get_string(node),
             relation_dict_.get_string(edge.relation),
             entity_dict_.get_string(edge.target), edge.start_ts,
             edge.end_ts});

        if (visited.insert(edge.target).second) {
          queue.push({edge.target, depth + 1});
        }
      }
    }

    result.node_ids.assign(visited.begin(), visited.end());
    return result;
  }

  // ── Raw O(1) 邻接数组访问 ───────────────────────────
  std::pair<const Edge *, size_t> get_raw_neighbors(EntityID src_id) const {
    if (src_id >= meta_->num_nodes)
      return {nullptr, 0};
    uint64_t start = offsets_[src_id];
    uint64_t end = offsets_[src_id + 1];
    return {&edges_[start], end - start};
  }

  const utils::StringDict &get_entity_dict() const { return entity_dict_; }
  const utils::StringDict &get_relation_dict() const {
    return relation_dict_;
  }
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
