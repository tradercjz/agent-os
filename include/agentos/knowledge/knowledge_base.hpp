#pragma once

#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/bm25_index.hpp"
#include "agentos/knowledge/document.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <fstream>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <hnswlib/hnswlib.h>
#pragma GCC diagnostic pop
#include <agentos/core/logger.hpp>
#ifndef AGENTOS_NO_DUCKDB
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <duckdb.hpp>
#pragma GCC diagnostic pop
#endif
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace agentos::knowledge {

/**
 * @brief Hybrid 检索结果（文本 + 可选图上下文）
 */
struct SearchResult {
  std::string doc_id;
  std::string chunk_id;
  std::string content;
  double score;
  std::string graph_context; // graph_hops > 0 时填充
};

/**
 * @brief Turnkey RAG + GraphRAG 知识库
 *
 * 功能：
 *   - 文本 ingest → 句子级分块 → BM25 + HNSW 双路索引
 *   - Hybrid RRF 融合检索
 *   - attach_graph() 后支持 GraphRAG（实体匹配 → K-hop 子图扩展）
 */
class KnowledgeBase {
public:
  explicit KnowledgeBase(std::shared_ptr<kernel::ILLMBackend> llm,
                         uint32_t vector_dim = 1536,
                         size_t max_chunks = 100000,
                         std::string embedding_model = "text-embedding-3-small")
      : llm_(std::move(llm)), embedding_model_(std::move(embedding_model)),
        dim_(vector_dim), max_chunks_(max_chunks) {
    space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
    hnsw_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(space_.get(),
                                                               max_chunks_);
  }

  // ── 配置访问 ─────────────────────────────────────
  [[nodiscard]]
  const std::string &embedding_model() const noexcept {
    std::lock_guard lk(mu_);
    return embedding_model_;
  }
  void set_embedding_model(const std::string &model) {
    std::lock_guard lk(mu_);
    embedding_model_ = model;
  }

  [[nodiscard]]
  size_t chunk_size() const noexcept {
    std::lock_guard lk(mu_);
    return chunk_size_;
  }
  [[nodiscard]]
  size_t chunk_overlap() const noexcept {
    std::lock_guard lk(mu_);
    return chunk_overlap_;
  }
  void set_chunk_params(size_t size, size_t overlap) {
    std::lock_guard lk(mu_);
    chunk_size_ = size;
    chunk_overlap_ = overlap;
  }

  // ── Graph 关联 ─────────────────────────────────────
  void attach_graph(
      std::shared_ptr<graph_engine::core::ImmutableGraph> graph) {
    std::lock_guard lk(mu_);
    graph_ = std::move(graph);
  }

  // ── 文本摄入（返回成功摄入的 chunk 数量）─────────────
  [[nodiscard]]
  Result<size_t> ingest_text(const std::string &doc_id, const std::string &text) {
    std::lock_guard lk(mu_);
    auto chunks = chunk_text(text, chunk_size_, chunk_overlap_);
    LOG_DEBUG(fmt::format("[KB] Document [{}] parsed into {} chunks", doc_id, chunks.size()));

    size_t ingested = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
      std::string chunk_id = doc_id + "_chunk_" + std::to_string(i);

      // 去重：跳过已存在的 chunk
      if (chunk_content_.contains(chunk_id))
        continue;

      bm25_.add_document(chunk_id, chunks[i]);
      chunk_content_[chunk_id] = chunks[i];
      chunk_docs_[chunk_id] = doc_id;

      // Dense embedding
      kernel::EmbeddingRequest req;
      req.inputs = {chunks[i]};
      req.model = embedding_model_;

      auto resp = llm_->embed(req);
      if (resp && !resp->embeddings.empty() &&
          resp->embeddings[0].size() == dim_) {
        // 动态扩容（saturated doubling）
        if (next_hnsw_id_ >= max_chunks_) {
          if (max_chunks_ > SIZE_MAX / 2) {
            LOG_ERROR(fmt::format("[KB] HNSW index capacity exceeded, cannot add chunk {}", chunk_id));
            continue; // Skip chunk, log error instead of silent loss
          }
          max_chunks_ *= 2;
          try {
            hnsw_->resizeIndex(max_chunks_);
          } catch (const std::exception &e) {
            LOG_ERROR(fmt::format("[KB] Failed to resize HNSW index: {}", e.what()));
            continue;
          }
          // Verify new capacity before adding point
          if (next_hnsw_id_ >= max_chunks_) {
            LOG_ERROR(fmt::format("[KB] HNSW resize failed to create capacity for chunk {}", chunk_id));
            continue;
          }
        }
        try {
          hnsw_->addPoint(resp->embeddings[0].data(), next_hnsw_id_);
          hnsw_id_to_chunk_[next_hnsw_id_] = chunk_id;
          next_hnsw_id_++;
        } catch (const std::exception &e) {
          LOG_ERROR(fmt::format("[KB] Failed to add point to HNSW: {}", e.what()));
          continue;
        }
      }
      ++ingested;
    }
    return ingested;
  }

  // ── 持久化（DuckDB + BM25/HNSW 二进制）──────────────
  bool save(const fs::path &dir) const {
    std::lock_guard lk(mu_);
    fs::create_directories(dir);

    // 1. BM25 索引（专用二进制格式）
    if (!bm25_.save(dir / "bm25_index.bin")) {
      LOG_ERROR("[KB] Failed to save BM25 index");
      return false;
    }

    // 2. HNSW 索引（专用二进制格式）
    if (hnsw_ && next_hnsw_id_ > 0) {
      try {
        hnsw_->saveIndex((dir / "hnsw_index.bin").string());
      } catch (const std::exception &e) {
        LOG_ERROR(fmt::format("[KB] Failed to save HNSW index: {}", e.what()));
        return false;
      }
    }

    // 3. chunk 元数据
#ifndef AGENTOS_NO_DUCKDB
    // DuckDB 列存格式
    try {
      auto db_path = (dir / "kb_meta.duckdb").string();
      duckdb::DuckDB db(db_path);
      duckdb::Connection con(db);

      con.Query("DROP TABLE IF EXISTS kb_config");
      con.Query("CREATE TABLE kb_config (key VARCHAR, value VARCHAR)");
      con.Query("DROP TABLE IF EXISTS chunks");
      con.Query("CREATE TABLE chunks ("
                "  chunk_id VARCHAR PRIMARY KEY,"
                "  doc_id VARCHAR NOT NULL,"
                "  content VARCHAR NOT NULL)");
      con.Query("DROP TABLE IF EXISTS hnsw_map");
      con.Query("CREATE TABLE hnsw_map ("
                "  label UBIGINT PRIMARY KEY,"
                "  chunk_id VARCHAR NOT NULL)");

      // Config
      auto cfg = con.Prepare(
          "INSERT INTO kb_config VALUES ($1, $2)");
      (void)cfg->Execute(std::string("dim"), std::to_string(dim_));
      (void)cfg->Execute(std::string("max_chunks"), std::to_string(max_chunks_));
      (void)cfg->Execute(std::string("next_hnsw_id"), std::to_string(next_hnsw_id_));
      (void)cfg->Execute(std::string("embedding_model"), embedding_model_);

      // Chunks (batch insert via appender)
      {
        duckdb::Appender appender(con, "chunks");
        for (const auto &[id, content] : chunk_content_) {
          auto doc_it = chunk_docs_.find(id);
          std::string doc_id = doc_it != chunk_docs_.end() ? doc_it->second : "";
          appender.AppendRow(duckdb::Value(id), duckdb::Value(doc_id), duckdb::Value(content));
        }
        appender.Close();
      }

      // HNSW label map
      {
        duckdb::Appender appender(con, "hnsw_map");
        for (const auto &[label, chunk_id] : hnsw_id_to_chunk_) {
          appender.AppendRow(duckdb::Value::UBIGINT(static_cast<uint64_t>(label)), duckdb::Value(chunk_id));
        }
        appender.Close();
      }

      LOG_INFO(fmt::format("[KB] Saved: {} chunks, {} HNSW points -> {}",
                           chunk_content_.size(), hnsw_id_to_chunk_.size(),
                           dir.string()));
    } catch (const std::exception &e) {
      LOG_ERROR(fmt::format("[KB] Failed to save metadata: {}", e.what()));
      return false;
    }
#else
    // 无 DuckDB: 使用旧二进制格式
    if (!save_to_legacy_bin(dir))
      return false;
#endif
    return true;
  }

  bool load(const fs::path &dir) {
    std::lock_guard lk(mu_);

    // 检查可用格式
#ifndef AGENTOS_NO_DUCKDB
    bool has_duckdb = fs::exists(dir / "kb_meta.duckdb");
#else
    bool has_duckdb = false;
#endif
    bool has_legacy = fs::exists(dir / "kb_meta.bin");
    if (!has_duckdb && !has_legacy)
      return false;

    // 1. BM25 索引
    if (!bm25_.load(dir / "bm25_index.bin")) {
      LOG_ERROR("[KB] Failed to load BM25 index");
      return false;
    }

#ifndef AGENTOS_NO_DUCKDB
    if (has_duckdb) {
      if (!load_from_duckdb(dir))
        return false;
    } else
#endif
    {
      if (!load_from_legacy_bin(dir))
        return false;
    }

    // HNSW 索引
    auto hnsw_path = dir / "hnsw_index.bin";
    if (fs::exists(hnsw_path) && !hnsw_id_to_chunk_.empty()) {
      space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
      try {
        hnsw_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            space_.get(), hnsw_path.string());
      } catch (const std::exception &e) {
        LOG_WARN(fmt::format("[KB] Failed to load HNSW: {}", e.what()));
        hnsw_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            space_.get(), max_chunks_);
      }
    }

    LOG_INFO(fmt::format("[KB] Loaded: {} chunks from {}",
                         chunk_content_.size(), dir.string()));
    return true;
  }

  void ingest_directory(const fs::path &dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      LOG_ERROR(fmt::format("[KB] Directory does not exist: {}", dir.string()));
      return;
    }
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && (entry.path().extension() == ".md" ||
                                      entry.path().extension() == ".txt")) {
        std::ifstream ifs(entry.path());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        (void)ingest_text(entry.path().filename().string(), content);
      }
    }
  }

  // ── 文档删除（逻辑删除：从 BM25 + HNSW 映射中移除）────
  bool remove_document(const std::string &doc_id) {
    std::lock_guard lk(mu_);
    // 收集该 doc 的所有 chunk_id
    std::vector<std::string> to_remove;
    for (const auto &[chunk_id, did] : chunk_docs_) {
      if (did == doc_id)
        to_remove.push_back(chunk_id);
    }
    if (to_remove.empty())
      return false;

    for (const auto &chunk_id : to_remove) {
      // 从 HNSW 标记删除
      for (auto it = hnsw_id_to_chunk_.begin(); it != hnsw_id_to_chunk_.end();) {
        if (it->second == chunk_id) {
          if (hnsw_) {
            try {
              hnsw_->markDelete(it->first);
            } catch (const std::exception &) {
            }
          }
          it = hnsw_id_to_chunk_.erase(it);
        } else {
          ++it;
        }
      }
      // 从 BM25 索引中移除
      bm25_.remove_document(chunk_id);
      // 从映射中移除
      chunk_content_.erase(chunk_id);
      chunk_docs_.erase(chunk_id);
    }
    return true;
  }

  // ── 统计 ─────────────────────────────────────────
  [[nodiscard]]
  size_t chunk_count() const noexcept {
    std::lock_guard lk(mu_);
    return chunk_content_.size();
  }
  [[nodiscard]]
  size_t document_count() const noexcept {
    std::lock_guard lk(mu_);
    std::unordered_set<std::string> docs;
    for (const auto &[_, doc_id] : chunk_docs_)
      docs.insert(doc_id);
    return docs.size();
  }

  // ── Hybrid 检索 + 可选 GraphRAG 扩展 ───────────────
  [[nodiscard]]
  std::vector<SearchResult> search(const std::string &query,
                                    size_t top_k = 5,
                                    int graph_hops = 0) {
    std::lock_guard lk(mu_);
    const size_t internal_k = top_k * 2;

    // 1. Sparse (BM25)
    auto bm25_results = bm25_.search(query, internal_k);

    // 2. Dense (HNSW)
    std::vector<std::pair<std::string, double>> dense_results;
    kernel::EmbeddingRequest req;
    req.inputs = {query};
    req.model = "text-embedding-3-small";
    auto emb_resp = llm_->embed(req);

    if (emb_resp && !emb_resp->embeddings.empty()) {
      size_t available = hnsw_->cur_element_count;
      size_t k = std::min(internal_k, available);
      if (k > 0) {
        auto result = hnsw_->searchKnn(emb_resp->embeddings[0].data(), k);
        dense_results.reserve(k);
        while (!result.empty()) {
          auto item = result.top();
          auto it = hnsw_id_to_chunk_.find(item.second);
          if (it != hnsw_id_to_chunk_.end()) {
            dense_results.push_back({it->second, item.first});
          }
          result.pop();
        }
        // HNSW returns max-heap (highest distance first); reverse for ascending order
        std::reverse(dense_results.begin(), dense_results.end());
      }
    }

    // 3. RRF Fusion
    auto fused = rrf_fuse(bm25_results, dense_results, top_k);

    // 4. GraphRAG 扩展（如果已 attach graph 且 hops > 0）
    if (graph_ && graph_hops > 0) {
      std::string graph_ctx = build_graph_context(query, fused, graph_hops);
      if (!graph_ctx.empty()) {
        for (auto &r : fused) {
          r.graph_context = graph_ctx;
        }
      }
    }

    return fused;
  }

