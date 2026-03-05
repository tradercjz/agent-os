#pragma once

#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/bm25_index.hpp"
#include "agentos/knowledge/document.hpp"
#include <filesystem>
#include <fstream>
#include <hnswlib/hnswlib.h>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace agentos::knowledge {

/**
 * @brief Search Result returned by the Hybrid RRF search.
 */
struct SearchResult {
  std::string doc_id;
  std::string chunk_id;
  std::string content;
  double score; // Combined Reciprocal Rank Fusion Score
};

/**
 * @brief A turnkey RAG API facade handling parsing, hybrid chunk indexing,
 *        dense LLM embedding, sparse tokenization, and RRF fusions.
 */
class KnowledgeBase {
public:
  /**
   * @brief Initialize standard KnowledgeBase routing to a specified backend
   * @param llm Instance of the backend used to synthesize dense embeddings
   * @param vector_dim Dimensionality of the target embed model
   */
  explicit KnowledgeBase(std::shared_ptr<kernel::ILLMBackend> llm,
                         uint32_t vector_dim = 1536)
      : llm_(std::move(llm)), dim_(vector_dim) {
    space_ = std::make_unique<hnswlib::InnerProductSpace>(dim_);
    // Max capacity bounded to 100k chunks for this embedded session
    hnsw_ =
        std::make_unique<hnswlib::HierarchicalNSW<float>>(space_.get(), 100000);
  }

  /**
   * @brief Raw text injestion endpoint
   */
  void ingest_text(const std::string &doc_id, const std::string &text) {
    // 1. naive sliding window chunking
    auto chunks = chunk_text(text, 500, 50);
    std::cout << "[KB Ingestion] Document [" << doc_id << "] parsed into "
              << chunks.size() << " chunks.\n";

    // 2. Index into Sparse and Dense spaces concurrently
    for (size_t i = 0; i < chunks.size(); ++i) {
      std::string chunk_id = doc_id + "_chunk_" + std::to_string(i);

      // Sparse Tokenization (BM25)
      bm25_.add_document(chunk_id, chunks[i]);

      chunk_content_[chunk_id] = chunks[i];
      chunk_docs_[chunk_id] = doc_id;

      // Dense Embedding (HNSW Vector Space)
      kernel::EmbeddingRequest req;
      req.inputs = {chunks[i]};
      req.model = "text-embedding-3-small"; // Model agnostic interface

      auto resp = llm_->embed(req);
      if (resp && !resp->embeddings.empty()) {
        hnsw_->addPoint(resp->embeddings[0].data(), next_hnsw_id_);
        hnsw_id_to_chunk_[next_hnsw_id_] = chunk_id;
        next_hnsw_id_++;
      } else {
        std::cerr << "[KB Ingestion] Warning: Backend failed to generate "
                     "embedding for chunk.\n";
      }
    }
  }

  /**
   * @brief Turnkey API: Reads all md/txt files locally and indexes them into
   * Agent Memory
   */
  void ingest_directory(const fs::path &dir) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      std::cerr << "[KB Error] Ingestion directory does not exist: " << dir
                << "\n";
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

  /**
   * @brief Hybrid Multi-Way Retrieval with RRF score balancing.
   */
  std::vector<SearchResult> search(const std::string &query, size_t top_k = 5) {
    const size_t internal_k = top_k * 2; // Fetch more for better fusion ranking

    // 1. Branch Sparse (BM25 Token Graph)
    auto bm25_results = bm25_.search(query, internal_k);

    // 2. Branch Dense (HNSW Nearest Neighbors)
    std::vector<std::pair<std::string, double>> dense_results;
    kernel::EmbeddingRequest req;
    req.inputs = {query};
    req.model = "text-embedding-3-small";
    auto emb_resp = llm_->embed(req);

    if (emb_resp && !emb_resp->embeddings.empty()) {
      size_t available_nodes = hnsw_->cur_element_count;
      size_t k = std::min(internal_k, available_nodes);

      if (k > 0) {
        auto result = hnsw_->searchKnn(emb_resp->embeddings[0].data(), k);
        while (!result.empty()) {
          auto item = result.top();
          dense_results.push_back({hnsw_id_to_chunk_[item.second], item.first});
          result.pop();
        }
        // HNSWlib searchKnn returns a max-heap where the top element is the
        // worst match among the k closest. Popping yields worst-to-best order.
        // We must reverse to get best-to-worst (rank 1 at index 0) for RRF.
        std::reverse(dense_results.begin(), dense_results.end());
      }
    }

    // 3. RRF Fusion Blend
    return rrf_fuse(bm25_results, dense_results, top_k);
  }

private:
  std::shared_ptr<kernel::ILLMBackend> llm_;
  BM25Index bm25_;

  // HNSW Topology Tracking
  uint32_t dim_;
  std::unique_ptr<hnswlib::SpaceInterface<float>> space_;
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_;
  hnswlib::labeltype next_hnsw_id_{0};

  // Virtual Pointer mappings for UUIDs across memory engines
  std::unordered_map<hnswlib::labeltype, std::string> hnsw_id_to_chunk_;
  std::unordered_map<std::string, std::string> chunk_content_;
  std::unordered_map<std::string, std::string> chunk_docs_;

  /**
   * @brief Helper to slice text using a simple character-based sliding window.
   * In a production system, this could be upgraded to semantic or recursive
   * splitting.
   */
  std::vector<std::string> chunk_text(const std::string &text,
                                      size_t chunk_size, size_t overlap) {
    std::vector<std::string> chunks;
    if (text.empty())
      return chunks;

    // Ensure forward progress
    size_t stride = (chunk_size > overlap) ? (chunk_size - overlap) : 1;

    for (size_t i = 0; i < text.size(); i += stride) {
      chunks.push_back(text.substr(i, chunk_size));
    }
    return chunks;
  }

  /**
   * @brief Reciprocal Rank Fusion Core Algorithm
   * Score = Sum(1 / (k + rank)) across multiple modalities.
   */
  std::vector<SearchResult>
  rrf_fuse(const std::vector<BM25Index::Match> &sparse,
           const std::vector<std::pair<std::string, double>> &dense,
           size_t top_k) {
    // Industry standard k parameter for RRF is ~60 smoothing offset
    const double k_fusion = 60.0;
    std::unordered_map<std::string, double> rrf_scores;

    // Overlay Sparse Route
    for (size_t i = 0; i < sparse.size(); ++i) {
      rrf_scores[sparse[i].doc_id] += 1.0 / (k_fusion + i + 1);
    }

    // Overlay Dense Route
    for (size_t i = 0; i < dense.size(); ++i) {
      rrf_scores[dense[i].first] += 1.0 / (k_fusion + i + 1);
    }

    // Collapse modalities downwards
    std::vector<SearchResult> fused;
    for (const auto &[chunk_id, score] : rrf_scores) {
      fused.push_back(
          {chunk_docs_[chunk_id], chunk_id, chunk_content_[chunk_id], score});
    }

    // Re-order highest score first
    std::sort(fused.begin(), fused.end(),
              [](const auto &a, const auto &b) { return a.score > b.score; });

    if (fused.size() > top_k) {
      fused.resize(top_k);
    }

    return fused;
  }
};

} // namespace agentos::knowledge
