#include <agentos/kernel/llm_kernel.hpp>
#include <algorithm>
#include <curl/curl.h>
#include <stdexcept>
#include <string>
#include <string_view>

// ============================================================
// 文件局部 RAII 辅助类（libcurl 资源管理）
// ============================================================

namespace {

/// RAII wrapper for CURL* easy handle
struct CurlHandle {
  CURL *handle = nullptr;
  char errbuf[CURL_ERROR_SIZE] = {};

  CurlHandle() : handle(curl_easy_init()) {
    if (handle) {
      curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errbuf);
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L); // 多线程安全
      curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L); // TLS peer verification
      curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L); // hostname verification
    }
  }
  ~CurlHandle() {
    if (handle)
      curl_easy_cleanup(handle);
  }

  CurlHandle(const CurlHandle &) = delete;
  CurlHandle &operator=(const CurlHandle &) = delete;

  explicit operator bool() const { return handle != nullptr; }
  CURL *get() const { return handle; }
};

/// RAII wrapper for curl_slist* header list
struct CurlHeaders {
  curl_slist *list = nullptr;

  CurlHeaders() = default;

  void append(const char *header) {
    list = curl_slist_append(list, header);
  }
  ~CurlHeaders() {
    if (list)
      curl_slist_free_all(list);
  }

  CurlHeaders(const CurlHeaders &) = delete;
  CurlHeaders &operator=(const CurlHeaders &) = delete;
};

/// 标准 write callback：将响应数据追加到 std::string
size_t write_string_callback(void *contents, size_t size, size_t nmemb,
                              void *userp) {
  auto *buf = static_cast<std::string *>(userp);
  size_t total = size * nmemb;
  buf->append(static_cast<char *>(contents), total);
  return total;
}

// ── SSE 流式回调上下文 ──────────────────────────────────────

struct StreamContext {
  agentos::kernel::ILLMBackend::TokenCallback token_cb;
  agentos::kernel::LLMResponse *response;
  std::string line_buffer; // 行缓冲（处理 SSE 分块到达）

  // 工具调用累积状态
  std::string current_tool_id;
  std::string current_tool_name;
  std::string current_tool_args;
  bool done = false;

  // Synchronization for response modification
  // Note: libcurl's easy interface is sequential, not truly async, but we add
  // proper synchronization for correctness and thread-safety.
  std::mutex response_mu;

  /// 处理一行完整的 SSE 数据
  void process_sse_line(std::string_view line) {
    using namespace agentos;
    using namespace agentos::kernel;

    if (!line.starts_with("data: "))
      return;
    auto data = line.substr(6);
    if (data == "[DONE]") {
      done = true;
      return;
    }

    try {
      Json j = Json::parse(data);
      if (!j.contains("choices") || !j["choices"].is_array() ||
          j["choices"].empty())
        return;

      auto &choice = j["choices"][0];

      // 解析 delta 内容
      if (choice.contains("delta")) {
        auto &delta = choice["delta"];

        // 文本内容
        if (delta.contains("content") && delta["content"].is_string()) {
          std::string text = delta["content"].get<std::string>();
          if (token_cb)
            token_cb(text);
          {
            std::lock_guard lk(response_mu);
            response->content += text;
          }
        }

        // 工具调用（增量式）
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array() &&
            !delta["tool_calls"].empty()) {
          auto &tc = delta["tool_calls"][0];
          if (tc.contains("id") && tc["id"].is_string())
            current_tool_id = tc["id"].get<std::string>();
          if (tc.contains("function")) {
            auto &func = tc["function"];
            if (func.contains("name") && func["name"].is_string())
              current_tool_name = func["name"].get<std::string>();
            if (func.contains("arguments") && func["arguments"].is_string())
              current_tool_args += func["arguments"].get<std::string>();
          }
        }
      }

      // finish_reason
      if (choice.contains("finish_reason") &&
          choice["finish_reason"].is_string()) {
        std::string fr = choice["finish_reason"].get<std::string>();
        {
          std::lock_guard lk(response_mu);
          response->finish_reason = fr;
        }
        if (fr == "tool_calls" && !current_tool_name.empty()) {
          ToolCallRequest tcr;
          tcr.id = current_tool_id.empty() ? "call_0" : current_tool_id;
          tcr.name = current_tool_name;
          tcr.args_json = current_tool_args;
          {
            std::lock_guard lk(response_mu);
            response->tool_calls.push_back(std::move(tcr));
          }
          // 重置累積状態（支持多個 tool call）
          current_tool_id.clear();
          current_tool_name.clear();
          current_tool_args.clear();
        }
      }

      // usage（stream_options.include_usage=true 时返回）
      if (j.contains("usage")) {
        auto &usage = j["usage"];
        if (usage.contains("prompt_tokens") &&
            usage["prompt_tokens"].is_number_integer()) {
          std::lock_guard lk(response_mu);
          response->prompt_tokens = static_cast<uint32_t>(std::max(0, usage["prompt_tokens"].get<int>()));
        }
        if (usage.contains("completion_tokens") &&
            usage["completion_tokens"].is_number_integer()) {
          std::lock_guard lk(response_mu);
          response->completion_tokens = static_cast<uint32_t>(std::max(0, usage["completion_tokens"].get<int>()));
        }
      }
    } catch (const std::exception &) {
      // 忽略单个 SSE chunk 的解析错误，继续处理后续数据
    }
  }
};

