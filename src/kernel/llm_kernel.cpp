#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/kernel/http_client.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

// ============================================================
// SSE streaming context (OpenAI-specific)
// ============================================================

namespace {

struct StreamContext {
  agentos::kernel::ILLMBackend::TokenCallback token_cb;
  agentos::kernel::LLMResponse *response;
  std::string line_buffer; // line buffer for SSE chunked delivery

  // Tool call accumulation state
  std::string current_tool_id;
  std::string current_tool_name;
  std::string current_tool_args;
  bool done = false;

  // R7-15: Stream completion validation
  bool stream_done{false};  // Set true when "data: [DONE]" is received

  // Synchronization for response modification
  std::mutex response_mu;

  /// Process a complete SSE data line
  void process_sse_line(std::string_view line) {
    using namespace agentos;
    using namespace agentos::kernel;

    if (!line.starts_with("data: "))
      return;
    auto data = line.substr(6);
    if (data == "[DONE]") {
      done = true;
      // R7-15: Mark stream as properly terminated
      stream_done = true;
      return;
    }

    try {
      Json j = Json::parse(data);
      if (!j.contains("choices") || !j["choices"].is_array() ||
          j["choices"].empty())
        return;

      auto &choice = j["choices"][0];

      // Parse delta content
      if (choice.contains("delta")) {
        auto &delta = choice["delta"];

        // Text content
        if (delta.contains("content") && delta["content"].is_string()) {
          std::string text = delta["content"].get<std::string>();
          if (token_cb)
            token_cb(text);
          {
            std::lock_guard lk(response_mu);
            response->content += text;
          }
        }

        // Tool calls (incremental)
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
          current_tool_id.clear();
          current_tool_name.clear();
          current_tool_args.clear();
        }
      }

      // usage (returned when stream_options.include_usage=true)
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
    } catch (const std::exception &e) {
      LOG_WARN(fmt::format("[SSE] JSON parse error on chunk: {}", e.what()));
    }
  }
};

/// SSE streaming write callback: line-buffered processing.
/// Returns chunk size to the HttpClient's on_data callback.
size_t sse_line_callback(const char *contents, size_t total, StreamContext *ctx) {
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

  ctx->line_buffer.append(contents, total);

  // Process all complete lines
  size_t pos;
  while ((pos = ctx->line_buffer.find('\n')) != std::string::npos) {
    std::string line = ctx->line_buffer.substr(0, pos);
    ctx->line_buffer.erase(0, pos + 1);

    // Strip \r
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;

    ctx->process_sse_line(line);
    if (ctx->done)
      return total; // Received [DONE], let curl finish
  }

  return total;
}

} // anonymous namespace

// ============================================================
// R7-16: Redaction helper for sensitive data in error messages
// ============================================================

namespace {

// Redact sensitive headers and API keys from error messages before logging
static std::string redact_auth(const std::string &msg) {
    std::string result = msg;
    // Redact "Bearer xxx..." patterns
    static const std::string bearer = "Bearer ";
    auto pos = result.find(bearer);
    while (pos != std::string::npos) {
        auto end = result.find_first_of(" \r\n\"'}", pos + bearer.size());
        if (end == std::string::npos) end = result.size();
        result.replace(pos + bearer.size(), end - pos - bearer.size(), "***REDACTED***");
        pos = result.find(bearer, pos + bearer.size() + 14);
    }
    // Redact "sk-..." API key patterns
    auto sk_pos = result.find("sk-");
    while (sk_pos != std::string::npos) {
        auto end = result.find_first_of(" \r\n\"'}", sk_pos);
        if (end == std::string::npos) end = result.size();
        result.replace(sk_pos, end - sk_pos, "sk-***REDACTED***");
        sk_pos = result.find("sk-", sk_pos + 17);
    }
    return result;
}

} // anonymous namespace

// ============================================================
// OpenAIBackend implementation
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

// ── http_post: now delegates to HttpClient ──────────────────

Result<std::string> OpenAIBackend::http_post(const std::string &endpoint,
                                             const std::string &body) const {
  std::string url = base_url_ + endpoint;

  // Build HTTP headers
  std::vector<std::string> headers;
  headers.emplace_back("Content-Type: application/json");
  headers.emplace_back("Authorization: Bearer " + api_key_);

  auto result = http_client_.post(url, body, headers, 60);
  if (!result) {
    // R7-16: Redact sensitive data from error messages
    return make_error(result.error().code,
                      redact_auth(result.error().message));
  }

  long http_code = result->status_code;

  if (http_code == 401) {
    return make_error(ErrorCode::LLMBackendError,
        "OpenAI API: authentication failed (check API key)");
  } else if (http_code == 429) {
    return make_error(ErrorCode::RateLimitExceeded,
        fmt::format("OpenAI API: rate limited (HTTP 429): {}",
                    redact_auth(result->body.substr(0, 500))));
  } else if (http_code >= 500 && http_code < 600) {
    return make_error(ErrorCode::LLMBackendError,
        fmt::format("OpenAI API: server error (HTTP {}): {}",
                    http_code, redact_auth(result->body.substr(0, 500))));
  } else if (http_code >= 400) {
    // Parse API error response for better error messages
    std::string error_msg;
    try {
      Json err_json = Json::parse(result->body);
      if (err_json.contains("error") && err_json["error"].is_object()) {
        auto &err_obj = err_json["error"];
        if (err_obj.contains("message") && err_obj["message"].is_string())
          error_msg = err_obj["message"].get<std::string>();
      }
    } catch (...) { /* not JSON, use raw body */ }

    if (error_msg.empty())
      error_msg = result->body.substr(0, 500);

    return make_error(ErrorCode::LLMBackendError,
        fmt::format("OpenAI API: unexpected status (HTTP {}): {}",
                    http_code, redact_auth(error_msg)));
  }

  return result->body;
}

