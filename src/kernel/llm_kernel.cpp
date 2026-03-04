#include <agentos/kernel/llm_kernel.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace agentos {

// ── Json 工具方法目前由 nlohmann/json 替代 ───────────────────

} // namespace agentos

namespace agentos::kernel {

// ── OpenAIBackend 实现 ────────────────────────────────────────

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
          // args_json is passed as JSON-encoded string in OpenAI's API
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
    root_obj["tools"] = Json::parse(*req.tools_json);
    root_obj["tool_choice"] = "auto";
  }

  return root_obj.dump();
}

Result<LLMResponse>
OpenAIBackend::parse_response(const std::string &json_str) const {
  LLMResponse resp;
  resp.finish_reason = "stop";
  try {
    Json j = Json::parse(json_str);

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
        resp.prompt_tokens = usage["prompt_tokens"].get<int>();
      if (usage.contains("completion_tokens") &&
          usage["completion_tokens"].is_number_integer())
        resp.completion_tokens = usage["completion_tokens"].get<int>();
    }
  } catch (const Json::exception &e) {
    return make_error(ErrorCode::LLMBackendError,
                      fmt::format("JSON Parse Error: {}", e.what()));
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

Result<LLMResponse> OpenAIBackend::stream(const LLMRequest &req,
                                          TokenCallback cb) {
  // 1. 构建请求体，注入 stream 标志
  std::string body = build_request_json(req);
  // 简单粗暴插入 "stream": true
  auto obj_end = body.rfind('}');
  if (obj_end != std::string::npos) {
    body.insert(obj_end, ",\"stream\":true");
  }

  // 将请求体写入临时文件，避免 shell 转义
  std::string tmp_req =
      fmt::format("/tmp/agentos_req_stream_{}.json", getpid());
  FILE *fw = fopen(tmp_req.c_str(), "w");
  if (!fw)
    return make_error(ErrorCode::LLMBackendError,
                      "Cannot write temp request file");
  fwrite(body.data(), 1, body.size(), fw);
  fclose(fw);

  // 2. 构造 curl 命令 (使用 -s 和 -N 避免缓冲，直接读取 stdout)
  std::string url = base_url_ + "/chat/completions";
  std::string cmd = fmt::format("curl -s -X POST '{}' "
                                "-H 'Content-Type: application/json' "
                                "-H 'Authorization: Bearer {}' "
                                "--data-binary @'{}' "
                                "-N --max-time 120",
                                url, api_key_, tmp_req);

  LLMResponse resp;
  resp.finish_reason = "stop";
  std::string current_tool_id;
  std::string current_tool_name;
  std::string current_tool_args;

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    remove(tmp_req.c_str());
    return make_error(ErrorCode::LLMBackendError,
                      "Failed to start curl process");
  }

  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    std::string line(buf);
    // 移除换行符
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();

    if (line.starts_with("data: ")) {
      std::string data = line.substr(6);
      if (data == "[DONE]")
        break; // 结束标志

      try {
        Json j = Json::parse(data);
        if (j.contains("choices") && j["choices"].is_array() &&
            !j["choices"].empty()) {
          auto &choice = j["choices"][0];
          if (choice.contains("delta")) {
            auto &delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
              std::string text = delta["content"].get<std::string>();
              if (cb)
                cb(text);
              resp.content += text;
            }
            if (delta.contains("tool_calls") &&
                delta["tool_calls"].is_array() &&
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
          if (choice.contains("finish_reason") &&
              choice["finish_reason"].is_string()) {
            std::string fr = choice["finish_reason"].get<std::string>();
            resp.finish_reason = fr;
            if (fr == "tool_calls" && !current_tool_name.empty()) {
              ToolCallRequest tcr;
              tcr.id = current_tool_id.empty() ? "call_0" : current_tool_id;
              tcr.name = current_tool_name;
              tcr.args_json = current_tool_args;
              resp.tool_calls.push_back(std::move(tcr));
            }
          }
        }
      } catch (...) {
      }
    }
  }

  pclose(pipe);
  remove(tmp_req.c_str());

  // 简单估算 token 用量，流式接口通常不在行内返回完整 usage
  resp.prompt_tokens = 0; // 可以遍历请求去估算
  for (auto &m : req.messages)
    resp.prompt_tokens += ILLMBackend::estimate_tokens(m.content);
  resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);

  return resp;
}

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
      resp.total_tokens = j["usage"]["total_tokens"].get<int>();
    }
    if (j.contains("data") && j["data"].is_array()) {
      for (auto &item : j["data"]) {
        if (item.contains("embedding") && item["embedding"].is_array()) {
          std::vector<float> vec;
          for (auto &v : item["embedding"]) {
            if (v.is_number()) {
              vec.push_back(v.get<float>());
            }
          }
          resp.embeddings.push_back(std::move(vec));
        }
      }
    }
  } catch (...) {
    return make_error(ErrorCode::LLMBackendError,
                      "Failed to parse embeddings JSON");
  }

  return resp;
}

} // namespace agentos::kernel
