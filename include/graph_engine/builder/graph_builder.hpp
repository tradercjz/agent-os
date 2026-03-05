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

  // 添加一条三元组（可选时间窗口）
  void add_edge(const std::string &src, const std::string &dst,
                const std::string &relation, uint64_t start_ts = 0,
                uint64_t end_ts = 0) {
    EntityID u = entity_dict_.get_or_insert(src);
    EntityID v = entity_dict_.get_or_insert(dst);
    RelationID r =
        static_cast<RelationID>(relation_dict_.get_or_insert(relation));

    raw_edges_.push_back({u, v, r, start_ts, end_ts});
  }

  // 构建 CSR/CSC 并序列化到磁盘
  bool build() {
    uint32_t num_nodes = static_cast<uint32_t>(entity_dict_.size());
    uint64_t num_edges = raw_edges_.size();

    // ── CSR（出边）─────────────────────────────────────
    std::sort(raw_edges_.begin(), raw_edges_.end(),
              [](const RawEdge &a, const RawEdge &b) {
                if (a.src != b.src)
                  return a.src < b.src;
                if (a.dst != b.dst)
                  return a.dst < b.dst;
                return a.rel < b.rel;
              });

    std::vector<uint64_t> offsets(num_nodes + 1, 0);
    std::vector<Edge> edges;
    edges.reserve(num_edges);

    EntityID cur = 0;
    for (size_t i = 0; i < num_edges; ++i) {
      const auto &e = raw_edges_[i];
      while (cur < e.src) {
        offsets[++cur] = i;
      }
      edges.push_back({e.dst, e.rel, e.start_ts, e.end_ts});
    }
    while (cur < num_nodes) {
      offsets[++cur] = num_edges;
    }

    // ── CSC（入边）─────────────────────────────────────
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

    EntityID cur_dst = 0;
    for (size_t i = 0; i < num_edges; ++i) {
      const auto &e = raw_edges_[i];
      while (cur_dst < e.dst) {
        csc_offsets[++cur_dst] = i;
      }
      // CSC 中 target 存的是 source
      csc_edges.push_back({e.src, e.rel, e.start_ts, e.end_ts});
    }
    while (cur_dst < num_nodes) {
      csc_offsets[++cur_dst] = num_edges;
    }

    // ── 序列化 ─────────────────────────────────────────
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
    uint64_t start_ts;
    uint64_t end_ts;
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
