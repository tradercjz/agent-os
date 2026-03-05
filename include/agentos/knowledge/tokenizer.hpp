#pragma once

#include <cppjieba/Jieba.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Ensure the dict path is provided via CMake compile definitions
#ifndef CPPJIEBA_DICT_DIR
#error "CPPJIEBA_DICT_DIR must be defined during compilation"
#endif

// We expand the macro to a const char*
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

namespace agentos::knowledge {

/**
 * @brief Singleton wrapper around cppjieba to prevent multiple initializations.
 * cppjieba takes ~500MB RAM and 1s to boot reading dictionaries, so it must be
 * shared.
 */
class Tokenizer {
public:
  static Tokenizer &instance() {
    static Tokenizer instance_;
    return instance_;
  }

  // Thread-safe segmentation
  std::vector<std::string> cut(const std::string &text) {
    std::vector<std::string> words;
    // jieba->CutForSearch internally does read operations, which are
    // thread-safe.
    jieba_->CutForSearch(text, words, true);
    return words;
  }

private:
  std::unique_ptr<cppjieba::Jieba> jieba_;

  Tokenizer() {
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
  }

  // Disable copy/move
  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;
};

} // namespace agentos::knowledge
