#pragma once

#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/bm25_index.hpp"
#include "agentos/knowledge/document.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <fstream>
#include <hnswlib/hnswlib.h>
#include <iostream>
#include <memory>
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
                         size_t max_chunks = 100000)
      : llm_(std::move(llm)), dim_(vector_dim), max_chunks_(max_chunks) {
    space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
    hnsw_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(space_.get(),
                                                               max_chunks_);
  }

  // ── Graph 关联 ─────────────────────────────────────
  void attach_graph(
      std::shared_ptr<graph_engine::core::ImmutableGraph> graph) {
    graph_ = std::move(graph);
  }

  // ── 文本摄入 ───────────────────────────────────────
  void ingest_text(const std::string &doc_id, const std::string &text) {
    auto chunks = chunk_text(text, 500, 50);
    std::cout << "[KB Ingestion] Document [" << doc_id << "] parsed into "
              << chunks.size() << " chunks.\n";

    for (size_t i = 0; i < chunks.size(); ++i) {
      std::string chunk_id = doc_id + "_chunk_" + std::to_string(i);

      // 去重：跳过已存在的 chunk
      if (chunk_content_.count(chunk_id))
        continue;

      bm25_.add_document(chunk_id, chunks[i]);
      chunk_content_[chunk_id] = chunks[i];
      chunk_docs_[chunk_id] = doc_id;

      // Dense embedding
      kernel::EmbeddingRequest req;
      req.inputs = {chunks[i]};
      req.model = "text-embedding-3-small";

      auto resp = llm_->embed(req);
      if (resp && !resp->embeddings.empty()) {
        // 动态扩容
        if (next_hnsw_id_ >= max_chunks_) {
          max_chunks_ *= 2;
          hnsw_->resizeIndex(max_chunks_);
        }
        hnsw_->addPoint(resp->embeddings[0].data(), next_hnsw_id_);
        hnsw_id_to_chunk_[next_hnsw_id_] = chunk_id;
        next_hnsw_id_++;
      }
    }
  }

  void ingest_directory(const fs::path &dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      std::cerr << "[KB Error] Directory does not exist: " << dir << "\n";
      return;
    }
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && (entry.path().extension() == ".md" ||
                                      entry.path().extension() == ".txt")) {
        std::ifstream ifs(entry.path());
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        ingest_text(entry.path().filename().string(), content);
      }
    }
  }

  // ── Hybrid 检索 + 可选 GraphRAG 扩展 ───────────────
  std::vector<SearchResult> search(const std::string &query,
                                    size_t top_k = 5,
                                    int graph_hops = 0) {
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
        while (!result.empty()) {
          auto item = result.top();
          auto it = hnsw_id_to_chunk_.find(item.second);
          if (it != hnsw_id_to_chunk_.end()) {
            dense_results.push_back({it->second, item.first});
          }
          result.pop();
        }
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
  std::shared_ptr<kernel::ILLMBackend> llm_;
  BM25Index bm25_;

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
      fused.push_back(
          {chunk_docs_[chunk_id], chunk_id, chunk_content_[chunk_id], score,
           ""});
    }

    std::sort(fused.begin(), fused.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });
    if (fused.size() > top_k) {
      fused.resize(top_k);
    }
    return fused;
  }

  // ── GraphRAG：实体匹配 + K-hop 子图 → 文本上下文 ──
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
