#pragma once

#include "agentos/knowledge/tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

  /// 移除文档：从倒排索引和 doc_lengths 中清除，修正 total_length
  bool remove_document(const std::string &doc_id) {
    std::lock_guard lk(mu_);
    auto dl_it = doc_lengths_.find(doc_id);
    if (dl_it == doc_lengths_.end())
      return false;

    total_length_ -= dl_it->second;
    doc_lengths_.erase(dl_it);

    // 从所有 posting list 中移除该 doc 的条目
    for (auto term_it = inverted_index_.begin();
         term_it != inverted_index_.end();) {
      auto &postings = term_it->second;
      postings.erase(
          std::remove_if(postings.begin(), postings.end(),
                         [&](const std::pair<std::string, int> &p) {
                           return p.first == doc_id;
                         }),
          postings.end());
      // 清除空 posting list 以保持索引紧凑
      if (postings.empty()) {
        term_it = inverted_index_.erase(term_it);
      } else {
        ++term_it;
      }
    }
    return true;
  }

  size_t size() const {
    std::lock_guard lk(mu_);
    return doc_lengths_.size();
  }

  // ── 持久化 ───────────────────────────────────────
  bool save(const std::filesystem::path &path) const {
    std::lock_guard lk(mu_);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
      return false;

    // Header
    uint32_t magic = 0x424D3235; // "BM25"
    ofs.write(reinterpret_cast<const char *>(&magic), 4);
    ofs.write(reinterpret_cast<const char *>(&k1_), sizeof(k1_));
    ofs.write(reinterpret_cast<const char *>(&b_), sizeof(b_));
    ofs.write(reinterpret_cast<const char *>(&total_length_),
              sizeof(total_length_));

    // doc_lengths_
    uint32_t n_docs = static_cast<uint32_t>(doc_lengths_.size());
    ofs.write(reinterpret_cast<const char *>(&n_docs), 4);
    for (const auto &[doc_id, len] : doc_lengths_) {
      uint32_t slen = static_cast<uint32_t>(doc_id.size());
      ofs.write(reinterpret_cast<const char *>(&slen), 4);
      ofs.write(doc_id.data(), slen);
      ofs.write(reinterpret_cast<const char *>(&len), sizeof(len));
    }

    // inverted_index_
    uint32_t n_terms = static_cast<uint32_t>(inverted_index_.size());
    ofs.write(reinterpret_cast<const char *>(&n_terms), 4);
    for (const auto &[term, postings] : inverted_index_) {
      uint32_t tlen = static_cast<uint32_t>(term.size());
      ofs.write(reinterpret_cast<const char *>(&tlen), 4);
      ofs.write(term.data(), tlen);

      uint32_t n_postings = static_cast<uint32_t>(postings.size());
      ofs.write(reinterpret_cast<const char *>(&n_postings), 4);
      for (const auto &[doc_id, tf] : postings) {
        uint32_t dlen = static_cast<uint32_t>(doc_id.size());
        ofs.write(reinterpret_cast<const char *>(&dlen), 4);
        ofs.write(doc_id.data(), dlen);
        ofs.write(reinterpret_cast<const char *>(&tf), sizeof(tf));
      }
    }

    return ofs.good();
  }

  bool load(const std::filesystem::path &path) {
    std::lock_guard lk(mu_);
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
      return false;

    uint32_t magic;
    ifs.read(reinterpret_cast<char *>(&magic), 4);
    if (magic != 0x424D3235)
      return false;

    ifs.read(reinterpret_cast<char *>(&k1_), sizeof(k1_));
    ifs.read(reinterpret_cast<char *>(&b_), sizeof(b_));
    ifs.read(reinterpret_cast<char *>(&total_length_), sizeof(total_length_));

    // doc_lengths_
    uint32_t n_docs;
    ifs.read(reinterpret_cast<char *>(&n_docs), 4);
    doc_lengths_.clear();
    doc_lengths_.reserve(n_docs);
    for (uint32_t i = 0; i < n_docs; ++i) {
      uint32_t slen;
      ifs.read(reinterpret_cast<char *>(&slen), 4);
      std::string doc_id(slen, '\0');
      ifs.read(doc_id.data(), slen);
      size_t len;
      ifs.read(reinterpret_cast<char *>(&len), sizeof(len));
      doc_lengths_[doc_id] = len;
    }

    // inverted_index_
    uint32_t n_terms;
    ifs.read(reinterpret_cast<char *>(&n_terms), 4);
    inverted_index_.clear();
    inverted_index_.reserve(n_terms);
    for (uint32_t i = 0; i < n_terms; ++i) {
      uint32_t tlen;
      ifs.read(reinterpret_cast<char *>(&tlen), 4);
      std::string term(tlen, '\0');
      ifs.read(term.data(), tlen);

      uint32_t n_postings;
      ifs.read(reinterpret_cast<char *>(&n_postings), 4);
      std::vector<std::pair<std::string, int>> postings;
      postings.reserve(n_postings);
      for (uint32_t j = 0; j < n_postings; ++j) {
        uint32_t dlen;
        ifs.read(reinterpret_cast<char *>(&dlen), 4);
        std::string doc_id(dlen, '\0');
        ifs.read(doc_id.data(), dlen);
        int tf;
        ifs.read(reinterpret_cast<char *>(&tf), sizeof(tf));
        postings.push_back({std::move(doc_id), tf});
      }
      inverted_index_[std::move(term)] = std::move(postings);
    }

    return ifs.good();
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