private:
  mutable std::mutex mu_;
  std::shared_ptr<kernel::ILLMBackend> llm_;
  BM25Index bm25_;
  std::string embedding_model_;
  size_t chunk_size_{500};
  size_t chunk_overlap_{50};

  uint32_t dim_;
  size_t max_chunks_;
  std::unique_ptr<hnswlib::SpaceInterface<float>> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_;
  hnswlib::labeltype next_hnsw_id_{0};

  std::unordered_map<hnswlib::labeltype, std::string> hnsw_id_to_chunk_;
  std::unordered_map<std::string, std::string> chunk_content_;
  std::unordered_map<std::string, std::string> chunk_docs_;

  // GraphRAG
  std::shared_ptr<graph_engine::core::ImmutableGraph> graph_;

#ifndef AGENTOS_NO_DUCKDB
  // ── DuckDB 元数据加载 ───────────────────────────────
  bool load_from_duckdb(const fs::path &dir) {
    try {
      duckdb::DuckDB db((dir / "kb_meta.duckdb").string());
      duckdb::Connection con(db);

      // Config
      auto cfg = con.Query("SELECT key, value FROM kb_config");
      if (cfg->HasError()) return false;
      while (auto chunk = cfg->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          auto key = chunk->GetValue(0, row).ToString();
          auto val = chunk->GetValue(1, row).ToString();
          if (key == "dim") dim_ = static_cast<uint32_t>(std::stoul(val));
          else if (key == "max_chunks") max_chunks_ = std::stoull(val);
          else if (key == "next_hnsw_id") next_hnsw_id_ = std::stoull(val);
          else if (key == "embedding_model") embedding_model_ = val;
        }
      }

      // Chunks
      chunk_content_.clear();
      chunk_docs_.clear();
      auto chunks = con.Query("SELECT chunk_id, doc_id, content FROM chunks");
      if (chunks->HasError()) return false;
      while (auto chunk = chunks->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          auto cid = chunk->GetValue(0, row).ToString();
          auto did = chunk->GetValue(1, row).ToString();
          auto content = chunk->GetValue(2, row).ToString();
          chunk_docs_[cid] = did;
          chunk_content_[cid] = std::move(content);
        }
      }

      // HNSW map
      hnsw_id_to_chunk_.clear();
      auto hmap = con.Query("SELECT label, chunk_id FROM hnsw_map");
      if (hmap->HasError()) return false;
      while (auto chunk = hmap->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); row++) {
          auto label = static_cast<hnswlib::labeltype>(
              chunk->GetValue(0, row).GetValue<uint64_t>());
          auto cid = chunk->GetValue(1, row).ToString();
          hnsw_id_to_chunk_[label] = std::move(cid);
        }
      }
      return true;
    } catch (const std::exception &e) {
      LOG_ERROR(fmt::format("[KB] DuckDB load failed: {}", e.what()));
      return false;
    }
  }
