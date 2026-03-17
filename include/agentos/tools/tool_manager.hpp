#pragma once
// ============================================================
// AgentOS :: Module 5 — Tool Manager
// 工具注册、发现、动态加载、沙箱隔离执行
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/memory/memory.hpp>
#include <functional>
#include <future>
#include <stop_token>
#include <queue>
#include <thread>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <queue>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <agentos/utils/thread_pool.hpp>

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
  uint32_t timeout_ms{30000}; // 执行超时（毫秒）, 0 = 无超时

  // validate() defined below (after ParsedArgs declaration)

  // 生成 OpenAI function calling 格式的 JSON
  std::string to_function_json() const {
    Json root = Json::object();
    root["type"] = "function";
    Json func = Json::object();
    func["name"] = id;
    if (!description.empty())
      func["description"] = description;

    Json params_obj = Json::object();
    params_obj["type"] = "object";
    Json props = Json::object();
    Json required_arr = Json::array();

    for (const auto &p : params) {
      Json p_desc = Json::object();
      switch (p.type) {
      case ParamType::String:
        p_desc["type"] = "string";
        break;
      case ParamType::Integer:
        p_desc["type"] = "integer";
        break;
      case ParamType::Float:
        p_desc["type"] = "number";
        break;
      case ParamType::Boolean:
        p_desc["type"] = "boolean";
        break;
      default:
        p_desc["type"] = "object";
        break;
      }
      if (!p.description.empty())
        p_desc["description"] = p.description;
      props[p.name] = p_desc;
      if (p.required)
        required_arr.push_back(p.name);
    }

    params_obj["properties"] = props;
    if (!required_arr.empty())
      params_obj["required"] = required_arr;
    func["parameters"] = params_obj;
    root["function"] = func;

    return root.dump();
  }
};

// ─────────────────────────────────────────────────────────────
// § 5.2  ToolCall / ToolResult
// ─────────────────────────────────────────────────────────────

struct ParsedArgs {
  std::stop_token stop_token;
  std::unordered_map<std::string, std::string> values;

  std::string get(std::string_view key,
                  std::string_view default_val = "") const {
    auto it = values.find(std::string(key));
    return it != values.end() ? it->second : std::string(default_val);
  }
};

// Helper: basic UTF-8 validation (rejects truncated/overlong sequences)
static bool is_valid_utf8(const std::string &s) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(s.data());
    size_t len = s.size();
    for (size_t i = 0; i < len; ) {
        if (bytes[i] <= 0x7F) { ++i; continue; }
        size_t seq_len;
        if      ((bytes[i] & 0xE0) == 0xC0) seq_len = 2;
        else if ((bytes[i] & 0xF0) == 0xE0) seq_len = 3;
        else if ((bytes[i] & 0xF8) == 0xF0) seq_len = 4;
        else return false;  // Invalid leading byte
        if (i + seq_len > len) return false;  // Truncated sequence
        for (size_t j = 1; j < seq_len; ++j) {
            if ((bytes[i + j] & 0xC0) != 0x80) return false;  // Invalid continuation
        }
        i += seq_len;
    }
    return true;
}

/// Validate parsed args against schema (required params)
inline Result<void> validate_tool_args(const ToolSchema &schema,
                                        const ParsedArgs &args) {
  for (const auto &p : schema.params) {
    if (p.required && !args.values.contains(p.name) &&
        !p.default_value.has_value()) {
      return make_error(
          ErrorCode::InvalidArgument,
          fmt::format("Missing required parameter '{}'", p.name));
    }
  }

  // Validate UTF-8 encoding of string arguments
  for (const auto &[key, val] : args.values) {
    if (!is_valid_utf8(val)) {
      return make_error(ErrorCode::InvalidArgument,
          fmt::format("Tool argument '{}' contains invalid UTF-8", key));
    }
  }

  return {};
}

