#pragma once

#include <cstdint>
#include <limits>

namespace graph_engine {

// We use 32-bit integers to support up to 4.2 billion nodes/edges tightly.
// For scales beyond 4B, this can be bumped to uint64_t via a template or
// typedef.
using EntityID = uint32_t;
using RelationID = uint16_t;

// Edge structures represent directed relationships.
// In the CSR array, we only store the destination and the relation type.
// The source node is implied by the offset array index.
struct Edge {
  EntityID target;
  RelationID relation;
};

// Represents a node in the graph, with string representation
struct Node {
  EntityID id;
  // We defer string attributes to an external dictionary lookup in Phase 1
};

// Represents a snapshot of the graph metadata
struct GraphMeta {
  uint32_t num_nodes;
  uint64_t num_edges;
  uint32_t version;
  // padding for 64-bit alignment
  uint32_t _padding{0};
};

} // namespace graph_engine
