#include <agentos/kernel/llm_kernel.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace agentos {

// ── Json 工具方法实现 ─────────────────────────────────────────
std::optional<std::string> Json::get_string(std::string_view key) const {
  // 简单查找 "key":"value" 模式
  std::string search = fmt::format("\"{}\":", key);
  auto pos = raw.find(search);
  if (pos == std::string::npos)
    return std::nullopt;
  pos += search.size();
  while (pos < raw.size() && raw[pos] == ' ')
    pos++;
  if (pos >= raw.size() || raw[pos] != '"')
    return std::nullopt;
  pos++; // skip opening "
  std::string result;
  while (pos < raw.size() && raw[pos] != '"') {
    if (raw[pos] == '\\' && pos + 1 < raw.size()) {
      pos++;
      if (raw[pos] == 'n')
        result += '\n';
      else if (raw[pos] == '"')
        result += '"';
      else
        result += raw[pos];
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
  if (pos == std::string::npos)
    return std::nullopt;
  pos += search.size();
  while (pos < raw.size() && raw[pos] == ' ')
    pos++;
  std::string num;
  while (pos < raw.size() && (std::isdigit(raw[pos]) || raw[pos] == '-')) {
    num += raw[pos++];
  }
  if (num.empty())
    return std::nullopt;
  return std::stoi(num);
}

} // namespace agentos

namespace agentos::kernel {

// ── OpenAIBackend 实现 ────────────────────────────────────────

std::string OpenAIBackend::build_request_json(const LLMRequest &req) const {
  std::string messages_json = "[";
  bool first = true;
  for (auto &m : req.messages) {
    if (!first)
      messages_json += ',';
    std::string role_str;
    switch (m.role) {
    case Role::System:
      role_str = "system";
      break;
    case Role::User:
      role_str = "user";
      break;
    case Role::Assistant:
      role_str = "assistant";
      break;
    case Role::Tool:
      role_str = "tool";
      break;
    }
    if (m.role == Role::Tool) {
      messages_json += "{\"role\":\"" + role_str +
                       "\",\"content\":" + Json::quote(m.content) +
                       ",\"tool_call_id\":" + Json::quote(m.tool_call_id) + "}";
    } else if (m.role == Role::Assistant) {
      std::string msg_json =
          "{\"role\":\"" + role_str + "\",\"content\":" +
          (m.content.empty() ? "null" : Json::quote(m.content));
      if (!m.tool_calls.empty()) {
        msg_json += ",\"tool_calls\":[";
        bool first_tc = true;
        for (const auto &tc : m.tool_calls) {
          if (!first_tc)
            msg_json += ",";
          msg_json += "{\"id\":" + Json::quote(tc.id) +
                      ",\"type\":\"function\",\"function\":{\"name\":" +
                      Json::quote(tc.name) +
                      ",\"arguments\":" + Json::quote(tc.args_json) + "}}";
          first_tc = false;
        }
        msg_json += "]";
      }
      msg_json += "}";
      messages_json += msg_json;
    } else {
      messages_json += "{\"role\":\"" + role_str +
                       "\",\"content\":" + Json::quote(m.content) + "}";
    }
    first = false;
  }
  messages_json += ']';

  // 使用请求中指定的模型，若为空则用后端默认模型
  const std::string &model = req.model.empty() ? default_model_ : req.model;

  std::string json = "{\"model\":" + Json::quote(model) +
                     ",\"messages\":" + messages_json +
                     ",\"temperature\":" + std::to_string(req.temperature) +
                     ",\"max_tokens\":" + std::to_string(req.max_tokens) + "}";

  // 若请求携带了工具定义，注入 tools + tool_choice 字段
  if (req.tools_json && !req.tools_json->empty()) {
    // 去掉最后的 } 然后追加工具字段
    json.pop_back();
    json += ",\"tools\":" + *req.tools_json + ",\"tool_choice\":\"auto\"}";
  }

  return json;
}

Result<LLMResponse>
OpenAIBackend::parse_response(const std::string &json_str) const {
  // 提取 content
  Json j{json_str};

  // 查找 choices[0].message.content
  auto content_pos = json_str.find("\"content\":");
  std::string content;
  if (content_pos != std::string::npos) {
    Json content_json{json_str.substr(content_pos)};
    auto val = content_json.get_string("content");
    if (val)
      content = *val;
  }

  // finish_reason
  std::string finish_reason = "stop";
  auto fr_pos = json_str.find("\"finish_reason\":");
  if (fr_pos != std::string::npos) {
    Json fr_json{json_str.substr(fr_pos)};
    auto val = fr_json.get_string("finish_reason");
    if (val)
      finish_reason = *val;
  }

  // token usage
  TokenCount prompt_tokens = 0, completion_tokens = 0;
  auto usage_pos = json_str.find("\"usage\":");
  if (usage_pos != std::string::npos) {
    Json usage_json{json_str.substr(usage_pos)};
    if (auto v = usage_json.get_int("prompt_tokens"))
      prompt_tokens = *v;
    if (auto v = usage_json.get_int("completion_tokens"))
      completion_tokens = *v;
  }

  LLMResponse resp;
  resp.content = content;
  resp.finish_reason = finish_reason;
  resp.prompt_tokens = prompt_tokens;
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
        if (auto v = name_json.get_string("name"))
          tcr.name = *v;
        auto args_pos = json_str.find("\"arguments\":", tc_pos);
        if (args_pos != std::string::npos) {
          Json args_json{json_str.substr(args_pos)};
          if (auto v = args_json.get_string("arguments"))
            tcr.args_json = *v;
        }
        tcr.id = "call_0";
        resp.tool_calls.push_back(std::move(tcr));
      }
    }
  }

  return resp;
}

