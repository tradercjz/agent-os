#pragma once

#include <cstdint>
#include <limits>

namespace graph_engine {

// 4.2B 节点上限；超出可改 uint64_t
using EntityID = uint32_t;
// uint32_t 支持 ~42 亿关系类型（金融本体场景需要远超 65K）
using RelationID = uint32_t;

// Temporal Edge：带时间窗口的有向关系
// CSR 中只存 target+relation+时间戳，source 由 offset 数组隐含
struct Edge {
  EntityID target;
  RelationID relation;
  uint64_t start_ts{0}; // 关系生效时间（0 = 未设定）
  uint64_t end_ts{0};   // 关系失效时间（0 = 当前有效）

  // 判断边在给定时间点是否有效
  bool is_active_at(uint64_t ts) const {
    if (ts == 0)
      return true; // 不做时间过滤
    if (start_ts == 0 && end_ts == 0)
      return true; // 无时间信息，视为永久有效
    return (start_ts <= ts) && (end_ts == 0 || end_ts >= ts);
  }
};

// 图元数据快照
struct GraphMeta {
  uint32_t num_nodes;
  uint64_t num_edges;
  uint32_t version;
  uint32_t _padding{0};
};

} // namespace graph_engine