#endif

  // ── 旧二进制格式保存 ───────────────────────────────
  bool save_to_legacy_bin(const fs::path &dir) const {
    std::ofstream ofs(dir / "kb_meta.bin", std::ios::binary);
    if (!ofs) return false;

    uint32_t magic = 0x4B424D54;
    ofs.write(reinterpret_cast<const char *>(&magic), 4);
    ofs.write(reinterpret_cast<const char *>(&dim_), sizeof(dim_));
    ofs.write(reinterpret_cast<const char *>(&max_chunks_), sizeof(max_chunks_));

    auto write_str = [&](const std::string &s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      ofs.write(reinterpret_cast<const char *>(&len), 4);
      ofs.write(s.data(), len);
    };

    write_str(embedding_model_);

    uint32_t n_chunks = static_cast<uint32_t>(chunk_content_.size());
    ofs.write(reinterpret_cast<const char *>(&n_chunks), 4);
    for (const auto &[id, content] : chunk_content_) {
      write_str(id);
      write_str(content);
    }

    uint32_t n_docs = static_cast<uint32_t>(chunk_docs_.size());
    ofs.write(reinterpret_cast<const char *>(&n_docs), 4);
    for (const auto &[chunk_id, doc_id] : chunk_docs_) {
      write_str(chunk_id);
      write_str(doc_id);
    }

    ofs.write(reinterpret_cast<const char *>(&next_hnsw_id_), sizeof(next_hnsw_id_));
    uint32_t n_hnsw = static_cast<uint32_t>(hnsw_id_to_chunk_.size());
    ofs.write(reinterpret_cast<const char *>(&n_hnsw), 4);
    for (const auto &[label, chunk_id] : hnsw_id_to_chunk_) {
      ofs.write(reinterpret_cast<const char *>(&label), sizeof(label));
      write_str(chunk_id);
    }

    LOG_INFO(fmt::format("[KB] Saved (legacy bin): {} chunks -> {}",
                         chunk_content_.size(), dir.string()));
    return ofs.good();
  }

  // ── 旧二进制格式兼容加载 ─────────────────────────────
  bool load_from_legacy_bin(const fs::path &dir) {
    std::ifstream ifs(dir / "kb_meta.bin", std::ios::binary);
    if (!ifs) return false;

    uint32_t magic;
    ifs.read(reinterpret_cast<char *>(&magic), 4);
    if (magic != 0x4B424D54) return false;

    ifs.read(reinterpret_cast<char *>(&dim_), sizeof(dim_));
    ifs.read(reinterpret_cast<char *>(&max_chunks_), sizeof(max_chunks_));

    auto read_str = [&]() -> std::string {
      uint32_t len;
      ifs.read(reinterpret_cast<char *>(&len), 4);
      if (!ifs.good() || len > 100 * 1024 * 1024) return {};
      std::string s(len, '\0');
      ifs.read(s.data(), len);
      return s;
    };

    embedding_model_ = read_str();

    uint32_t n_chunks;
    ifs.read(reinterpret_cast<char *>(&n_chunks), 4);
    if (!ifs.good() || n_chunks > 10'000'000) return false;
    chunk_content_.clear();
    chunk_content_.reserve(n_chunks);
    for (uint32_t i = 0; i < n_chunks; ++i) {
      auto id = read_str();
      auto content = read_str();
      if (!ifs.good()) return false;
      chunk_content_[std::move(id)] = std::move(content);
    }

    uint32_t n_docs;
    ifs.read(reinterpret_cast<char *>(&n_docs), 4);
    if (!ifs.good() || n_docs > 10'000'000) return false;
    chunk_docs_.clear();
    chunk_docs_.reserve(n_docs);
    for (uint32_t i = 0; i < n_docs; ++i) {
      auto chunk_id = read_str();
      auto doc_id = read_str();
      if (!ifs.good()) return false;
      chunk_docs_[std::move(chunk_id)] = std::move(doc_id);
    }

    ifs.read(reinterpret_cast<char *>(&next_hnsw_id_), sizeof(next_hnsw_id_));
    uint32_t n_hnsw;
    ifs.read(reinterpret_cast<char *>(&n_hnsw), 4);
    if (!ifs.good() || n_hnsw > 10'000'000) return false;
    hnsw_id_to_chunk_.clear();
    hnsw_id_to_chunk_.reserve(n_hnsw);
    for (uint32_t i = 0; i < n_hnsw; ++i) {
      hnswlib::labeltype label;
      ifs.read(reinterpret_cast<char *>(&label), sizeof(label));
      auto chunk_id = read_str();
      hnsw_id_to_chunk_[label] = std::move(chunk_id);
    }
    return ifs.good();
  }

  // ── 句子级分块（UTF-8 安全）─────────────────────────
  static std::vector<std::string> split_sentences(const std::string &text) {
    std::vector<std::string> sentences;
    size_t start = 0;

    for (size_t i = 0; i < text.size();) {
      unsigned char c = static_cast<unsigned char>(text[i]);
      size_t clen = 1;
      if (c >= 0xF0)
        clen = 4;
      else if (c >= 0xE0)
        clen = 3;
      else if (c >= 0xC0)
        clen = 2;
      if (i + clen > text.size())
        clen = 1;

      bool is_boundary = false;
      if (clen == 1) {
        is_boundary = (c == '.' || c == '!' || c == '?' || c == '\n');
      } else if (clen == 3 && i + 3 <= text.size()) {
        std::string_view sv(&text[i], 3);
        is_boundary =
            (sv == "\xe3\x80\x82" || // 。
             sv == "\xef\xbc\x81" || // ！
             sv == "\xef\xbc\x9f" || // ？
             sv == "\xef\xbc\x9b"); // ；
      }

      i += clen;

      if (is_boundary || i >= text.size()) {
        std::string sent = text.substr(start, i - start);
        // trim
        size_t s = sent.find_first_not_of(" \t\r\n");
        size_t e = sent.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) {
          sentences.push_back(sent.substr(s, e - s + 1));
        }
        start = i;
      }
    }

    return sentences;
  }

  static std::vector<std::string> chunk_text(const std::string &text,
                                              size_t chunk_size,
                                              size_t overlap) {
    auto sents = split_sentences(text);
    if (sents.empty())
      return {};
    if (text.size() <= chunk_size)
      return {text};

    std::vector<std::string> chunks;
    size_t si = 0;

    while (si < sents.size()) {
      std::string chunk;
      size_t chunk_start = si;

      while (si < sents.size()) {
        size_t cost = sents[si].size() + (chunk.empty() ? 0 : 1);
        if (!chunk.empty() && chunk.size() + cost > chunk_size)
          break;
        if (!chunk.empty())
          chunk += ' ';
        chunk += sents[si++];
      }

      if (si == chunk_start) { // 单个超长句
        chunk = sents[si++];
      }

      chunks.push_back(std::move(chunk));

      // 按字符预算回退若干句作为 overlap
      if (si < sents.size() && overlap > 0) {
        size_t back = 0;
        size_t back_chars = 0;
        while (back < si - chunk_start - 1 && back_chars < overlap) {
          back++;
          back_chars += sents[si - back].size();
        }
        if (back > 0)
          si -= back;
      }
    }

    return chunks;
  }

  // ── RRF 融合 ───────────────────────────────────────
  std::vector<SearchResult>
  rrf_fuse(const std::vector<BM25Index::Match> &sparse,
           const std::vector<std::pair<std::string, double>> &dense,
           size_t top_k) {
    const double k_fusion = 60.0;
    std::unordered_map<std::string, double> rrf_scores;

    for (size_t i = 0; i < sparse.size(); ++i) {
      rrf_scores[sparse[i].doc_id] += 1.0 / (k_fusion + i + 1);
    }
    for (size_t i = 0; i < dense.size(); ++i) {
      rrf_scores[dense[i].first] += 1.0 / (k_fusion + i + 1);
    }

    std::vector<SearchResult> fused;
    for (const auto &[chunk_id, score] : rrf_scores) {
      // 跳过已删除的 chunk（remove_document 后 BM25 可能仍返回旧 ID）
      auto content_it = chunk_content_.find(chunk_id);
      if (content_it == chunk_content_.end())
        continue;
      auto doc_it = chunk_docs_.find(chunk_id);
      std::string doc_id = doc_it != chunk_docs_.end() ? doc_it->second : "";
      fused.push_back({doc_id, chunk_id, content_it->second, score, ""});
    }

    std::sort(fused.begin(), fused.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });
    if (fused.size() > top_k) {
      fused.resize(top_k);
    }
    return fused;
  }

  // ── GraphRAG：实体匹配 + K-hop 子图 → 文本上下文 ──
  [[nodiscard]]
  std::string build_graph_context(
      const std::string &query,
      const std::vector<SearchResult> &results,
      int hops) {
    if (!graph_)
      return "";

    // 1. 从 query + top chunk 中提取已知实体
    const auto &dict = graph_->get_entity_dict();
    std::unordered_set<std::string> matched_entities;

    auto match_entities = [&](const std::string &text) {
      for (size_t i = 0; i < dict.size(); i++) {
        const auto &name = dict.get_string(
            static_cast<graph_engine::EntityID>(i));
        if (name.size() >= 2 && text.find(name) != std::string::npos) {
          matched_entities.insert(name);
        }
      }
    };

    match_entities(query);
    for (const auto &r : results) {
      match_entities(r.content);
    }

    if (matched_entities.empty())
      return "";

    // 2. 对每个匹配实体做 K-hop 遍历，收集三元组
    std::unordered_set<std::string> seen_triples;
    std::string context;

    for (const auto &entity : matched_entities) {
      auto sub = graph_->k_hop(entity, hops);
      if (sub.triples.empty()) {
        // Log warning when entity not found in graph
        LOG_WARN(fmt::format("GraphRAG: entity '{}' not found in graph, skipping expansion", entity));
        continue;
      }
      for (const auto &t : sub.triples) {
        std::string key = t.src + "|" + t.relation + "|" + t.dst;
        if (seen_triples.insert(key).second) {
          context += "[" + t.src + "] --(" + t.relation + ")--> [" + t.dst +
                     "]\n";
        }
      }
    }

    return context;
  }
};

} // namespace agentos::knowledge
