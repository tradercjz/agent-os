#pragma once
// ============================================================
// AgentOS :: Module 4 — Graph Memory Extension
// 本体图机制：多租户支持 + 基础的关系存储与召回
// ============================================================
#include <agentos/core/types.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos::memory {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// Ontology Graph Basic Definitions
// ─────────────────────────────────────────────────────────────

struct GraphNode {
  std::string id;      // Unique entity name (e.g., "Apple", "User_001")
  std::string type;    // e.g., "Company", "Person", "Concept"
  std::string content; // Optional description or JSON payload
  uint64_t created_at{0};
};

struct GraphEdge {
  std::string source_id;
  std::string target_id;
  std::string relation; // e.g., "founded_by", "ceo_of", "likes"
  float weight{1.0f};   // Optional edge weight/importance
  uint64_t start_ts{0};
  uint64_t end_ts{0}; // For temporal graphs
};

struct Subgraph {
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;
};

// ─────────────────────────────────────────────────────────────
// IGraphMemory Interface (Phase 1)
// ─────────────────────────────────────────────────────────────

class IGraphMemory {
public:
  virtual ~IGraphMemory() = default;

  virtual Result<bool> add_node(GraphNode node) = 0;
  virtual Result<bool> add_edge(GraphEdge edge) = 0;

  // Quick adjacent search
  virtual Result<std::vector<GraphEdge>>
  get_edges(const std::string &node_id) = 0;
  virtual Result<std::vector<GraphEdge>>
  get_edges_by_relation(const std::string &node_id,
                        const std::string &relation) = 0;

  // Semantic K-hop Subgraph retrieval
  virtual Result<Subgraph> k_hop_search(const std::string &start_node_id, int k,
                                        uint64_t current_ts = 0) = 0;

  virtual void save_snapshot() = 0;
};

// ─────────────────────────────────────────────────────────────
// LocalGraphMemory Implementation (in-memory + append-only WAL)
// ─────────────────────────────────────────────────────────────

class LocalGraphMemory : public IGraphMemory {
public:
  explicit LocalGraphMemory(fs::path dir = "/tmp/agentos_ltm")
      : dir_(std::move(dir)), wal_path_(dir_ / "graph_wal.log") {
    fs::create_directories(dir_);
    load_from_wal();
  }

  ~LocalGraphMemory() override {
    // Optional: flush remaining states or compact WAL on shutdown
  }

  Result<bool> add_node(GraphNode node) override {
    std::lock_guard lk(mu_);
    if (node.created_at == 0)
      node.created_at = now_ts();

    // In-memory update
    nodes_[node.id] = node;

    // Append to WAL
    append_wal("N," + escape(node.id) + "," + escape(node.type) + "," +
               escape(node.content) + "," + std::to_string(node.created_at));
    return true;
  }

  Result<bool> add_edge(GraphEdge edge) override {
    std::lock_guard lk(mu_);
    if (edge.start_ts == 0)
      edge.start_ts = now_ts();
    if (edge.end_ts == 0)
      edge.end_ts = UINT64_MAX; // indefinite

    // In-memory update
    edges_[edge.source_id].push_back(edge);

    // Ensure nodes exist implicitly
    if (nodes_.find(edge.source_id) == nodes_.end()) {
      GraphNode n;
      n.id = edge.source_id;
      n.type = "Entity";
      n.created_at = edge.start_ts;
      nodes_[n.id] = n;
      append_wal("N," + escape(n.id) + "," + escape(n.type) + "," +
                 escape(n.content) + "," + std::to_string(n.created_at));
    }
    if (nodes_.find(edge.target_id) == nodes_.end()) {
      GraphNode n;
      n.id = edge.target_id;
      n.type = "Entity";
      n.created_at = edge.start_ts;
      nodes_[n.id] = n;
      append_wal("N," + escape(n.id) + "," + escape(n.type) + "," +
                 escape(n.content) + "," + std::to_string(n.created_at));
    }

    // Append to WAL
    append_wal("E," + escape(edge.source_id) + "," + escape(edge.target_id) +
               "," + escape(edge.relation) + "," + std::to_string(edge.weight) +
               "," + std::to_string(edge.start_ts) + "," +
               std::to_string(edge.end_ts));
    return true;
  }

  Result<std::vector<GraphEdge>>
  get_edges(const std::string &node_id) override {
    std::lock_guard lk(mu_);
    if (auto it = edges_.find(node_id); it != edges_.end()) {
      return it->second;
    }
    return std::vector<GraphEdge>{};
  }

  Result<std::vector<GraphEdge>>
  get_edges_by_relation(const std::string &node_id,
                        const std::string &relation) override {
    std::lock_guard lk(mu_);
    std::vector<GraphEdge> res;
    if (auto it = edges_.find(node_id); it != edges_.end()) {
      for (const auto &e : it->second) {
        if (e.relation == relation)
          res.push_back(e);
      }
    }
    return res;
  }

