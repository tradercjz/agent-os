#pragma once
// ============================================================
// AgentOS :: Module 5 — Tool Manager
// 工具注册、发现、动态加载、沙箱隔离执行
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos::tools {

// ─────────────────────────────────────────────────────────────
// § 5.1  ToolSchema — 工具元数据（JSON Schema 风格）
// ─────────────────────────────────────────────────────────────

enum class ParamType { String, Integer, Float, Boolean, Object, Array };

struct ParamDef {
  std::string name;
  ParamType type;
  std::string description;
  bool required{true};
  std::optional<std::string> default_value;
};

struct ToolSchema {
  std::string id; // 唯一标识（如 "web_search"）
  std::string description;
  std::vector<ParamDef> params;
  bool is_dangerous{false}; // 需要 RBAC 检查
  bool sandboxed{false};    // 在子进程中运行

  // 生成 OpenAI function calling 格式的 JSON
  std::string to_function_json() const {
    std::string params_json = "{\"type\":\"object\",\"properties\":{";
    std::string required_json = "[";
    bool first = true, first_req = true;
    for (auto &p : params) {
      if (!first)
        params_json += ',';
      std::string type_str;
      switch (p.type) {
      case ParamType::String:
        type_str = "string";
        break;
      case ParamType::Integer:
        type_str = "integer";
        break;
      case ParamType::Float:
        type_str = "number";
        break;
      case ParamType::Boolean:
        type_str = "boolean";
        break;
      default:
        type_str = "object";
        break;
      }
      params_json += "\"" + p.name + "\":{\"type\":\"" + type_str +
                     "\",\"description\":" + Json::quote(p.description) + "}";
      if (p.required) {
        if (!first_req)
          required_json += ',';
        required_json += '"' + p.name + '"';
        first_req = false;
      }
      first = false;
    }
    params_json += "},\"required\":" + required_json + "]}";

    return "{\"type\":\"function\",\"function\":{\"name\":" + Json::quote(id) +
           ",\"description\":" + Json::quote(description) +
           ",\"parameters\":" + params_json + "}}";
  }
};

// ─────────────────────────────────────────────────────────────
// § 5.2  ToolCall / ToolResult
// ─────────────────────────────────────────────────────────────

struct ParsedArgs {
  std::unordered_map<std::string, std::string> values;

  std::string get(std::string_view key,
                  std::string_view default_val = "") const {
    auto it = values.find(std::string(key));
    return it != values.end() ? it->second : std::string(default_val);
  }
};

// 从 JSON 字符串粗略解析参数（简化版）
inline ParsedArgs parse_args(const std::string &json) {
  ParsedArgs args;
  // 解析 "key": "value" 或 "key": number 模式
  size_t pos = 0;
  while (pos < json.size()) {
    // 找下一个 "
    auto k_start = json.find('"', pos);
    if (k_start == std::string::npos)
      break;
    auto k_end = json.find('"', k_start + 1);
    if (k_end == std::string::npos)
      break;
    std::string key = json.substr(k_start + 1, k_end - k_start - 1);
    pos = k_end + 1;
    // 跳过 : 和空格
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' '))
      pos++;
    if (pos >= json.size())
      break;
    std::string value;
    if (json[pos] == '"') {
      // 字符串值
      pos++;
      while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\') {
          pos++;
        }
        value += json[pos++];
      }
      pos++; // 跳过结束 "
    } else {
      // 数字或 bool
      while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
             json[pos] != '\n') {
        value += json[pos++];
      }
      // trim
      while (!value.empty() && std::isspace(value.back()))
        value.pop_back();
    }
    if (!key.empty())
      args.values[key] = value;
  }
  return args;
}

// ─────────────────────────────────────────────────────────────
// § 5.3  ITool — 工具接口
// ─────────────────────────────────────────────────────────────

struct ToolResult {
  bool success;
  std::string output;
  std::string error;

  static ToolResult ok(std::string out) { return {true, std::move(out), ""}; }
  static ToolResult fail(std::string err) {
    return {false, "", std::move(err)};
  }
};

