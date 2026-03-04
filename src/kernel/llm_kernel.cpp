#include <agentos/kernel/llm_kernel.hpp>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace agentos {

// ── Json 工具方法实现 ─────────────────────────────────────────
std::optional<std::string> Json::get_string(std::string_view key) const {
    // 简单查找 "key":"value" 模式
    std::string search = fmt::format("\"{}\":", key);
    auto pos = raw.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos += search.size();
    while (pos < raw.size() && raw[pos] == ' ') pos++;
    if (pos >= raw.size() || raw[pos] != '"') return std::nullopt;
    pos++; // skip opening "
    std::string result;
    while (pos < raw.size() && raw[pos] != '"') {
        if (raw[pos] == '\\' && pos + 1 < raw.size()) {
            pos++;
            if (raw[pos] == 'n') result += '\n';
            else if (raw[pos] == '"') result += '"';
            else result += raw[pos];
        } else {
            result += raw[pos];
        }
        pos++;
    }
    return result;
}

std::optional<int> Json::get_int(std::string_view key) const {
    std::string search = fmt::format("\"{}\":", key);
    auto pos = raw.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos += search.size();
    while (pos < raw.size() && raw[pos] == ' ') pos++;
    std::string num;
    while (pos < raw.size() && (std::isdigit(raw[pos]) || raw[pos] == '-')) {
        num += raw[pos++];
    }
    if (num.empty()) return std::nullopt;
    return std::stoi(num);
}

} // namespace agentos

namespace agentos::kernel {

// ── OpenAIBackend 实现 ────────────────────────────────────────

std::string OpenAIBackend::build_request_json(const LLMRequest& req) const {
    std::string messages_json = "[";
    bool first = true;
    for (auto& m : req.messages) {
        if (!first) messages_json += ',';
        std::string role_str;
        switch (m.role) {
            case Role::System:    role_str = "system";    break;
            case Role::User:      role_str = "user";      break;
            case Role::Assistant: role_str = "assistant"; break;
            case Role::Tool:      role_str = "tool";      break;
        }
        if (m.role == Role::Tool) {
            messages_json += fmt::format(
                R"({{"role":"{}","content":{},"tool_call_id":{}}})",
                role_str,
                Json::quote(m.content),
                Json::quote(m.tool_call_id));
        } else {
            messages_json += fmt::format(
                R"({{"role":"{}","content":{}}})",
                role_str, Json::quote(m.content));
        }
        first = false;
    }
    messages_json += ']';

    return fmt::format(
        R"({{"model":{},"messages":{},"temperature":{},"max_tokens":{}}})",
        Json::quote(req.model),
        messages_json,
        req.temperature,
        req.max_tokens);
}

Result<LLMResponse> OpenAIBackend::parse_response(const std::string& json_str) const {
    // 提取 content
    Json j{json_str};

    // 查找 choices[0].message.content
    auto content_pos = json_str.find("\"content\":");
    std::string content;
    if (content_pos != std::string::npos) {
        Json content_json{json_str.substr(content_pos)};
        auto val = content_json.get_string("content");
        if (val) content = *val;
    }

    // finish_reason
    std::string finish_reason = "stop";
    auto fr_pos = json_str.find("\"finish_reason\":");
    if (fr_pos != std::string::npos) {
        Json fr_json{json_str.substr(fr_pos)};
        auto val = fr_json.get_string("finish_reason");
        if (val) finish_reason = *val;
    }

    // token usage
    TokenCount prompt_tokens = 0, completion_tokens = 0;
    auto usage_pos = json_str.find("\"usage\":");
    if (usage_pos != std::string::npos) {
        Json usage_json{json_str.substr(usage_pos)};
        if (auto v = usage_json.get_int("prompt_tokens")) prompt_tokens = *v;
        if (auto v = usage_json.get_int("completion_tokens")) completion_tokens = *v;
    }

    LLMResponse resp;
    resp.content           = content;
    resp.finish_reason     = finish_reason;
    resp.prompt_tokens     = prompt_tokens;
    resp.completion_tokens = completion_tokens;

    // 工具调用解析（简化版）
    if (finish_reason == "tool_calls") {
        auto tc_pos = json_str.find("\"tool_calls\":");
        if (tc_pos != std::string::npos) {
            // 简单提取第一个工具调用
            auto name_pos = json_str.find("\"name\":", tc_pos);
            if (name_pos != std::string::npos) {
                Json name_json{json_str.substr(name_pos)};
                ToolCallRequest tcr;
                if (auto v = name_json.get_string("name")) tcr.name = *v;
                auto args_pos = json_str.find("\"arguments\":", tc_pos);
                if (args_pos != std::string::npos) {
                    Json args_json{json_str.substr(args_pos)};
                    if (auto v = args_json.get_string("arguments")) tcr.args_json = *v;
                }
                tcr.id = "call_0";
                resp.tool_calls.push_back(std::move(tcr));
            }
        }
    }

    return resp;
}

Result<std::string> OpenAIBackend::http_post(const std::string& endpoint,
                                             const std::string& body) const {
    // 通过 curl 子进程发起 HTTP 请求
    std::string url = base_url_ + endpoint;
    std::string cmd = fmt::format(
        "curl -s -X POST {} "
        "-H 'Content-Type: application/json' "
        "-H 'Authorization: Bearer {}' "
        "-d '{}'",
        url, api_key_,
        // 转义单引号
        [&]{
            std::string escaped;
            for (char c : body) {
                if (c == '\'') escaped += "'\\''";
                else escaped += c;
            }
            return escaped;
        }());

    std::array<char, 4096> buf{};
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return make_error(ErrorCode::LLMBackendError, "Failed to start curl process");
    }
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        result += buf.data();
    }
    int exit_code = pclose(pipe);
    if (exit_code != 0) {
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("curl failed with code {}", exit_code));
    }
    if (result.find("\"error\"") != std::string::npos) {
        return make_error(ErrorCode::LLMBackendError,
                          fmt::format("OpenAI API error: {}", result));
    }
    return result;
}

Result<LLMResponse> OpenAIBackend::complete(const LLMRequest& req) {
    std::string body = build_request_json(req);
    auto http_result = http_post("/chat/completions", body);
    if (!http_result) return make_unexpected(http_result.error());
    return parse_response(*http_result);
}

} // namespace agentos::kernel