/// SSE 流式 write callback：行缓冲 + 逐行处理
size_t stream_write_callback(void *contents, size_t size, size_t nmemb,
                              void *userp) {
  auto *ctx = static_cast<StreamContext *>(userp);
  size_t total = size * nmemb;

  // Safety: cap line buffer at 1 MiB to prevent DoS via no-newline streams
  constexpr size_t MAX_LINE_BUFFER = 1024 * 1024;
  {
    std::lock_guard lk(ctx->response_mu);
    if (ctx->line_buffer.size() + total > MAX_LINE_BUFFER) {
      ctx->response->content += "[stream error: line buffer overflow]";
      ctx->done = true;
      return 0; // Signal curl to abort
    }
  }

  ctx->line_buffer.append(static_cast<char *>(contents), total);

  // 处理所有完整行
  size_t pos;
  while ((pos = ctx->line_buffer.find('\n')) != std::string::npos) {
    std::string line = ctx->line_buffer.substr(0, pos);
    ctx->line_buffer.erase(0, pos + 1);

    // 去除 \r
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;

    ctx->process_sse_line(line);
    if (ctx->done)
      return total; // 收到 [DONE]，让 curl 结束
  }

  return total;
}

} // anonymous namespace

// ============================================================
// OpenAIBackend 实现
// ============================================================