class ITool {
public:
  virtual ~ITool() = default;
  virtual ToolSchema schema() const = 0;
  virtual ToolResult execute(const ParsedArgs &args) = 0;
};

// ─────────────────────────────────────────────────────────────
// § 5.4  内置工具实现
// ─────────────────────────────────────────────────────────────

// 内存键值存储工具
class KVStoreTool : public ITool {
public:
  ToolSchema schema() const override {
    return {
        .id = "kv_store",
        .description = "读写键值存储，用于 Agent 间共享数据",
        .params =
            {
                {"op", ParamType::String, "操作：get / set / delete"},
                {"key", ParamType::String, "键名"},
                {"value", ParamType::String, "值（set 时必填）", false},
            },
    };
  }

  ToolResult execute(const ParsedArgs &args) override {
    auto op = args.get("op");
    auto key = args.get("key");
    if (op == "set") {
      std::lock_guard lk(mu_);
      store_[key] = args.get("value");
      return ToolResult::ok("OK");
    } else if (op == "get") {
      std::lock_guard lk(mu_);
      auto it = store_.find(key);
      if (it == store_.end())
        return ToolResult::fail("Key not found");
      return ToolResult::ok(it->second);
    } else if (op == "delete") {
      std::lock_guard lk(mu_);
      store_.erase(key);
      return ToolResult::ok("OK");
    }
    return ToolResult::fail("Unknown op: " + op);
  }

private:
  std::mutex mu_;
  std::unordered_map<std::string, std::string> store_;
};

// Shell 命令执行工具（沙箱化）
class ShellTool : public ITool {
public:
  explicit ShellTool(std::unordered_set<std::string> allowed_cmds =
                         {"echo", "date", "pwd", "ls", "cat", "wc", "sort",
                          "uniq"})
      : allowed_cmds_(std::move(allowed_cmds)) {}

  ToolSchema schema() const override {
    return {
        .id = "shell_exec",
        .description = "在受限沙箱中执行 Shell 命令（仅允许白名单命令）",
        .params =
            {
                {"cmd", ParamType::String, "要执行的命令"},
            },
        .is_dangerous = true,
        .sandboxed = true,
    };
  }

  ToolResult execute(const ParsedArgs &args) override {
    auto cmd = args.get("cmd");
    // 提取第一个词（命令名）
    std::string cmd_name = cmd.substr(0, cmd.find(' '));
    if (!allowed_cmds_.count(cmd_name)) {
      return ToolResult::fail(
          fmt::format("Command '{}' not in allowlist", cmd_name));
    }
    // 限制输出长度
    std::string safe_cmd = cmd + " 2>&1 | head -100";
    std::array<char, 2048> buf{};
    std::string output;
    FILE *pipe = popen(safe_cmd.c_str(), "r");
    if (!pipe)
      return ToolResult::fail("Failed to open pipe");
    while (fgets(buf.data(), buf.size(), pipe) != nullptr)
      output += buf.data();
    pclose(pipe);
    return ToolResult::ok(output);
  }

private:
  std::unordered_set<std::string> allowed_cmds_;
};

// HTTP Fetch 工具
class HttpFetchTool : public ITool {
public:
  ToolSchema schema() const override {
    return {
        .id = "http_fetch",
        .description = "获取指定 URL 的内容（GET 请求）",
        .params =
            {
                {"url", ParamType::String, "目标 URL"},
            },
    };
  }

  ToolResult execute(const ParsedArgs &args) override {
    auto url = args.get("url");
    if (url.empty())
      return ToolResult::fail("URL is required");
    // 安全检查：只允许 http/https
    if (url.substr(0, 4) != "http")
      return ToolResult::fail("Only http/https URLs are allowed");

    std::string cmd =
        fmt::format("curl -s --max-time 10 --max-filesize 102400 {}", url);
    std::array<char, 4096> buf{};
    std::string output;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
      return ToolResult::fail("curl not available");
    while (fgets(buf.data(), buf.size(), pipe) != nullptr)
      output += buf.data();
    pclose(pipe);
    // 截断过长输出
    if (output.size() > 4000)
      output = output.substr(0, 4000) + "...[truncated]";
    return ToolResult::ok(output);
  }
};

