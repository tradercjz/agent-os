#pragma once

#include "agentos/knowledge/tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::knowledge {

/**
 * @brief Okapi BM25 稀疏检索索引（线程安全）
 */
class BM25Index {
public:
  struct Match {
    std::string doc_id;
    double score;
  };

  BM25Index(double k1 = 1.5, double b = 0.75) : k1_(k1), b_(b) {}

  void add_document(const std::string &doc_id, const std::string &text) {
    auto tokens = Tokenizer::instance().cut(text);
    if (tokens.empty())
      return;

    std::unordered_map<std::string, int> term_counts;
    for (const auto &token : tokens) {
      term_counts[token]++;
    }

    std::lock_guard lk(mu_);
    doc_lengths_[doc_id] = tokens.size();
    total_length_ += tokens.size();

    for (const auto &[term, count] : term_counts) {
      inverted_index_[term].push_back({doc_id, count});
    }
  }

  std::vector<Match> search(const std::string &query,
                             size_t top_k = 10) const {
    auto query_tokens = Tokenizer::instance().cut(query);

    std::lock_guard lk(mu_);
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

  size_t size() const {
    std::lock_guard lk(mu_);
    return doc_lengths_.size();
  }

private:
  double k1_;
  double b_;
  size_t total_length_{0};

  mutable std::mutex mu_;
  std::unordered_map<std::string, size_t> doc_lengths_;
  std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>
      inverted_index_;
};

} // namespace agentos::knowledge
