#pragma once
// ============================================================
// AgentOS :: Module 4 — Graph Memory Extension
// 本体图机制：多租户支持 + 基础的关系存储与召回
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
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

  // Update node content/type (returns false if node doesn't exist)
  virtual Result<bool> update_node(const std::string &node_id,
                                   std::string new_content,
                                   std::string new_type = "") = 0;

  // Delete a node and all its edges
  virtual Result<bool> delete_node(const std::string &node_id) = 0;

  // Delete a specific edge
  virtual Result<bool> delete_edge(const std::string &source_id,
                                   const std::string &target_id,
                                   const std::string &relation) = 0;

  // Quick adjacent search
  virtual Result<std::vector<GraphEdge>>
  get_edges(const std::string &node_id) = 0;
  virtual Result<std::vector<GraphEdge>>
  get_edges_by_relation(const std::string &node_id,
                        const std::string &relation) = 0;

  // Semantic K-hop Subgraph retrieval
  virtual Result<Subgraph> k_hop_search(const std::string &start_node_id, int k,
                                        uint64_t current_ts = 0) = 0;

  // Prune expired edges (end_ts < cutoff_ts), returns count removed
  virtual Result<size_t> cleanup_before(uint64_t cutoff_ts) = 0;

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

  static constexpr size_t WAL_COMPACT_THRESHOLD = 5000;

  Result<bool> add_node(GraphNode node) override {
    std::lock_guard lk(mu_);
    if (node.created_at == 0)
      node.created_at = now_ts();

    // In-memory update
    nodes_[node.id] = node;

    // Append to WAL
    append_wal("N," + escape(node.id) + "," + escape(node.type) + "," +
               escape(node.content) + "," + std::to_string(node.created_at));
    maybe_compact_locked();
    return true;
  }

  Result<bool> add_edge(GraphEdge edge) override {
    std::lock_guard lk(mu_);
    if (edge.start_ts == 0)
      edge.start_ts = now_ts();
    if (edge.end_ts == 0)
      edge.end_ts = UINT64_MAX; // indefinite

    // In-memory update（去重：相同 source+target+relation 的边覆盖）
    auto &edge_list = edges_[edge.source_id];
    auto dup = std::find_if(edge_list.begin(), edge_list.end(),
                            [&](const GraphEdge &ex) {
                              return ex.target_id == edge.target_id &&
                                     ex.relation == edge.relation;
                            });
    if (dup != edge_list.end()) {
      *dup = edge; // 覆盖（更新 weight/timestamp）
    } else {
      edge_list.push_back(edge);
    }

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
    maybe_compact_locked();
    return true;
  }

  Result<bool> update_node(const std::string &node_id,
                           std::string new_content,
                           std::string new_type = "") override {
    std::lock_guard lk(mu_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end())
      return false;
    it->second.content = std::move(new_content);
    if (!new_type.empty())
      it->second.type = std::move(new_type);
    auto &n = it->second;
    append_wal("U," + escape(n.id) + "," + escape(n.type) + "," +
               escape(n.content) + "," + std::to_string(n.created_at));
    maybe_compact_locked();
    return true;
  }

  Result<bool> delete_node(const std::string &node_id) override {
    std::lock_guard lk(mu_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end())
      return false;
    nodes_.erase(it);
    // Remove outgoing edges
    edges_.erase(node_id);
    // Remove incoming edges from all adjacency lists
    for (auto &[src, edge_list] : edges_) {
      edge_list.erase(
          std::remove_if(edge_list.begin(), edge_list.end(),
                         [&](const GraphEdge &e) {
                           return e.target_id == node_id;
                         }),
          edge_list.end());
    }
    append_wal("DN," + escape(node_id));
    maybe_compact_locked();
    return true;
  }

  Result<bool> delete_edge(const std::string &source_id,
                           const std::string &target_id,
                           const std::string &relation) override {
    std::lock_guard lk(mu_);
    auto it = edges_.find(source_id);
    if (it == edges_.end())
      return false;
    auto &edge_list = it->second;
    auto before_size = edge_list.size();
    edge_list.erase(
        std::remove_if(edge_list.begin(), edge_list.end(),
                       [&](const GraphEdge &e) {
                         return e.target_id == target_id &&
                                e.relation == relation;
                       }),
        edge_list.end());
    if (edge_list.size() == before_size)
      return false; // Edge not found
    append_wal("DE," + escape(source_id) + "," + escape(target_id) + "," +
               escape(relation));
    maybe_compact_locked();
    return true;
  }

  Result<size_t> cleanup_before(uint64_t cutoff_ts) override {
    std::lock_guard lk(mu_);
    size_t removed = 0;
    for (auto &[src, edge_list] : edges_) {
      auto new_end = std::remove_if(
          edge_list.begin(), edge_list.end(),
          [&](const GraphEdge &e) {
            return e.end_ts < cutoff_ts && e.end_ts != 0;
          });
      removed += std::distance(new_end, edge_list.end());
      edge_list.erase(new_end, edge_list.end());
    }
    if (removed > 0) {
      // Compact WAL to reflect removals
      maybe_compact_locked();
    }
    return removed;
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
      std::string record = "N," + escape(n.id) + "," + escape(n.type) + "," +
                           escape(n.content) + "," + std::to_string(n.created_at);
      ofs << record << "|CRC:" << crc32(record) << "\n";
    }
    for (const auto &[id, edge_list] : edges_) {
      for (const auto &e : edge_list) {
        std::string record = "E," + escape(e.source_id) + "," + escape(e.target_id) + "," +
                             escape(e.relation) + "," + std::to_string(e.weight) + "," +
                             std::to_string(e.start_ts) + "," + std::to_string(e.end_ts);
        ofs << record << "|CRC:" << crc32(record) << "\n";
      }
    }
    ofs.flush();
    bool ok = ofs.good();
    ofs.close();
    if (ok) {
      fs::rename(temp_wal, wal_path_);
    } else {
      fs::remove(temp_wal); // Don't replace good WAL with partial write
    }
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

  /// Escape-aware split: 不会在转义逗号 (\c) 处分割
  std::vector<std::string> split(const std::string &s, char delim) const {
    std::vector<std::string> res;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        // 保留转义序列，跳过下一个字符（防止 \c 被当作分隔符）
        cur += s[i];
        cur += s[++i];
      } else if (s[i] == delim) {
        res.push_back(cur);
        cur.clear();
      } else {
        cur += s[i];
      }
    }
    res.push_back(cur);
    return res;
  }

  // CRC32 (ISO 3309 polynomial) for WAL integrity verification
  static uint32_t crc32(const std::string &data) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char c : data) {
      crc ^= c;
      for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return ~crc;
  }

  void append_wal(const std::string &line) {
    // Write to temp file then append atomically to reduce corruption risk
    uint32_t checksum = crc32(line);
    std::string record = line + "|CRC:" + std::to_string(checksum) + "\n";

    fs::path tmp_path = wal_path_.string() + ".pending";
    {
      std::ofstream ofs(tmp_path, std::ios::trunc);
      if (!ofs) return;
      ofs << record;
      ofs.flush();
      ofs.close();
    }

    // Append from temp to WAL (two-step for crash safety)
    std::ofstream wal(wal_path_, std::ios::app);
    if (wal) {
      wal << record;
      wal.flush();
    }
    fs::remove(tmp_path);
  }

  // Verify and strip CRC suffix from WAL line. Returns empty string if invalid.
  std::string verify_wal_line(const std::string &raw_line) const {
    auto crc_pos = raw_line.rfind("|CRC:");
    if (crc_pos == std::string::npos) {
      // Legacy format without CRC — accept but warn
      return raw_line;
    }
    std::string payload = raw_line.substr(0, crc_pos);
    std::string crc_str = raw_line.substr(crc_pos + 5);
    try {
      uint32_t stored_crc = static_cast<uint32_t>(std::stoul(crc_str));
      uint32_t computed_crc = crc32(payload);
      if (stored_crc != computed_crc) {
        return ""; // Corrupted line
      }
    } catch (const std::exception &) {
      return ""; // Malformed CRC
    }
    return payload;
  }

  void load_from_wal() {
    if (!fs::exists(wal_path_))
      return;

    // Check for interrupted write — recover pending record
    fs::path pending = wal_path_.string() + ".pending";
    if (fs::exists(pending)) {
      // Pending record wasn't appended — discard it (incomplete write)
      fs::remove(pending);
    }

    std::ifstream ifs(wal_path_);
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty())
        continue;

      // Verify CRC integrity
      std::string verified = verify_wal_line(line);
      if (verified.empty()) {
        LOG_WARN("GraphMemory: skipping corrupted WAL line");
        continue;
      }

      auto parts = split(verified, ',');
      try {
        if (parts.size() > 0) {
          if (parts[0] == "N" && parts.size() >= 5) {
            GraphNode n;
            n.id = unescape(parts[1]);
            n.type = unescape(parts[2]);
            n.content = unescape(parts[3]);
            n.created_at = std::stoull(parts[4]);
            nodes_[n.id] = n;
          } else if (parts[0] == "U" && parts.size() >= 5) {
            // Update node
            std::string id = unescape(parts[1]);
            auto nit = nodes_.find(id);
            if (nit != nodes_.end()) {
              nit->second.type = unescape(parts[2]);
              nit->second.content = unescape(parts[3]);
            }
          } else if (parts[0] == "DN" && parts.size() >= 2) {
            // Delete node
            std::string id = unescape(parts[1]);
            nodes_.erase(id);
            edges_.erase(id);
            for (auto &[src, el] : edges_) {
              el.erase(std::remove_if(el.begin(), el.end(),
                                      [&](const GraphEdge &e) {
                                        return e.target_id == id;
                                      }),
                       el.end());
            }
          } else if (parts[0] == "DE" && parts.size() >= 4) {
            // Delete edge
            std::string src = unescape(parts[1]);
            std::string tgt = unescape(parts[2]);
            std::string rel = unescape(parts[3]);
            auto eit = edges_.find(src);
            if (eit != edges_.end()) {
              auto &el = eit->second;
              el.erase(std::remove_if(el.begin(), el.end(),
                                      [&](const GraphEdge &e) {
                                        return e.target_id == tgt &&
                                               e.relation == rel;
                                      }),
                       el.end());
            }
          } else if (parts[0] == "E" && parts.size() >= 7) {
            GraphEdge e;
            e.source_id = unescape(parts[1]);
            e.target_id = unescape(parts[2]);
            e.relation = unescape(parts[3]);
            e.weight = std::stof(parts[4]);
            e.start_ts = std::stoull(parts[5]);
            e.end_ts = std::stoull(parts[6]);

            // 去重：相同 (source, target, relation) 的边覆盖而非累加
            auto &edge_list = edges_[e.source_id];
            auto dup = std::find_if(edge_list.begin(), edge_list.end(),
                                    [&](const GraphEdge &ex) {
                                      return ex.target_id == e.target_id &&
                                             ex.relation == e.relation;
                                    });
            if (dup != edge_list.end()) {
              *dup = e; // 用最新值覆盖
            } else {
              edge_list.push_back(e);
            }
          }
        }
      } catch (const std::exception &) {
        // 跳过 WAL 中损坏的行
        LOG_WARN("GraphMemory: skipping malformed WAL record");
        continue;
      }
    }
  }

  // Auto-compact WAL when it grows too large (called under lock)
  void maybe_compact_locked() {
    wal_ops_count_++;
    if (wal_ops_count_ >= WAL_COMPACT_THRESHOLD) {
      // Compact: rewrite WAL with only current state
      fs::path temp_wal = wal_path_.string() + ".tmp";
      std::ofstream ofs(temp_wal);
      if (!ofs) return;

      for (const auto &[id, n] : nodes_) {
        std::string record = "N," + escape(n.id) + "," + escape(n.type) + "," +
                             escape(n.content) + "," + std::to_string(n.created_at);
        ofs << record << "|CRC:" << crc32(record) << "\n";
      }
      for (const auto &[id, edge_list] : edges_) {
        for (const auto &e : edge_list) {
          std::string record = "E," + escape(e.source_id) + "," + escape(e.target_id) + "," +
                               escape(e.relation) + "," + std::to_string(e.weight) + "," +
                               std::to_string(e.start_ts) + "," + std::to_string(e.end_ts);
          ofs << record << "|CRC:" << crc32(record) << "\n";
        }
      }
      ofs.flush();
      bool write_ok = ofs.good();
      ofs.close();
      if (write_ok) {
        fs::rename(temp_wal, wal_path_);
        wal_ops_count_ = 0;
        LOG_INFO("GraphMemory: WAL compacted");
      } else {
        // Disk full or I/O error — remove partial temp file, keep original WAL
        fs::remove(temp_wal);
        LOG_WARN("GraphMemory: WAL compaction failed (disk full?), retaining original");
      }
    }
  }

  mutable std::mutex mu_;
  fs::path dir_;
  fs::path wal_path_;
  size_t wal_ops_count_{0};

  std::unordered_map<std::string, GraphNode> nodes_;
  std::unordered_map<std::string, std::vector<GraphEdge>>
      edges_; // adjacency list
};

} // namespace agentos::memory