// ─────────────────────────────────────────────────────────────
// § 5.5  ToolRegistry — 注册与发现
// ─────────────────────────────────────────────────────────────

class ToolRegistry : private NonCopyable {
public:
  // 注册工具
  void register_tool(std::shared_ptr<ITool> tool) {
    std::lock_guard lk(mu_);
    auto schema = tool->schema();
    registry_[schema.id] = std::move(tool);
  }

  // 动态注册函数式工具（lambda/function）
  template <typename Fn> void register_fn(ToolSchema schema, Fn &&fn) {
    class FnTool : public ITool {
    public:
      FnTool(ToolSchema s, std::function<ToolResult(const ParsedArgs &)> f)
          : schema_(std::move(s)), fn_(std::move(f)) {}
      ToolSchema schema() const override { return schema_; }
      ToolResult execute(const ParsedArgs &a) override { return fn_(a); }

    private:
      ToolSchema schema_;
      std::function<ToolResult(const ParsedArgs &)> fn_;
    };
    register_tool(std::make_shared<FnTool>(
        std::move(schema),
        std::function<ToolResult(const ParsedArgs &)>(std::forward<Fn>(fn))));
  }

  std::shared_ptr<ITool> find(const std::string &id) const {
    std::lock_guard lk(mu_);
    auto it = registry_.find(id);
    return it != registry_.end() ? it->second : nullptr;
  }

  std::vector<ToolSchema> list_schemas() const {
    std::lock_guard lk(mu_);
    std::vector<ToolSchema> schemas;
    for (auto &[id, tool] : registry_)
      schemas.push_back(tool->schema());
    return schemas;
  }

  bool has(const std::string &id) const {
    std::lock_guard lk(mu_);
    return registry_.count(id) > 0;
  }

  void unregister(const std::string &id) {
    std::lock_guard lk(mu_);
    registry_.erase(id);
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<ITool>> registry_;
};

// ─────────────────────────────────────────────────────────────
// § 5.6  ToolManager — 统一门面（注册 + 路由 + 执行）
// ─────────────────────────────────────────────────────────────

class ToolManager : private NonCopyable {
public:
  ToolManager() {
    // 注册内置工具
    registry_.register_tool(std::make_shared<KVStoreTool>());
    registry_.register_tool(std::make_shared<ShellTool>());
    registry_.register_tool(std::make_shared<HttpFetchTool>());
  }

  ToolRegistry &registry() { return registry_; }

  // 路由并执行来自 LLM 的工具调用请求
  ToolResult
  dispatch(const kernel::ToolCallRequest &call,
           const std::unordered_set<std::string> &allowed_tools = {}) {
    // 检查工具是否被允许
    if (!allowed_tools.empty() && !allowed_tools.count(call.name)) {
      return ToolResult::fail(
          fmt::format("Tool '{}' not in allowed set", call.name));
    }

    auto tool = registry_.find(call.name);
    if (!tool) {
      return ToolResult::fail(
          fmt::format("Tool '{}' not found in registry", call.name));
    }

    // 解析参数并执行
    auto args = parse_args(call.args_json);
    try {
      return tool->execute(args);
    } catch (const std::exception &e) {
      return ToolResult::fail(
          fmt::format("Tool execution exception: {}", e.what()));
    }
  }

  // 生成工具列表供 LLM 使用
  std::string tools_json(const std::vector<std::string> &filter = {}) const {
    auto schemas = registry_.list_schemas();
    std::string out = "[";
    bool first = true;
    for (auto &s : schemas) {
      if (!filter.empty() &&
          std::find(filter.begin(), filter.end(), s.id) == filter.end())
        continue;
      if (!first)
        out += ',';
      out += s.to_function_json();
      first = false;
    }
    out += ']';
    return out;
  }

private:
  ToolRegistry registry_;
};

} // namespace agentos::tools