Result<std::string> OpenAIBackend::http_post(const std::string &endpoint,
                                             const std::string &body) const {
  // 将请求体写入临时文件，完全规避 shell 转义问题
  std::string tmp_req = fmt::format("/tmp/agentos_req_{}.json", getpid());
  std::string tmp_resp = fmt::format("/tmp/agentos_resp_{}.json", getpid());

  // 写请求体
  FILE *fw = fopen(tmp_req.c_str(), "w");
  if (!fw)
    return make_error(ErrorCode::LLMBackendError,
                      "Cannot write temp request file");
  fwrite(body.data(), 1, body.size(), fw);
  fclose(fw);

  // 构造 curl 命令：--data-binary @file 避免任何 shell 转义
  std::string url = base_url_ + endpoint;
  std::string cmd = fmt::format("curl -s -f -X POST '{}' "
                                "-H 'Content-Type: application/json' "
                                "-H 'Authorization: Bearer {}' "
                                "--data-binary @'{}' "
                                "-o '{}' "
                                "--max-time 60 " // 最长等待 60 秒
                                "--retry 2 "     // 网络层自动重试 2 次
                                "--retry-delay 1 2>&1",
                                url, api_key_, tmp_req, tmp_resp);

  // 执行 curl，捕获 stderr（用于错误诊断）
  std::array<char, 1024> err_buf{};
  std::string curl_stderr;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    remove(tmp_req.c_str());
    return make_error(ErrorCode::LLMBackendError,
                      "Failed to start curl process");
  }
  while (fgets(err_buf.data(), err_buf.size(), pipe) != nullptr)
    curl_stderr += err_buf.data();
  int status = pclose(pipe);
  int exit_code = -1;
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  } else {
    exit_code = status;
  }

  // 读取响应文件
  std::string result;
  FILE *fr = fopen(tmp_resp.c_str(), "r");
  if (fr) {
    char buf[4096];
    while (fgets(buf, sizeof(buf), fr) != nullptr)
      result += buf;
    fclose(fr);
  }

  // 清理临时文件
  remove(tmp_req.c_str());
  remove(tmp_resp.c_str());

  if (exit_code != 0) {
    std::string detail = result.empty() ? curl_stderr : result;
    return make_error(
        ErrorCode::LLMBackendError,
        fmt::format("curl failed (code {}): {}", exit_code, detail));
  }
  if (result.find("\"error\"") != std::string::npos) {
    return make_error(ErrorCode::LLMBackendError,
                      fmt::format("OpenAI API error: {}", result));
  }
  return result;
}

Result<LLMResponse> OpenAIBackend::complete(const LLMRequest &req) {
  std::string body = build_request_json(req);
  auto http_result = http_post("/chat/completions", body);
  if (!http_result)
    return make_unexpected(http_result.error());
  return parse_response(*http_result);
}

} // namespace agentos::kernel