namespace agentos::kernel {

std::string OpenAIBackend::build_request_json(const LLMRequest &req) const {
  Json root_obj = Json::object();

  Json messages_arr = Json::array();
  for (auto &m : req.messages) {
    Json msg_obj = Json::object();
    switch (m.role) {
    case Role::System:
      msg_obj["role"] = "system";
      break;
    case Role::User:
      msg_obj["role"] = "user";
      break;
    case Role::Assistant:
      msg_obj["role"] = "assistant";
      break;
    case Role::Tool:
      msg_obj["role"] = "tool";
      break;
    }
    if (m.role == Role::Tool) {
      msg_obj["content"] = m.content;
      msg_obj["tool_call_id"] = m.tool_call_id;
    } else if (m.role == Role::Assistant) {
      if (m.content.empty()) {
        msg_obj["content"] = nullptr;
      } else {
        msg_obj["content"] = m.content;
      }
      if (!m.tool_calls.empty()) {
        Json tool_calls_arr = Json::array();
        for (const auto &tc : m.tool_calls) {
          Json call_obj = Json::object();
          call_obj["id"] = tc.id;
          call_obj["type"] = "function";
          Json func_obj = Json::object();
          func_obj["name"] = tc.name;
          func_obj["arguments"] = tc.args_json;
          call_obj["function"] = func_obj;
          tool_calls_arr.push_back(call_obj);
        }
        msg_obj["tool_calls"] = tool_calls_arr;
      }
    } else {
      msg_obj["content"] = m.content;
    }
    messages_arr.push_back(msg_obj);
  }

  root_obj["messages"] = messages_arr;
  const std::string &model = req.model.empty() ? default_model_ : req.model;
  root_obj["model"] = model;
  root_obj["temperature"] = req.temperature;
  root_obj["max_tokens"] = req.max_tokens;

  if (req.tools_json && !req.tools_json->empty()) {
    try {
      root_obj["tools"] = Json::parse(*req.tools_json);
      root_obj["tool_choice"] = "auto";
    } catch (const std::exception &e) {
      LOG_WARN(fmt::format("Failed to parse tools_json: {}", e.what()));
      // Proceed without tools rather than crash
    }
  }

  return root_obj.dump();
}

Result<LLMResponse>
OpenAIBackend::parse_response(const std::string &json_str) const {
  LLMResponse resp;
  resp.finish_reason = "stop";
  try {
    Json j = Json::parse(json_str);

    // Check for API error in response body (e.g., content filtering)
    if (j.contains("error") && j["error"].is_object()) {
      auto &err = j["error"];
      std::string msg = err.value("message", "Unknown API error");
      return make_error(ErrorCode::LLMBackendError,
                        fmt::format("API error: {}", msg));
    }

    if (j.contains("choices") && j["choices"].is_array() &&
        !j["choices"].empty()) {
      auto &choice = j["choices"][0];
      if (choice.contains("message")) {
        auto &msg = choice["message"];
        if (msg.contains("content") && msg["content"].is_string()) {
          resp.content = msg["content"].get<std::string>();
        } else {
          resp.content = "";
        }
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
          for (auto &tc : msg["tool_calls"]) {
            if (tc.contains("id") && tc.contains("function")) {
              ToolCallRequest tcr;
              tcr.id = tc["id"].get<std::string>();
              auto &func = tc["function"];
              if (func.contains("name") && func["name"].is_string())
                tcr.name = func["name"].get<std::string>();
              if (func.contains("arguments") && func["arguments"].is_string())
                tcr.args_json = func["arguments"].get<std::string>();
              resp.tool_calls.push_back(std::move(tcr));
            }
          }
        }
      }
      if (choice.contains("finish_reason") &&
          choice["finish_reason"].is_string()) {
        resp.finish_reason = choice["finish_reason"].get<std::string>();
      }
    }

    if (j.contains("usage")) {
      auto &usage = j["usage"];
      if (usage.contains("prompt_tokens") &&
          usage["prompt_tokens"].is_number_integer())
        resp.prompt_tokens = static_cast<uint32_t>(std::max(0, usage["prompt_tokens"].get<int>()));
      if (usage.contains("completion_tokens") &&
          usage["completion_tokens"].is_number_integer())
        resp.completion_tokens = static_cast<uint32_t>(std::max(0, usage["completion_tokens"].get<int>()));
    }
  } catch (const Json::exception &e) {
    return make_error(ErrorCode::LLMBackendError,
                      fmt::format("JSON Parse Error: {}", e.what()));
  }

  return resp;
}

// ── http_post: 使用 libcurl C API（替代 popen）──────────────

Result<std::string> OpenAIBackend::http_post(const std::string &endpoint,
                                             const std::string &body) const {
  CurlHandle curl;
  if (!curl)
    return make_error(ErrorCode::LLMBackendError, "Failed to init libcurl");

  std::string url = base_url_ + endpoint;
  std::string response_body;

  // 构建 HTTP 头
  CurlHeaders headers;
  headers.append("Content-Type: application/json");
  std::string auth_header = "Authorization: Bearer " + api_key_;
  headers.append(auth_header.c_str());

  // 配置 curl 选项
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_string_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);

  // 执行请求
  CURLcode res = curl_easy_perform(curl.get());
  if (res != CURLE_OK) {
    return make_error(
        ErrorCode::LLMBackendError,
        fmt::format("HTTP request failed: {} ({})", curl.errbuf,
                    curl_easy_strerror(res)));
  }

  // 检查 HTTP 状态码
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code >= 400) {
    // Parse API error response for better error messages
    std::string error_msg;
    try {
      Json err_json = Json::parse(response_body);
      if (err_json.contains("error") && err_json["error"].is_object()) {
        auto &err_obj = err_json["error"];
        if (err_obj.contains("message") && err_obj["message"].is_string())
          error_msg = err_obj["message"].get<std::string>();
      }
    } catch (...) { /* not JSON, use raw body */ }

    if (error_msg.empty())
      error_msg = response_body.substr(0, 500); // Truncate long error bodies

    // Distinguish transient from permanent errors
    ErrorCode code = ErrorCode::LLMBackendError;
    if (http_code == 429) {
      code = ErrorCode::RateLimitExceeded;
    }

    return make_error(
        code,
        fmt::format("HTTP {} error: {}", http_code, error_msg));
  }

  return response_body;
}

