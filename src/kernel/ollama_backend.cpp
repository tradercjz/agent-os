#include <agentos/kernel/ollama_backend.hpp>
#include <agentos/core/logger.hpp>
#include <algorithm>

namespace agentos::kernel {

// ============================================================
// Construction
// ============================================================

OllamaBackend::OllamaBackend(std::string model,
                               std::string base_url,
                               std::shared_ptr<HttpClient> http)
    : model_(std::move(model)),
      base_url_(std::move(base_url)),
      http_(http ? std::move(http) : std::make_shared<HttpClient>()) {}

// ============================================================
// JSON request building
// ============================================================

Json OllamaBackend::build_chat_request(const LLMRequest& req, bool streaming) const {
    Json root = Json::object();
    root["model"] = req.model.empty() ? model_ : req.model;
    root["stream"] = streaming;

    // Messages array
    Json messages = Json::array();
    for (const auto& m : req.messages) {
        Json msg = Json::object();
        switch (m.role) {
        case Role::System:    msg["role"] = "system";    break;
        case Role::User:      msg["role"] = "user";      break;
        case Role::Assistant: msg["role"] = "assistant";  break;
        case Role::Tool:      msg["role"] = "tool";      break;
        }
        msg["content"] = m.content;

        // Tool role needs tool_call_id
        if (m.role == Role::Tool && !m.tool_call_id.empty()) {
            // Ollama doesn't use tool_call_id in the same way, but include for compat
        }

        // Assistant messages with tool_calls
        if (m.role == Role::Assistant && !m.tool_calls.empty()) {
            Json tool_calls = Json::array();
            for (const auto& tc : m.tool_calls) {
                Json call = Json::object();
                Json func = Json::object();
                func["name"] = tc.name;
                // Ollama expects arguments as object, not string
                try {
                    func["arguments"] = Json::parse(tc.args_json);
                } catch (const std::exception&) {
                    func["arguments"] = Json::object();
                }
                call["function"] = func;
                tool_calls.push_back(call);
            }
            msg["tool_calls"] = tool_calls;
        }

        messages.push_back(msg);
    }
    root["messages"] = messages;

    // Options
    Json options = Json::object();
    options["temperature"] = req.temperature;
    if (req.max_tokens > 0) {
        options["num_predict"] = static_cast<int>(req.max_tokens);
    }
    root["options"] = options;

    // Tools (if provided)
    if (req.tools_json && !req.tools_json->empty()) {
        try {
            root["tools"] = Json::parse(*req.tools_json);
        } catch (const std::exception& e) {
            LOG_WARN(fmt::format("[Ollama] Failed to parse tools_json: {}", e.what()));
        }
    }

    return root;
}

// ============================================================
// Response parsing
// ============================================================

Result<LLMResponse> OllamaBackend::parse_chat_response(const std::string& body) const {
    LLMResponse resp;
    resp.finish_reason = "stop";

    try {
        Json j = Json::parse(body);

        // Check for error
        if (j.contains("error") && j["error"].is_string()) {
            return make_error(ErrorCode::LLMBackendError,
                              fmt::format("Ollama error: {}", j["error"].get<std::string>()));
        }

        // Message content
        if (j.contains("message") && j["message"].is_object()) {
            auto& msg = j["message"];
            if (msg.contains("content") && msg["content"].is_string()) {
                resp.content = msg["content"].get<std::string>();
            }

            // Tool calls
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (auto& tc : msg["tool_calls"]) {
                    if (tc.contains("function") && tc["function"].is_object()) {
                        ToolCallRequest tcr;
                        auto& func = tc["function"];
                        if (func.contains("name") && func["name"].is_string())
                            tcr.name = func["name"].get<std::string>();
                        if (func.contains("arguments")) {
                            tcr.args_json = func["arguments"].dump();
                        }
                        // Ollama doesn't assign tool call IDs, generate one
                        static std::atomic<uint64_t> tc_counter{0};
                        tcr.id = fmt::format("ollama_call_{}",
                            tc_counter.fetch_add(1, std::memory_order_relaxed));
                        resp.tool_calls.push_back(std::move(tcr));
                    }
                }
                if (!resp.tool_calls.empty()) {
                    resp.finish_reason = "tool_calls";
                }
            }
        }

        // Token counts
        if (j.contains("eval_count") && j["eval_count"].is_number_integer()) {
            resp.completion_tokens = static_cast<uint32_t>(
                std::max(0, j["eval_count"].get<int>()));
        }
        if (j.contains("prompt_eval_count") && j["prompt_eval_count"].is_number_integer()) {
            resp.prompt_tokens = static_cast<uint32_t>(
                std::max(0, j["prompt_eval_count"].get<int>()));
        }
    } catch (const Json::exception& e) {
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("Ollama JSON parse error: {}", e.what()));
    }

    return resp;
}

// ============================================================
// complete — non-streaming chat
// ============================================================

Result<LLMResponse> OllamaBackend::complete(const LLMRequest& req) {
    Json request_json = build_chat_request(req, /*streaming=*/false);
    std::string body = request_json.dump();
    std::string url = base_url_ + "/api/chat";

    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: application/json");

    auto result = http_->post(url, body, headers, 120);
    if (!result) {
        return make_error(result.error().code,
                          fmt::format("Ollama request failed: {}", result.error().message));
    }

    if (result->status_code >= 400) {
        std::string error_detail = result->body.substr(0, 500);
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("Ollama API error (HTTP {}): {}",
                                      result->status_code, error_detail));
    }

    return parse_chat_response(result->body);
}