  Result<Subgraph> k_hop_search(const std::string &start_node_id, int k,
                                uint64_t current_ts = 0) override {
    std::lock_guard lk(mu_);
    if (current_ts == 0)
      current_ts = now_ts();

    Subgraph result;
    if (nodes_.find(start_node_id) == nodes_.end()) {
      return make_error(ErrorCode::NotFound, "Start node not found");
    }

    std::unordered_set<std::string> visited;
    std::queue<std::pair<std::string, int>> q;

    q.push({start_node_id, 0});
    visited.insert(start_node_id);

    while (!q.empty()) {
      auto [curr_id, depth] = q.front();
      q.pop();

      result.nodes.push_back(nodes_[curr_id]);

      if (depth >= k)
        continue;

      if (edges_.find(curr_id) != edges_.end()) {
        for (const auto &edge : edges_[curr_id]) {
          // Temporal filter
          if (edge.start_ts <= current_ts && edge.end_ts >= current_ts) {
            result.edges.push_back(edge);
            if (visited.find(edge.target_id) == visited.end()) {
              visited.insert(edge.target_id);
              q.push({edge.target_id, depth + 1});
            }
          }
        }
      }
    }
    return result;
  }

  void save_snapshot() override {
    // Compaction can be done here: rewrite the WAL skipping overwritten data
    std::lock_guard lk(mu_);
    fs::path temp_wal = wal_path_.string() + ".tmp";
    std::ofstream ofs(temp_wal);
    if (!ofs)
      return;

    for (const auto &[id, n] : nodes_) {
      ofs << "N," << escape(n.id) << "," << escape(n.type) << ","
          << escape(n.content) << "," << n.created_at << "\n";
    }
    for (const auto &[id, edge_list] : edges_) {
      for (const auto &e : edge_list) {
        ofs << "E," << escape(e.source_id) << "," << escape(e.target_id) << ","
            << escape(e.relation) << "," << e.weight << "," << e.start_ts << ","
            << e.end_ts << "\n";
      }
    }
    ofs.flush();
    ofs.close();
    fs::rename(temp_wal, wal_path_);
  }

private:
  uint64_t now_ts() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::string escape(const std::string &s) const {
    std::string res;
    for (char c : s) {
      if (c == ',')
        res += "\\c";
      else if (c == '\n')
        res += "\\n";
      else if (c == '\\')
        res += "\\\\";
      else
        res += c;
    }
    return res;
  }

  std::string unescape(const std::string &s) const {
    std::string res;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        if (s[i + 1] == 'c')
          res += ',';
        else if (s[i + 1] == 'n')
          res += '\n';
        else if (s[i + 1] == '\\')
          res += '\\';
        ++i;
      } else {
        res += s[i];
      }
    }
    return res;
  }

  std::vector<std::string> split(const std::string &s, char delim) const {
    std::vector<std::string> res;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == delim) {
        res.push_back(cur);
        cur.clear();
      } else {
        cur += s[i];
      }
    }
    res.push_back(cur);
    return res;
  }

  void append_wal(const std::string &line) {
    std::ofstream ofs(wal_path_, std::ios::app);
    if (ofs) {
      ofs << line << "\n";
      ofs.flush();
    }
  }

  void load_from_wal() {
    if (!fs::exists(wal_path_))
      return;
    std::ifstream ifs(wal_path_);
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty())
        continue;
      auto parts = split(line, ',');
      try {
        if (parts.size() > 0) {
          if (parts[0] == "N" && parts.size() >= 5) {
            GraphNode n;
            n.id = unescape(parts[1]);
            n.type = unescape(parts[2]);
            n.content = unescape(parts[3]);
            n.created_at = std::stoull(parts[4]);
            nodes_[n.id] = n;
          } else if (parts[0] == "E" && parts.size() >= 7) {
            GraphEdge e;
            e.source_id = unescape(parts[1]);
            e.target_id = unescape(parts[2]);
            e.relation = unescape(parts[3]);
            e.weight = std::stof(parts[4]);
            e.start_ts = std::stoull(parts[5]);
            e.end_ts = std::stoull(parts[6]);
            edges_[e.source_id].push_back(e);
          }
        }
      } catch (...) {
        // 跳过 WAL 中损坏的行
        continue;
      }
    }
  }

  mutable std::mutex mu_;
  fs::path dir_;
  fs::path wal_path_;

  std::unordered_map<std::string, GraphNode> nodes_;
  std::unordered_map<std::string, std::vector<GraphEdge>>
      edges_; // adjacency list
};

} // namespace agentos::memory