// ── complete: 同步推理 ─────────────────────────────────────

Result<LLMResponse> OpenAIBackend::complete(const LLMRequest &req) {
  std::string body = build_request_json(req);
  auto http_result = http_post("/chat/completions", body);
  if (!http_result)
    return make_unexpected(http_result.error());
  return parse_response(*http_result);
}

// ── stream: SSE 流式推理（libcurl + 行缓冲回调）────────────

Result<LLMResponse> OpenAIBackend::stream(const LLMRequest &req,
                                          TokenCallback cb) {
  // 构建请求体，注入 stream 标志（通过 JSON 库安全修改）
  std::string body;
  try {
    Json j = Json::parse(build_request_json(req));
    j["stream"] = true;
    j["stream_options"] = {{"include_usage", true}};
    body = j.dump();
  } catch (const std::exception &e) {
    return make_error(ErrorCode::LLMBackendError,
                      fmt::format("Failed to build stream request: {}", e.what()));
  }

  CurlHandle curl;
  if (!curl)
    return make_error(ErrorCode::LLMBackendError, "Failed to init libcurl");

  std::string url = base_url_ + "/chat/completions";

  CurlHeaders headers;
  headers.append("Content-Type: application/json");
  std::string auth_header = "Authorization: Bearer " + api_key_;
  headers.append(auth_header.c_str());

  LLMResponse resp;
  resp.finish_reason = "stop";

  StreamContext sctx;
  sctx.token_cb = std::move(cb);
  sctx.response = &resp;

  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, stream_write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sctx);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl.get());

  // CURLE_WRITE_ERROR 在 [DONE] 后 callback 返回提前终止是正常的
  if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
    // 检查是否已收到完整数据（done=true 说明流正常结束）
    if (!sctx.done) {
      return make_error(
          ErrorCode::LLMBackendError,
          fmt::format("Stream request failed: {} ({})", curl.errbuf,
                      curl_easy_strerror(res)));
    }
  }

  // 若流未返回 usage（旧版 API），用启发式估算
  if (resp.prompt_tokens == 0) {
    for (auto &m : req.messages)
      resp.prompt_tokens += ILLMBackend::estimate_tokens(m.content);
  }
  if (resp.completion_tokens == 0) {
    resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);
  }

  return resp;
}

// ── embed: 向量嵌入 ────────────────────────────────────────

Result<EmbeddingResponse> OpenAIBackend::embed(const EmbeddingRequest &req) {
  Json root_obj = Json::object();
  Json inputs_arr = Json::array();
  for (const auto &in : req.inputs) {
    inputs_arr.push_back(in);
  }
  const std::string &model =
      req.model.empty() ? "text-embedding-3-small" : req.model;
  root_obj["model"] = model;
  root_obj["input"] = inputs_arr;

  std::string body = root_obj.dump();

  auto http_result = http_post("/embeddings", body);
  if (!http_result)
    return make_unexpected(http_result.error());

  const std::string &json_str = *http_result;
  EmbeddingResponse resp;

  try {
    Json j = Json::parse(json_str);
    if (j.contains("usage") && j["usage"].contains("total_tokens") &&
        j["usage"]["total_tokens"].is_number_integer()) {
      resp.total_tokens = static_cast<uint32_t>(std::max(0, j["usage"]["total_tokens"].get<int>()));
    }
    if (j.contains("data") && j["data"].is_array()) {
      for (auto &item : j["data"]) {
        if (item.contains("embedding") && item["embedding"].is_array()) {
          std::vector<float> vec;
          vec.reserve(item["embedding"].size());
          for (auto &v : item["embedding"]) {
            if (v.is_number()) {
              vec.push_back(v.get<float>());
            }
          }
          resp.embeddings.push_back(std::move(vec));
        }
      }
    }
  } catch (const std::exception &) {
    return make_error(ErrorCode::LLMBackendError,
                      "Failed to parse embeddings JSON");
  }

  return resp;
}

} // namespace agentos::kernel