// 使用 nlohmann::json 解析参数
inline ParsedArgs parse_args(std::string_view json_str) {
  ParsedArgs args;
  if (json_str.empty())
    return args;
  try {
    Json j = Json::parse(std::string(json_str));
    if (j.is_object()) {
      for (const auto &[key, value] : j.items()) {
        if (value.is_string()) {
          args.values[key] = value.get<std::string>();
        } else {
          args.values[key] = value.dump(); // 转换非字符串为原生字符串表示
        }
      }
    }
  } catch (const std::exception &e) {
    LOG_WARN(fmt::format("parse_args: invalid JSON input: {}", e.what()));
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
  bool truncated{false};  // NEW: true if output was cut short

  static ToolResult ok(std::string out) { return {true, std::move(out), "", false}; }
  static ToolResult fail(std::string err) {
    return {false, "", std::move(err), false};
  }

  // C++20 defaulted equality comparison
  friend bool operator==(const ToolResult&, const ToolResult&) = default;
};

class ITool {
public:
  virtual ~ITool() = default;
  virtual ToolSchema schema() const = 0;
  virtual ToolResult execute(const ParsedArgs &args, std::stop_token st = {}) = 0;
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
                {.name = "op",
                 .type = ParamType::String,
                 .description = "操作：get / set / delete",
                 .required = true,
                 .default_value = std::nullopt},
                {.name = "key",
                 .type = ParamType::String,
                 .description = "键名",
                 .required = true,
                 .default_value = std::nullopt},
                {.name = "value",
                 .type = ParamType::String,
                 .description = "值（set 时必填）",
                 .required = false,
                 .default_value = std::nullopt},
            },
    };
  }

  ToolResult execute(const ParsedArgs &args, std::stop_token st = {}) override {
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
                {.name = "cmd",
                 .type = ParamType::String,
                 .description = "要执行的命令",
                 .required = true,
                 .default_value = std::nullopt},
            },
        .is_dangerous = true,
        .sandboxed = true,
    };
  }

  ToolResult execute(const ParsedArgs &args, std::stop_token st = {}) override;

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
                {.name = "url",
                 .type = ParamType::String,
                 .description = "目标 URL",
                 .required = true,
                 .default_value = std::nullopt},
            },
    };
  }

  ToolResult execute(const ParsedArgs &args, std::stop_token st = {}) override;
};

// ─────────────────────────────────────────────────────────────
// § 5.5  ToolRegistry — 注册与发现
// ─────────────────────────────────────────────────────────────

class ToolRegistry : private NonCopyable {
public:
  // 注册工具
  void register_tool(std::shared_ptr<ITool> tool) {
    std::lock_guard lk(mu_);
    auto s = tool->schema();
    std::string id = s.id;
    cached_jsons_[id] = s.to_function_json();
    registry_[std::move(id)] = std::move(tool);
    invalidate_cache_locked();
  }

  // 动态注册函数式工具（lambda/function）
  template <typename Fn> void register_fn(ToolSchema schema, Fn &&fn) {
    auto id = schema.id;
    class FnTool : public ITool {
    public:
      FnTool(ToolSchema s, std::function<ToolResult(const ParsedArgs &, std::stop_token)> f)
          : schema_(std::move(s)), fn_(std::move(f)) {}
      ToolSchema schema() const override { return schema_; }
      ToolResult execute(const ParsedArgs &a, std::stop_token /*st*/ = {}) override { return fn_(a); }

    private:
      ToolSchema schema_;
      std::function<ToolResult(const ParsedArgs &, std::stop_token)> fn_;
    };
    register_tool(std::make_shared<FnTool>(
        std::move(schema),
        std::function<ToolResult(const ParsedArgs &, std::stop_token)>(
            [f = std::forward<Fn>(fn)](const ParsedArgs& args, std::stop_token /*st*/) mutable {
                return f(args);
            }
        )));
  }

  std::shared_ptr<ITool> find(const std::string &id) const {
    std::lock_guard lk(mu_);
    auto it = registry_.find(id);
    return it != registry_.end() ? it->second : nullptr;
  }

