#pragma once
// ============================================================
// AgentOS :: Module 4 — Hindsight Memory Extension
// Adapter for the Vectorize.io Hindsight API
// ============================================================

#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <agentos/memory/memory.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace agentos::memory {

class HindsightMemoryStore : public IMemoryStore {
public:
  explicit HindsightMemoryStore(std::string base_url = "http://localhost:8888",
                                std::string bank_id = "default",
                                std::string api_key = "")
      : base_url_(std::move(base_url)), bank_id_(std::move(bank_id)), api_key_(std::move(api_key)) {}

  ~HindsightMemoryStore() override = default;

  [[nodiscard]] Result<std::string> write(MemoryEntry entry) override {
    if (entry.id.empty()) {
      entry.id = "hs_" + std::to_string(id_counter_++);
    }

    nlohmann::json payload = {
      {"bank_id", bank_id_},
      {"content", entry.content}
    };

    if (!entry.source.empty()) {
      payload["context"] = entry.source;
    }

    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     entry.created_at.time_since_epoch()).count();
    payload["timestamp"] = std::to_string(ts_ms);

    auto req_result = http_post(base_url_ + "/retain", payload.dump());
    if (!req_result) {
      return make_error(ErrorCode::MemoryWriteFailed, "Hindsight retain failed: " + req_result.error().message);
    }

    // Success
    return entry.id;
  }

  [[nodiscard]] Result<MemoryEntry> read(const std::string &id) override {
    (void)id;
    return make_error(ErrorCode::MemoryReadFailed, "Hindsight API does not support read by ID directly via IMemoryStore.");
  }

  [[nodiscard]] Result<bool> forget(const std::string &id) override {
    (void)id;
    return make_error(ErrorCode::MemoryWriteFailed, "Hindsight API does not support forget by ID directly via IMemoryStore.");
  }

  [[nodiscard]] Result<std::vector<SearchResult>> search(const Embedding &q_emb,
                                                         const MemoryFilter &filter,
                                                         size_t top_k = 5) override {
    (void)q_emb;
    (void)filter;
    (void)top_k;
    return make_error(ErrorCode::MemoryReadFailed, "Hindsight API uses semantic text search instead of raw embeddings. Use search_by_text() instead.");
  }

  // To integrate better with systems that might only have embeddings, we provide an empty fallback or require text search
  [[nodiscard]] Result<std::vector<SearchResult>> search_by_text(const std::string &query,
                                                                 const MemoryFilter &filter = {},
                                                                 size_t top_k = 5) {
    (void)filter;
    nlohmann::json payload = {
      {"bank_id", bank_id_},
      {"query", query}
    };

    auto req_result = http_post(base_url_ + "/recall", payload.dump());
    if (!req_result) {
      return make_error(ErrorCode::MemoryReadFailed, "Hindsight recall failed: " + req_result.error().message);
    }

    std::vector<SearchResult> results;
    try {
      auto json_resp = nlohmann::json::parse(*req_result);
      if (json_resp.contains("results") && json_resp["results"].is_array()) {
        for (const auto &item : json_resp["results"]) {
          MemoryEntry entry;
          entry.id = "hs_" + std::to_string(id_counter_++);
          if (item.contains("content") && item["content"].is_string()) {
            entry.content = item["content"].get<std::string>();
          } else if (item.contains("text") && item["text"].is_string()) {
            entry.content = item["text"].get<std::string>();
          }
          entry.created_at = now();
          entry.accessed_at = now();
          entry.importance = 0.8f;
          float score = 0.0f;
          if (item.contains("score") && item["score"].is_number()) {
            score = item["score"].get<float>();
          }
          results.push_back({std::move(entry), score});
        }
      }
    } catch (const std::exception &e) {
      return make_error(ErrorCode::MemoryReadFailed, std::string("Failed to parse Hindsight response: ") + e.what());
    }

    if (results.size() > top_k) {
      results.resize(top_k);
    }

    return results;
  }

  [[nodiscard]] Result<std::string> reflect(const std::string &query) {
    nlohmann::json payload = {
      {"bank_id", bank_id_},
      {"query", query}
    };

    auto req_result = http_post(base_url_ + "/reflect", payload.dump());
    if (!req_result) {
      return make_error(ErrorCode::MemoryReadFailed, "Hindsight reflect failed: " + req_result.error().message);
    }

    try {
      auto json_resp = nlohmann::json::parse(*req_result);
      if (json_resp.contains("answer") && json_resp["answer"].is_string()) {
        return json_resp["answer"].get<std::string>();
      } else if (json_resp.contains("content") && json_resp["content"].is_string()) {
        return json_resp["content"].get<std::string>();
      } else {
        return *req_result; // Return raw response if unknown format
      }
    } catch (const std::exception &e) {
       // Return raw response if it's not valid JSON
       return *req_result;
    }
  }

  std::vector<MemoryEntry> get_all() override {
    return {};
  }

  size_t size() const noexcept override {
    return 0;
  }

  std::string name() const noexcept override { return "HindsightMemoryStore"; }

private:
  static size_t write_string_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *str = static_cast<std::string *>(userp);
    str->append(static_cast<char *>(contents), realsize);
    return realsize;
  }

  Result<std::string> http_post(const std::string &url, const std::string &body) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      return make_error(ErrorCode::MemoryWriteFailed, "Failed to init libcurl");
    }

    std::string response_body;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_key_.empty()) {
      std::string auth_header = "Authorization: Bearer " + api_key_;
      headers = curl_slist_append(headers, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      return make_error(ErrorCode::MemoryWriteFailed, std::string("CURL error: ") + errbuf);
    }

    if (http_code < 200 || http_code >= 300) {
      return make_error(ErrorCode::MemoryWriteFailed, "HTTP error " + std::to_string(http_code) + ": " + response_body);
    }

    return response_body;
  }

  std::string base_url_;
  std::string bank_id_;
  std::string api_key_;
  std::atomic<uint64_t> id_counter_{0};
};

} // namespace agentos::memory
