#pragma once

#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef AGENTOS_ENABLE_JIEBA

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cppjieba/Jieba.hpp>
#pragma GCC diagnostic pop

// Ensure the dict path is provided via CMake compile definitions
#ifndef CPPJIEBA_DICT_DIR
#error "CPPJIEBA_DICT_DIR must be defined during compilation"
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#endif // AGENTOS_ENABLE_JIEBA

namespace agentos::knowledge {

/**
 * @brief Singleton tokenizer. When AGENTOS_ENABLE_JIEBA is defined, uses
 * cppjieba for Chinese segmentation. Otherwise falls back to simple
 * whitespace/punctuation splitting.
 */
class Tokenizer {
public:
  static Tokenizer &instance() {
    static Tokenizer instance_;
    return instance_;
  }

  [[nodiscard]]
  std::vector<std::string> cut(const std::string &text) noexcept {
    std::vector<std::string> words;
#ifdef AGENTOS_ENABLE_JIEBA
    if (!is_ready()) {
      // Return empty result if jieba initialization failed
      return words;
    }
    jieba_->CutForSearch(text, words, true);
#else
    // Fallback: split on whitespace and common punctuation
    std::string token;
    for (char c : text) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
          c == ',' || c == '.' || c == '!' || c == '?' ||
          c == ';' || c == ':') {
        if (!token.empty()) {
          words.push_back(token);
          token.clear();
        }
      } else {
        token += c;
      }
    }
    if (!token.empty()) words.push_back(token);
#endif
    return words;
  }

  [[nodiscard]]
  bool is_ready() const noexcept {
#ifdef AGENTOS_ENABLE_JIEBA
    return !init_failed_;
#else
    return true;
#endif
  }

private:
  Tokenizer() noexcept {
#ifdef AGENTOS_ENABLE_JIEBA
    try {
      const std::string dict_path =
          std::string(CPPJIEBA_DICT_DIR) + "/jieba.dict.utf8";
      const std::string hmm_path =
          std::string(CPPJIEBA_DICT_DIR) + "/hmm_model.utf8";
      const std::string user_dict =
          std::string(CPPJIEBA_DICT_DIR) + "/user.dict.utf8";
      const std::string idf_path = std::string(CPPJIEBA_DICT_DIR) + "/idf.utf8";
      const std::string stop_word =
          std::string(CPPJIEBA_DICT_DIR) + "/stop_words.utf8";

      jieba_ = std::make_unique<cppjieba::Jieba>(dict_path, hmm_path, user_dict,
                                                 idf_path, stop_word);
      init_failed_ = false;
    } catch (const std::exception &e) {
      init_failed_ = true;
    }
#endif
  }

  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;

#ifdef AGENTOS_ENABLE_JIEBA
  std::unique_ptr<cppjieba::Jieba> jieba_;
  bool init_failed_{false};
#endif
};

} // namespace agentos::knowledge