  std::vector<ToolSchema> list_schemas() const {
    std::lock_guard lk(mu_);
    std::vector<ToolSchema> schemas;
    schemas.reserve(registry_.size());
    for (auto &[id, tool] : registry_)
      schemas.push_back(tool->schema());
    return schemas;
  }

  // 高性能获取所有工具的合并 JSON
  std::string get_all_tools_json() const {
      std::lock_guard lk(mu_);
      if (all_tools_json_cache_.empty() && !registry_.empty()) {
          all_tools_json_cache_ = "[";
          bool first = true;
          for (const auto& [id, json] : cached_jsons_) {
              if (!first) all_tools_json_cache_ += ",";
              all_tools_json_cache_ += json;
              first = false;
          }
          all_tools_json_cache_ += "]";
      }
      return all_tools_json_cache_;
  }

  std::string get_filtered_tools_json(const std::vector<std::string>& filter) const {
      if (filter.empty()) return get_all_tools_json();
      
      std::lock_guard lk(mu_);
      std::string result = "[";
      bool first = true;
      for (const auto& id : filter) {
          auto it = cached_jsons_.find(id);
          if (it != cached_jsons_.end()) {
              if (!first) result += ",";
              result += it->second;
              first = false;
          }
      }
      result += "]";
      return result;
  }

  bool has(const std::string &id) const {
    std::lock_guard lk(mu_);
    return registry_.contains(id);
  }

  void unregister(const std::string &id) {
    std::lock_guard lk(mu_);
    registry_.erase(id);
    cached_jsons_.erase(id);
    invalidate_cache_locked();
  }

private:
  void invalidate_cache_locked() {
      all_tools_json_cache_.clear();
  }

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<ITool>> registry_;
  std::unordered_map<std::string, std::string> cached_jsons_;
  mutable std::string all_tools_json_cache_;
};

// ─────────────────────────────────────────────────────────────
// § 5.6  ToolManager — 统一门面（注册 + 路由 + 执行）
// ─────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────
// § 5.5.5 ToolThreadPool — Bounded thread pool for tool execution
// ─────────────────────────────────────────────────────────────
class ToolThreadPool : private NonCopyable {
public:
  explicit ToolThreadPool(size_t threads = 4) : stop_(false) {
    for (size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
              return;
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          task();
        }
      });
    }
  }

  ~ToolThreadPool() {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      stop_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  template <class F>
  auto enqueue(F &&f) -> std::future<std::invoke_result_t<F>> {
    using return_type = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::forward<F>(f));
    std::future<return_type> res = task->get_future();
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stop_)
        throw std::runtime_error("enqueue on stopped ThreadPool");
      tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return res;
  }

private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable condition_;
  bool stop_;
};

