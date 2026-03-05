#pragma once

#include "agentos/knowledge/tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::knowledge {

/**
 * @brief Okapi BM25 Index for sparse retrieval.
 * Employs a classical Document Inverse Index mapping terms to TF frequencies.
 */
class BM25Index {
public:
  struct Match {
    std::string doc_id;
    double score;
  };

  BM25Index(double k1 = 1.5, double b = 0.75) : k1_(k1), b_(b) {}

  /**
   * @brief Tokenizes and records a document into the inverted index.
   */
  void add_document(const std::string &doc_id, const std::string &text) {
    auto tokens = Tokenizer::instance().cut(text);
    if (tokens.empty())
      return;

    std::unordered_map<std::string, int> term_counts;
    for (const auto &token : tokens) {
      term_counts[token]++;
    }

    doc_lengths_[doc_id] = tokens.size();
    total_length_ += tokens.size();

    for (const auto &[term, count] : term_counts) {
      inverted_index_[term].push_back({doc_id, count});
    }
  }

  /**
   * @brief Performs the Okapi BM25 ranking algorithm against a query string.
   */
  std::vector<Match> search(const std::string &query, size_t top_k = 10) const {
    auto query_tokens = Tokenizer::instance().cut(query);
    std::unordered_map<std::string, double> scores;

    if (doc_lengths_.empty())
      return {};

    double avgdl = static_cast<double>(total_length_) / doc_lengths_.size();
    double N = static_cast<double>(doc_lengths_.size());

    for (const auto &token : query_tokens) {
      auto it = inverted_index_.find(token);
      if (it == inverted_index_.end())
        continue;

      const auto &doc_tfs = it->second;
      double df = doc_tfs.size();

      // Standard BM25 IDF formula with +0.5 smoothing
      double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));

      for (const auto &[doc_id, tf] : doc_tfs) {
        double doc_len = doc_lengths_.at(doc_id);
        double numerator = tf * (k1_ + 1.0);
        double denominator = tf + k1_ * (1.0 - b_ + b_ * (doc_len / avgdl));
        scores[doc_id] += idf * (numerator / denominator);
      }
    }

    std::vector<Match> results;
    results.reserve(scores.size());
    for (const auto &[doc_id, score] : scores) {
      results.push_back({doc_id, score});
    }

    std::sort(results.begin(), results.end(),
              [](const Match &a, const Match &b) { return a.score > b.score; });

    if (results.size() > top_k) {
      results.resize(top_k);
    }

    return results;
  }

  /**
   * @brief Total number of indexed documents
   */
  size_t size() const { return doc_lengths_.size(); }

private:
  double k1_;
  double b_;
  size_t total_length_{0};

  // doc_id -> total token count lengths
  std::unordered_map<std::string, size_t> doc_lengths_;

  // Inverted Index: term -> list of {doc_id, term_freq} combinations
  std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>
      inverted_index_;
};

} // namespace agentos::knowledge
