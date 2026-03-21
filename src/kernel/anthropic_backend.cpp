#include <agentos/kernel/anthropic_backend.hpp>

namespace agentos::kernel {

AnthropicBackend::AnthropicBackend(std::string api_key,
                                   std::string model,
                                   std::string base_url,
                                   std::shared_ptr<HttpClient> http)
    : api_key_(std::move(api_key)),
      model_(std::move(model)),
      base_url_(std::move(base_url)),
      http_(http ? std::move(http) : std::make_shared<HttpClient>()) {}

std::vector<std::string> AnthropicBackend::build_headers() const {
    return {
        "content-type: application/json",
        "x-api-key: " + api_key_,
        "anthropic-version: 2024-10-22"
    };
}

Json AnthropicBackend::build_request(const LLMRequest& req) const {
    Json j;
    j["model"] = req.model.empty() ? model_ : req.model;
    j["max_tokens"] = req.max_tokens > 0 ? req.max_tokens : 2048;

    // Extract system messages into separate "system" field
    std::string system_text;
    Json messages = Json::array();
    for (const auto& msg : req.messages) {
        if (msg.role == Role::System) {
            if (!system_text.empty()) system_text += "\n";
            system_text += msg.content;
        } else {
            Json m;
            if (msg.role == Role::User) m["role"] = "user";
            else if (msg.role == Role::Assistant) m["role"] = "assistant";
            else if (msg.role == Role::Tool) {
                // Tool results go as user messages with tool_result content
                m["role"] = "user";
                Json content_arr = Json::array();
                Json tool_result;
                tool_result["type"] = "tool_result";
                tool_result["tool_use_id"] = msg.tool_call_id;
                tool_result["content"] = msg.content;
                content_arr.push_back(tool_result);
                m["content"] = content_arr;
                messages.push_back(m);
                continue;
            }
            m["content"] = msg.content;
            messages.push_back(m);
        }
    }

    if (!system_text.empty()) {
        j["system"] = system_text;
    }
    j["messages"] = messages;

    // Tools
    if (req.tools_json.has_value() && !req.tools_json->empty()) {
        try {
            auto tools = Json::parse(*req.tools_json);
            if (tools.is_array() && !tools.empty()) {
                // Convert OpenAI tool format to Anthropic format
                Json anthropic_tools = Json::array();
                for (const auto& tool : tools) {
                    Json t;
                    if (tool.contains("function")) {
                        t["name"] = tool["function"].value("name", "");
                        t["description"] = tool["function"].value("description", "");
                        if (tool["function"].contains("parameters")) {
                            t["input_schema"] = tool["function"]["parameters"];
                        }
                    }
                    anthropic_tools.push_back(t);
                }
                j["tools"] = anthropic_tools;
            }
        } catch (const Json::parse_error&) {
            // ignore malformed tools JSON
        }
    }

    return j;
}

Result<LLMResponse> AnthropicBackend::parse_response(const std::string& body) const {
    Json j;
    try {
        j = Json::parse(body);
    } catch (const Json::parse_error& e) {
        return make_error(ErrorCode::LLMBackendError,
                         ::agentos::fmt::format("Anthropic: JSON parse error: {}", e.what()));
    }

    // Check for API error
    if (j.contains("error")) {
        auto msg = j["error"].value("message", "unknown error");
        return make_error(ErrorCode::LLMBackendError,
                         ::agentos::fmt::format("Anthropic API error: {}", msg));
    }

    LLMResponse response;

    // Parse content array
    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& block : j["content"]) {
            auto type = block.value("type", "");
            if (type == "text") {
                if (!response.content.empty()) response.content += "\n";
                response.content += block.value("text", "");
            } else if (type == "tool_use") {
                ToolCallRequest tc;
                tc.id = block.value("id", "");
                tc.name = block.value("name", "");
                if (block.contains("input")) {
                    tc.args_json = block["input"].dump();
                } else {
                    tc.args_json = "{}";
                }
                response.tool_calls.push_back(std::move(tc));
            }
        }
    }

    // Map stop_reason
    auto stop_reason = j.value("stop_reason", "");
    if (!response.tool_calls.empty()) {
        response.finish_reason = "tool_calls"; // compatibility with wants_tool_call()
    } else {
        response.finish_reason = stop_reason;
    }

    // Token usage
    if (j.contains("usage")) {
        response.prompt_tokens = j["usage"].value("input_tokens", 0u);
        response.completion_tokens = j["usage"].value("output_tokens", 0u);
    }

    return response;
}

Result<LLMResponse> AnthropicBackend::complete(const LLMRequest& req) {
    auto body = build_request(req);
    auto headers = build_headers();

    auto result = http_->post(base_url_ + "/v1/messages", body.dump(), headers);
    if (!result.has_value()) {
        return make_error(result.error().code,
                         ::agentos::fmt::format("Anthropic HTTP error: {}", result.error().message));
    }

    if (result->status_code != 200) {
        // Try to parse error from body
        auto parsed = parse_response(result->body);
        if (!parsed.has_value()) return parsed;
        return make_error(ErrorCode::LLMBackendError,
                         ::agentos::fmt::format("Anthropic: HTTP {}", result->status_code));
    }

    return parse_response(result->body);
}

Result<LLMResponse> AnthropicBackend::stream(const LLMRequest& req, TokenCallback cb) {
    auto body = build_request(req);
    body["stream"] = true;
    auto headers = build_headers();

    LLMResponse response;
    std::string line_buffer;

    auto on_data = [&](const char* data, size_t len) -> size_t {
        line_buffer.append(data, len);

        // Process complete SSE lines
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            auto line = line_buffer.substr(0, pos);
            line_buffer.erase(0, pos + 1);

            // Remove trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            // SSE format: "data: {...}"
            if (line.starts_with("data: ")) {
                auto json_str = line.substr(6);
                if (json_str == "[DONE]") continue;

                try {
                    auto j = Json::parse(json_str);
                    auto type = j.value("type", "");

                    if (type == "content_block_delta") {
                        if (j.contains("delta")) {
                            auto text = j["delta"].value("text", "");
                            if (!text.empty()) {
                                response.content += text;
                                if (cb) cb(text);
                            }
                        }
                    } else if (type == "message_delta") {
                        if (j.contains("delta")) {
                            response.finish_reason = j["delta"].value("stop_reason", "");
                        }
                        if (j.contains("usage")) {
                            response.completion_tokens = j["usage"].value("output_tokens", 0u);
                        }
                    } else if (type == "message_start") {
                        if (j.contains("message") && j["message"].contains("usage")) {
                            response.prompt_tokens = j["message"]["usage"].value("input_tokens", 0u);
                        }
                    }
                } catch (const Json::parse_error&) {
                    // skip malformed SSE data
                }
            }
        }
        return len;
    };

    auto result = http_->post_stream(base_url_ + "/v1/messages", body.dump(), headers, on_data);
    if (!result.has_value()) {
        return make_error(result.error().code,
                         ::agentos::fmt::format("Anthropic stream error: {}", result.error().message));
    }

    return response;
}

} // namespace agentos::kernel