class ToolManager : private NonCopyable {
public:
  ToolManager(memory::MemorySystem *memory = nullptr) : memory_(memory), thread_pool_(4) {
    // 注册内置工具
    registry_.register_tool(std::make_shared<KVStoreTool>());
    registry_.register_tool(std::make_shared<ShellTool>());
    registry_.register_tool(std::make_shared<HttpFetchTool>());

    if (memory_) {
      // 注册知识图谱/本体提取工具
      registry_.register_fn(
          ToolSchema{
              .id = "extract_knowledge_graph",
              .description = "提取实体和关系以构建本体知识图谱。用于将客"
                             "观事实或逻辑关系保存到长期的知识图谱中。",
              .params = {{.name = "subject",
                          .type = ParamType::String,
                          .description = "主语实体名称，如 'Apple' 或 'Alice'",
                          .required = true,
                          .default_value = std::nullopt},
                         {.name = "predicate",
                          .type = ParamType::String,
                          .description = "谓语关系，如 'founded_by' 或 'likes'",
                          .required = true,
                          .default_value = std::nullopt},
                         {.name = "object",
                          .type = ParamType::String,
                          .description =
                              "宾语实体名称，如 'Steve Jobs' 或 'Bob'",
                          .required = true,
                          .default_value = std::nullopt}}},
          [this](const ParsedArgs &args) -> ToolResult {
            auto subj = args.get("subject");
            auto pred = args.get("predicate");
            auto obj = args.get("object");
            if (subj.empty() || pred.empty() || obj.empty()) {
              return ToolResult::fail(
                  "subject, predicate, object must not be empty");
            }
            auto res = memory_->add_triplet(subj, pred, obj);
            if (res)
              return ToolResult::ok(fmt::format(
                  "Successfully recorded: {} -[{}]-> {}", subj, pred, obj));
            else
              return ToolResult::fail("Failed to add graph relation");
          });

      // 注册知识图谱查询工具
      registry_.register_fn(
          ToolSchema{
              .id = "query_ontology",
              .description =
                  "查询知识图谱（Ontology）。当面临需要复杂多跳关系的推理问题时"
                  "使用，如 '苹果的CEO在哪里上学？'。返回与之相连的子图信息。",
              .params = {{.name = "entity",
                          .type = ParamType::String,
                          .description = "要查询的中心实体名称",
                          .required = true,
                          .default_value = std::nullopt}}},
          [this](const ParsedArgs &args) -> ToolResult {
            auto entity = args.get("entity");
            if (entity.empty())
              return ToolResult::fail("entity parameter is required");

            auto res = memory_->query_graph(entity, 2); // default 2-hop
            if (!res)
              return ToolResult::fail(
                  "Failed to query ontology or entity not found");

            std::string out =
                fmt::format("Ontology sub-graph for '{}':\n", entity);
            for (const auto &r : res->edges) {
              out += fmt::format("- {} -[{}]-> {}\n", r.source_id, r.relation,
                                 r.target_id);
            }
            if (res->edges.empty()) {
              out += "No known relations found.";
            }
            return ToolResult::ok(out);
          });
    }
  }

  ToolRegistry &registry() { return registry_; }

  // 路由并执行来自 LLM 的工具调用请求
  ToolResult
  dispatch(const kernel::ToolCallRequest &call,
           const std::unordered_set<std::string> &allowed_tools = {}) {
    // 检查工具是否被允许
    if (!allowed_tools.empty() && !allowed_tools.contains(call.name)) {
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

    // Validate required arguments before spawning async task (early return avoids
    // creating unnecessary threads for malformed requests)
    auto schema = tool->schema();
    if (auto v = validate_tool_args(schema, args); !v)
      return ToolResult::fail(v.error().message);

    // Execute with timeout protection
    try {
      if (schema.timeout_ms > 0) {
        // Use thread pool and std::stop_token for cooperative cancellation
        std::stop_source stop_source;
        auto st = stop_source.get_token();

        // Capture by value to avoid dangling references in async context
        auto future = thread_pool_.enqueue(
            [tool, args, st]() { return tool->execute(args, st); });

        auto status = future.wait_for(
            std::chrono::milliseconds(schema.timeout_ms));

        if (status == std::future_status::timeout) {
          // Cancel the tool execution cooperatively
          stop_source.request_stop();
          return ToolResult::fail(
              fmt::format("Tool '{}' timed out after {}ms",
                          call.name, schema.timeout_ms));
        }
        try {
          return future.get();
        } catch (const std::exception &e) {
          return ToolResult{false, fmt::format("Tool threw exception: {}", e.what()), {}};
        } catch (...) {
          return ToolResult{false, "Tool threw unknown exception", {}};
        }
      }
      return tool->execute(args, {});
    } catch (const std::exception &e) {
      return ToolResult::fail(
          fmt::format("Tool execution exception: {}", e.what()));
    }
  }

  // 生成工具列表供 LLM 使用
  std::string tools_json(const std::vector<std::string> &filter = {}) const {
    return registry_.get_filtered_tools_json(filter);
  }

private:
  ToolRegistry registry_;
  ToolThreadPool thread_pool_{4};
  memory::MemorySystem *memory_{nullptr};
  ToolThreadPool thread_pool_;
};

} // namespace agentos::tools