// ============================================================
// stream — NDJSON streaming chat
// ============================================================

Result<LLMResponse> OllamaBackend::stream(const LLMRequest& req, TokenCallback cb) {
    Json request_json = build_chat_request(req, /*streaming=*/true);
    std::string body = request_json.dump();
    std::string url = base_url_ + "/api/chat";

    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: application/json");

    LLMResponse resp;
    resp.finish_reason = "stop";
    std::string line_buffer;
    bool done = false;

    auto stream_result = http_->post_stream(
        url, body, headers,
        [&](const char* data, size_t len) -> size_t {
            line_buffer.append(data, len);

            // Ollama streams NDJSON: one complete JSON object per line
            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                if (line.empty()) continue;
                // Strip \r
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                try {
                    Json j = Json::parse(line);

                    // Check for error
                    if (j.contains("error") && j["error"].is_string()) {
                        LOG_WARN(fmt::format("[Ollama] Stream error: {}",
                                             j["error"].get<std::string>()));
                        done = true;
                        return len;
                    }

                    // Extract content delta
                    if (j.contains("message") && j["message"].is_object()) {
                        auto& msg = j["message"];
                        if (msg.contains("content") && msg["content"].is_string()) {
                            std::string text = msg["content"].get<std::string>();
                            if (!text.empty()) {
                                if (cb) cb(text);
                                resp.content += text;
                            }
                        }

                        // Tool calls in streaming (if present)
                        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                            for (auto& tc : msg["tool_calls"]) {
                                if (tc.contains("function") && tc["function"].is_object()) {
                                    ToolCallRequest tcr;
                                    auto& func = tc["function"];
                                    if (func.contains("name") && func["name"].is_string())
                                        tcr.name = func["name"].get<std::string>();
                                    if (func.contains("arguments"))
                                        tcr.args_json = func["arguments"].dump();
                                    static std::atomic<uint64_t> stc{0};
                                    tcr.id = fmt::format("ollama_call_{}",
                                        stc.fetch_add(1, std::memory_order_relaxed));
                                    resp.tool_calls.push_back(std::move(tcr));
                                }
                            }
                            resp.finish_reason = "tool_calls";
                        }
                    }

                    // Check if done
                    if (j.contains("done") && j["done"].is_boolean() && j["done"].get<bool>()) {
                        done = true;
                        // Final message has token counts
                        if (j.contains("eval_count") && j["eval_count"].is_number_integer()) {
                            resp.completion_tokens = static_cast<uint32_t>(
                                std::max(0, j["eval_count"].get<int>()));
                        }
                        if (j.contains("prompt_eval_count") && j["prompt_eval_count"].is_number_integer()) {
                            resp.prompt_tokens = static_cast<uint32_t>(
                                std::max(0, j["prompt_eval_count"].get<int>()));
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARN(fmt::format("[Ollama] NDJSON parse error: {}", e.what()));
                }
            }

            return len;
        },
        120);

    if (!stream_result && !done) {
        return make_error(stream_result.error().code,
                          fmt::format("Ollama stream failed: {}", stream_result.error().message));
    }

    // Fallback token estimation
    if (resp.prompt_tokens == 0) {
        for (const auto& m : req.messages)
            resp.prompt_tokens += ILLMBackend::estimate_tokens(m.content);
    }
    if (resp.completion_tokens == 0) {
        resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);
    }

    return resp;
}

// ============================================================
// embed — vector embeddings
// ============================================================

Result<EmbeddingResponse> OllamaBackend::embed(const EmbeddingRequest& req) {
    Json root = Json::object();
    root["model"] = req.model.empty() ? model_ : req.model;

    // Ollama /api/embed expects "input" as array of strings
    Json input_arr = Json::array();
    for (const auto& text : req.inputs) {
        input_arr.push_back(text);
    }
    root["input"] = input_arr;

    std::string body = root.dump();
    std::string url = base_url_ + "/api/embed";

    std::vector<std::string> headers;
    headers.emplace_back("Content-Type: application/json");

    auto result = http_->post(url, body, headers, 120);
    if (!result) {
        return make_error(result.error().code,
                          fmt::format("Ollama embed request failed: {}", result.error().message));
    }

    if (result->status_code >= 400) {
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("Ollama embed error (HTTP {}): {}",
                                      result->status_code,
                                      result->body.substr(0, 500)));
    }

    EmbeddingResponse resp;
    try {
        Json j = Json::parse(result->body);

        if (j.contains("error") && j["error"].is_string()) {
            return make_error(ErrorCode::LLMBackendError,
                              fmt::format("Ollama embed error: {}", j["error"].get<std::string>()));
        }

        if (j.contains("embeddings") && j["embeddings"].is_array()) {
            for (auto& emb : j["embeddings"]) {
                if (emb.is_array()) {
                    std::vector<float> vec;
                    vec.reserve(emb.size());
                    for (auto& v : emb) {
                        if (v.is_number())
                            vec.push_back(v.get<float>());
                    }
                    resp.embeddings.push_back(std::move(vec));
                }
            }
        }

        // Ollama doesn't return token counts for embeddings; estimate
        for (const auto& text : req.inputs) {
            resp.total_tokens += ILLMBackend::estimate_tokens(text);
        }
    } catch (const std::exception& e) {
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("Failed to parse Ollama embed response: {}", e.what()));
    }

    return resp;
}

} // namespace agentos::kernel