// ── complete: synchronous inference ─────────────────────────

Result<LLMResponse> OpenAIBackend::complete(const LLMRequest &req) {
  // Auto-generate request_id for tracing if not provided
  LLMRequest req_copy = req;
  if (req_copy.request_id.empty()) {
    static std::atomic<uint64_t> req_counter{0};
    req_copy.request_id = fmt::format("req_{:x}_{}",
        std::chrono::steady_clock::now().time_since_epoch().count(),
        req_counter.fetch_add(1, std::memory_order_relaxed));
  }

  LOG_INFO(fmt::format("[LLM:{}] Sending request to /chat/completions", req_copy.request_id));
  std::string body = build_request_json(req_copy);
  auto http_result = http_post("/chat/completions", body);
  if (!http_result) {
    LOG_ERROR(fmt::format("[LLM:{}] Request failed: {}", req_copy.request_id, http_result.error().message));
    return make_unexpected(http_result.error());
  }
  LOG_INFO(fmt::format("[LLM:{}] Request completed", req_copy.request_id));
  return parse_response(*http_result);
}

// ── stream: SSE streaming inference ─────────────────────────

Result<LLMResponse> OpenAIBackend::stream(const LLMRequest &req,
                                          TokenCallback cb) {
  // Build request body with stream flag
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

  std::string url = base_url_ + "/chat/completions";

  std::vector<std::string> headers;
  headers.emplace_back("Content-Type: application/json");
  headers.emplace_back("Authorization: Bearer " + api_key_);

  LLMResponse resp;
  resp.finish_reason = "stop";

  StreamContext sctx;
  sctx.token_cb = cb;  // copy, not move — cb may still be needed by caller
  sctx.response = &resp;

  auto stream_result = http_client_.post_stream(
      url, body, headers,
      [&sctx](const char *data, size_t len) -> size_t {
        return sse_line_callback(data, len, &sctx);
      },
      120);

  if (!stream_result) {
    // Check if we already received complete data
    if (!sctx.done) {
      return make_error(stream_result.error().code,
                        redact_auth(stream_result.error().message));
    }
  }

  // R7-15: Validate that stream was properly terminated with [DONE] marker
  if (stream_result && !sctx.stream_done) {
    LOG_WARN(fmt::format("[LLM] Stream ended without [DONE] marker — response may be incomplete"));
  }

  // If stream didn't return usage (older API), use heuristic estimates
  if (resp.prompt_tokens == 0) {
    for (auto &m : req.messages)
      resp.prompt_tokens += ILLMBackend::estimate_tokens(m.content);
  }
  if (resp.completion_tokens == 0) {
    resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);
  }

  return resp;
}

// ── embed: vector embedding ─────────────────────────────────

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

// ── R7-17: SSRF validation implementation ──────────────────────────

void OpenAIBackend::validate_and_warn_ssrf() const {
  // SECURITY: Validate base URL is not targeting private networks
  auto is_safe_url = [](const std::string &url) -> bool {
    // Must be HTTPS (except localhost for development)
    if (!url.starts_with("https://") && !url.starts_with("http://localhost")) {
      return false;
    }
    // Extract hostname
    auto start = url.find("://") + 3;
    auto end = url.find_first_of(":/", start);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(start, end - start);

    // Reject known private ranges
    if (host == "localhost" || host == "127.0.0.1" || host == "0.0.0.0" ||
        host.starts_with("10.") || host.starts_with("192.168.") ||
        host.starts_with("172.16.") || host.starts_with("172.17.") ||
        host.starts_with("172.18.") || host.starts_with("172.19.") ||
        host.starts_with("172.2") || host.starts_with("172.3") ||
        host == "169.254.169.254" ||  // AWS metadata
        host.ends_with(".internal") || host.ends_with(".local")) {
      LOG_WARN(fmt::format("[LLM] base_url targets private network: {} — SSRF risk", host));
      return false;
    }
    return true;
  };

  if (!is_safe_url(base_url_)) {
    LOG_WARN(fmt::format("[LLM] base_url '{}' may target private network. "
                         "Set AGENTOS_ALLOW_PRIVATE_LLM=1 to override.", base_url_));
    // Don't throw — allow override for development/testing
  }
}

} // namespace agentos::kernel
