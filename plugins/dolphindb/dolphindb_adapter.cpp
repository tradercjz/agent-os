// ============================================================
// AgentOS :: DolphinDB Plugin Adapter — Implementation
//
// 遵循 DolphinDB 插件开发规范：
//   - extern "C" 导出所有符号
//   - system 函数签名: ConstantSP(Heap*, vector<ConstantSP>&)
//   - command 函数签名: void(Heap*, vector<ConstantSP>&)
//   - 用 IllegalArgumentException 报告参数错误
//   - 用 RuntimeException 报告运行时错误
//   - 所有子线程内 try/catch，不能抛到线程外
// ============================================================
#include "dolphindb_adapter.hpp"

#include <agentos/core/logger.hpp>
#include <agentos/memory/graph_memory.hpp>
#include <agentos/knowledge/knowledge_base.hpp>
#include <iostream>
#include <sstream>

using namespace ddb;
using std::vector;

namespace agentos::dolphindb {

// 统一异常转换宏：捕获所有非 DolphinDB 标准异常，转为 RuntimeException
#define DDB_SAFE_BEGIN  try {
#define DDB_SAFE_END(func_name) \
    } catch (const IllegalArgumentException&) { throw; } \
      catch (const RuntimeException&) { throw; } \
      catch (const std::exception& e) { \
          throw RuntimeException(std::string(func_name " error: ") + e.what()); \
      } catch (...) { \
          throw RuntimeException(func_name ": unknown internal error"); \
      }

// ─────────────────────────────────────────────────────────────
// § D.1  PluginRuntime 实现
// ─────────────────────────────────────────────────────────────

bool PluginRuntime::init(const std::string& config_json) {
    std::lock_guard lk(mu_);
    if (os_) {
        return false;  // 幂等
    }

    nlohmann::json cfg;
    try {
        cfg = config_json.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(config_json);
    } catch (const std::exception& e) {
        throw IllegalArgumentException("agentOS::init",
            std::string("Invalid JSON config: ") + e.what());
    }

    std::string api_key = cfg.value("api_key", "");
    std::string base_url = cfg.value("base_url", "https://api.openai.com/v1");
    std::string model = cfg.value("model", "gpt-4o-mini");

    if (api_key.empty()) {
        backend_ = std::make_unique<kernel::MockLLMBackend>("dolphindb-mock");
    } else {
        backend_ = std::make_unique<kernel::OpenAIBackend>(
            std::move(api_key), std::move(base_url), std::move(model));
    }

    AgentOS::Config os_cfg;
    os_cfg.scheduler_threads = cfg.value("scheduler_threads", 4u);
    os_cfg.tpm_limit = cfg.value("tpm_limit", 100000u);

    std::string data_dir = cfg.value("data_dir", "/tmp/agentos_dolphindb");
    os_cfg.snapshot_dir = data_dir + "/snapshots";
    os_cfg.ltm_dir = data_dir + "/ltm";
    os_cfg.enable_security = cfg.value("enable_security", true);

    os_ = std::make_unique<AgentOS>(std::move(backend_), os_cfg);
    return true;
}

void PluginRuntime::close() {
    std::lock_guard lk(mu_);
    if (!os_) return;

    for (auto& [handle, agent] : agents_) {
        os_->destroy_agent(agent->id());
    }
    agents_.clear();

    os_->graceful_shutdown();
    os_.reset();
}

AgentOS& PluginRuntime::os() {
    std::lock_guard lk(mu_);
    if (!os_) {
        throw RuntimeException("AgentOS not initialized. Call agentOS::init() first.");
    }
    return *os_;
}

bool PluginRuntime::is_initialized() const noexcept {
    std::lock_guard lk(mu_);
    return os_ != nullptr;
}

std::shared_ptr<Agent> PluginRuntime::find_agent(long long handle) const {
    std::lock_guard lk(mu_);
    auto it = agents_.find(handle);
    if (it == agents_.end()) return nullptr;
    return it->second;
}

long long PluginRuntime::create_agent(const AgentConfig& cfg) {
    std::lock_guard lk(mu_);
    if (!os_) {
        throw RuntimeException("AgentOS not initialized");
    }
    auto agent = os_->create_agent(cfg);
    long long handle = next_handle_++;
    agents_[handle] = agent;
    return handle;
}

bool PluginRuntime::destroy_agent(long long handle) {
    std::lock_guard lk(mu_);
    auto it = agents_.find(handle);
    if (it == agents_.end()) return false;
    if (os_) {
        os_->destroy_agent(it->second->id());
    }
    agents_.erase(it);
    return true;
}

// ─────────────────────────────────────────────────────────────
// § D.2  类型转换工具实现
// ─────────────────────────────────────────────────────────────

std::string extract_string(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return "";
    }
    return val->getString();
}

int extract_int(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return 0;
    }
    return val->getInt();
}

float extract_float(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return 0.0f;
    }
    return val->getFloat();
}

long long extract_long(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return 0;
    }
    return val->getLong();
}

nlohmann::json parse_json(const std::string& s) {
    if (s.empty()) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(s);
    } catch (const std::exception& e) {
        throw IllegalArgumentException("JSON parse",
            std::string("Invalid JSON: ") + e.what());
    }
    // This line will never be reached due to the throw above
    __builtin_unreachable();
}

TableSP search_results_to_table(
    const std::vector<memory::SearchResult>& results) {
    int n = static_cast<int>(results.size());

    // 创建列
    vector<string> colNames{"content", "score", "source", "created_at"};
    vector<DATA_TYPE> colTypes{DT_STRING, DT_DOUBLE, DT_STRING, DT_LONG};
    TableSP table = Util::createTable(colNames, colTypes, 0, n);

    if (n == 0) return table;

    // 构建各列 vector
    VectorSP colContent = Util::createVector(DT_STRING, n);
    VectorSP colScore = Util::createVector(DT_DOUBLE, n);
    VectorSP colSource = Util::createVector(DT_STRING, n);
    VectorSP colCreatedAt = Util::createVector(DT_LONG, n);

    for (int i = 0; i < n; ++i) {
        auto idx = static_cast<size_t>(i);
        colContent->setString(i, results[idx].entry.content);
        colScore->setDouble(i, static_cast<double>(results[idx].score));
        colSource->setString(i, results[idx].entry.source);
        colCreatedAt->setLong(i, static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                results[idx].entry.created_at.time_since_epoch()).count()));
    }

    // 用 append 写入表
    vector<ConstantSP> cols{colContent, colScore, colSource, colCreatedAt};
    INDEX insertedRows;
    string errMsg;
    table->append(cols, insertedRows, errMsg);

    return table;
}

// ─────────────────────────────────────────────────────────────
// § D.3  DolphinDB 导出函数实现
// ─────────────────────────────────────────────────────────────

extern "C" {

// ─── 特殊入口 ────────────────────────────────────────────────

ConstantSP initialize(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;
    // loadPlugin 后自动调用
    // 这里不做 AgentOS 初始化（需要用户显式调用 init 传配置）
    LOG_INFO("AgentOS DolphinDB plugin loaded.");
    return new Void();
}

ConstantSP version(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;
    return new String("1.0.0");
}

ConstantSP pluginInfo(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;
    DictionarySP info = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);
    info->set(new String("isFree"), new Bool(true));
    info->set(new String("pluginName"), new String("agentOS"));
    info->set(new String("description"),
              new String("LLM Agent Operating System for DolphinDB"));
    info->set(new String("version"), new String("1.0.0"));
    return info;
}

// ─── agentOS::init([configJson]) ─────────────────────────────

ConstantSP agentOSInit(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    std::string config_json;
    if (!args.empty()) {
        if (!args[0]->isScalar() || args[0]->getType() != DT_STRING)
            throw IllegalArgumentException("agentOS::init",
                "Usage: agentOS::init([configJson]). configJson must be a string.");
        config_json = args[0]->getString();
    }

    bool is_new = PluginRuntime::instance().init(config_json);
    if (is_new) {
        LOG_INFO("AgentOS initialized via DolphinDB plugin.");
    }

#ifdef AGENTOS_ENABLE_SSE
    // 启动内嵌 SSE 服务（幂等）
    // 可通过 config_json 中的 "sse_port" 和 "sse_cors" 配置
    int sse_port = 8849;
    std::string sse_cors = "*";
    if (!config_json.empty()) {
        try {
            auto cfg = nlohmann::json::parse(config_json);
            if (cfg.contains("sse_port")) sse_port = cfg["sse_port"].get<int>();
            if (cfg.contains("sse_cors")) sse_cors = cfg["sse_cors"].get<std::string>();
        } catch (...) { /* 忽略解析错误 */ }
    }
    SSEServer::instance().start(sse_port, sse_cors);
    LOG_INFO("AgentOS SSE server started on port " + std::to_string(sse_port));
#endif

    return new Bool(true);
}

// ─── agentOS::close() ────────────────────────────────────────

void agentOSClose(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;
    PluginRuntime::instance().close();
    LOG_INFO("AgentOS closed via DolphinDB plugin.");
}

// ─── agentOS::ask(question, [systemPrompt]) ──────────────────

ConstantSP agentOSAsk(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::ask(question, [systemPrompt])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::ask", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::ask", usage + " question must not be empty.");

    std::string system_prompt;
    if (args.size() >= 2 && args[1]->getType() == DT_STRING) {
        system_prompt = args[1]->getString();
    }

    DDB_SAFE_BEGIN
        auto& os = PluginRuntime::instance().os();

        kernel::LLMRequest req;
        req.messages.push_back(kernel::Message::system(
            system_prompt.empty()
                ? "You are a helpful AI assistant integrated with DolphinDB."
                : system_prompt));
        req.messages.push_back(kernel::Message::user(question));

        // 用 stream_infer 减少首 token 延迟（内部 HTTP 分块读取）
        std::string full_response;
        auto result = os.kernel().stream_infer(req,
            [&full_response](std::string_view token) {
                full_response += std::string(token);
            });

        auto resp = unwrap_or_throw("agentOS::ask", std::move(result));
        std::string answer = full_response.empty() ? resp.content : full_response;

        if (answer.empty()) {
            LOG_WARN("agentOS::ask: LLM returned empty content");
            return new String("[empty response from LLM]");
        }
        return new String(answer);
    DDB_SAFE_END("agentOS::ask")
}

// ─── agentOS::askStream(question, [systemPrompt], [callback]) ─

ConstantSP agentOSAskStream(Heap* heap, vector<ConstantSP>& args) {
    const string usage =
        "Usage: agentOS::askStream(question, [systemPrompt], [callbackFunc])\n"
        "  callbackFunc: a DolphinDB function(token) called for each token chunk.\n"
        "  Example: agentOS::askStream(`What is DolphinDB?`, , print)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::askStream", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::askStream", usage);

    std::string system_prompt;
    if (args.size() >= 2 && !args[1]->isNull() && args[1]->getType() == DT_STRING) {
        system_prompt = args[1]->getString();
    }

    // 第三个参数：回调函数（可选）
    // DolphinDB 传函数时，ConstantSP 的 type 为 DT_FUNCTIONDEF
    FunctionDefSP callback;
    if (args.size() >= 3 && !args[2]->isNull() && args[2]->getType() == DT_FUNCTIONDEF) {
        callback = args[2];
    }

    DDB_SAFE_BEGIN
        auto& os = PluginRuntime::instance().os();

        kernel::LLMRequest req;
        req.messages.push_back(kernel::Message::system(
            system_prompt.empty()
                ? "You are a helpful AI assistant integrated with DolphinDB."
                : system_prompt));
        req.messages.push_back(kernel::Message::user(question));

        std::string full_response;

        // 如果有回调函数，每个 token 调用回调推给客户端
        auto token_handler = [&](std::string_view token) {
            full_response += std::string(token);
            if (!callback.isNull()) {
                try {
                    vector<ConstantSP> cb_args{new String(std::string(token))};
                    callback->call(heap, cb_args);
                } catch (const std::exception& e) {
                    LOG_WARN("agentOS::askStream callback error: " +
                             std::string(e.what()));
                }
            }
        };

        auto result = os.kernel().stream_infer(req, token_handler);

        auto resp = unwrap_or_throw("agentOS::askStream", std::move(result));
        std::string answer = full_response.empty() ? resp.content : full_response;

        if (answer.empty()) {
            LOG_WARN("agentOS::askStream: LLM returned empty content");
            return new String("[empty response from LLM]");
        }
        return new String(answer);
    DDB_SAFE_END("agentOS::askStream")
}

// ─── agentOS::askTable(question, [configJson], [agentHandle]) ─

ConstantSP agentOSAskTable(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage =
        "Usage: agentOS::askTable(question, [configJson], [agentHandle])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::askTable", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::askTable", usage);

  DDB_SAFE_BEGIN
    // 解析可选配置
    std::string system_prompt = "You are a helpful AI assistant integrated with DolphinDB.";
    int max_steps = 10;
    long long agent_handle = 0;

    if (args.size() >= 2 && args[1]->getType() == DT_STRING) {
        std::string cfg_str = args[1]->getString();
        if (!cfg_str.empty()) {
            auto opts = parse_json(cfg_str);
            system_prompt = opts.value("system_prompt", system_prompt);
            max_steps = opts.value("max_steps", max_steps);
        }
    }
    if (args.size() >= 3 && args[2]->getType() != DT_VOID) {
        agent_handle = args[2]->getLong();
    }

    auto& os = PluginRuntime::instance().os();
    auto& runtime = PluginRuntime::instance();

    // 交互记录
    struct Row {
        std::string role, content, tool_name, tool_args, tool_result;
    };
    std::vector<Row> rows;

    std::shared_ptr<Agent> agent;
    bool temp_agent = false;

    if (agent_handle > 0) {
        agent = runtime.find_agent(agent_handle);
        if (!agent)
            throw IllegalArgumentException("agentOS::askTable",
                "Invalid agent handle " + std::to_string(agent_handle));
    } else {
        AgentConfig cfg;
        cfg.name = "ddb_table_temp";
        cfg.role_prompt = system_prompt;
        cfg.context_limit = 8192;
        agent = os.create_agent(cfg);
        temp_agent = true;
    }

    auto& ctx_win = os.ctx().get_window(agent->id(), agent->config().context_limit);
    ctx_win.try_add(kernel::Message::user(question));
    rows.push_back({"user", question, "", "", ""});

    for (int step = 0; step < max_steps; ++step) {
        kernel::LLMRequest req;
        req.agent_id = agent->id();
        req.priority = agent->config().priority;
        for (const auto& m : ctx_win.messages()) {
            req.messages.push_back(m);
        }

        std::string tj = os.tools().tools_json({});
        if (!tj.empty() && tj != "[]") {
            req.tools_json = tj;
        }

        auto resp = unwrap_or_throw("agentOS::askTable", os.kernel().infer(req));

        if (!resp.wants_tool_call()) {
            rows.push_back({"assistant", resp.content, "", "", ""});
            ctx_win.try_add(kernel::Message::assistant(resp.content));
            break;
        }

        rows.push_back({"assistant", resp.content, "", "", ""});
        auto msg = kernel::Message::assistant(resp.content);
        msg.tool_calls = resp.tool_calls;
        ctx_win.try_add(std::move(msg));

        for (const auto& tc : resp.tool_calls) {
            auto tool_result = os.tools().dispatch(tc);
            std::string obs;
            obs = tool_result.success ? tool_result.output : tool_result.error;

            rows.push_back({"tool", obs, tc.name, tc.args_json, obs});

            kernel::Message obs_msg;
            obs_msg.role = kernel::Role::Tool;
            obs_msg.content = obs;
            obs_msg.tool_call_id = tc.id;
            obs_msg.name = tc.name;
            ctx_win.try_add(obs_msg);
        }
    }

    if (temp_agent) {
        os.destroy_agent(agent->id());
    }

    // 构建结果表
    int n = static_cast<int>(rows.size());
    vector<string> colNames{"role", "content", "tool_name", "tool_args", "tool_result"};
    vector<DATA_TYPE> colTypes{DT_STRING, DT_STRING, DT_STRING, DT_STRING, DT_STRING};
    TableSP table = Util::createTable(colNames, colTypes, 0, n);

    VectorSP cRole = Util::createVector(DT_STRING, n);
    VectorSP cContent = Util::createVector(DT_STRING, n);
    VectorSP cToolName = Util::createVector(DT_STRING, n);
    VectorSP cToolArgs = Util::createVector(DT_STRING, n);
    VectorSP cToolResult = Util::createVector(DT_STRING, n);

    for (int i = 0; i < n; ++i) {
        auto idx = static_cast<size_t>(i);
        cRole->setString(i, rows[idx].role);
        cContent->setString(i, rows[idx].content);
        cToolName->setString(i, rows[idx].tool_name);
        cToolArgs->setString(i, rows[idx].tool_args);
        cToolResult->setString(i, rows[idx].tool_result);
    }

    vector<ConstantSP> cols{cRole, cContent, cToolName, cToolArgs, cToolResult};
    INDEX insertedRows;
    string errMsg;
    table->append(cols, insertedRows, errMsg);

    return table;
    DDB_SAFE_END("agentOS::askTable")
}

// ─── agentOS::remember(content, [importance], [source]) ──────

ConstantSP agentOSRemember(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::remember(content, [importance], [source])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::remember", usage);

    std::string content = args[0]->getString();
    if (content.empty())
        throw IllegalArgumentException("agentOS::remember", usage + " content must not be empty.");

    float importance = 0.5f;
    if (args.size() >= 2 && args[1]->getType() != DT_VOID) {
        importance = args[1]->getFloat();
        if (importance < 0.0f) importance = 0.0f;
        if (importance > 1.0f) importance = 1.0f;
    }

    std::string source = "dolphindb";
    if (args.size() >= 3 && args[2]->getType() == DT_STRING) {
        source = args[2]->getString();
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    // 生成 embedding
    kernel::EmbeddingRequest emb_req;
    emb_req.inputs.push_back(content);
    auto emb_res = os.kernel().embed(emb_req);

    memory::Embedding emb;
    if (emb_res && !emb_res->embeddings.empty()) {
        emb = std::move(emb_res->embeddings[0]);
    }

    auto result = os.memory().remember(
        std::move(content), emb, source, importance);
    std::string id = unwrap_or_throw("agentOS::remember", std::move(result));
    return new String(id);
    DDB_SAFE_END("agentOS::remember")
}

// ─── agentOS::recall(query, [topK]) ─────────────────────────

ConstantSP agentOSRecall(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::recall(query, [topK])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::recall", usage);

    std::string query = args[0]->getString();
    if (query.empty())
        throw IllegalArgumentException("agentOS::recall", usage);

    int top_k = 5;
    if (args.size() >= 2 && args[1]->getType() != DT_VOID) {
        top_k = args[1]->getInt();
        if (top_k <= 0) top_k = 5;
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    kernel::EmbeddingRequest emb_req;
    emb_req.inputs.push_back(query);
    auto emb_res = os.kernel().embed(emb_req);

    memory::Embedding emb;
    if (emb_res && !emb_res->embeddings.empty()) {
        emb = std::move(emb_res->embeddings[0]);
    }

    auto results = unwrap_or_throw("agentOS::recall",
        os.memory().recall(emb, {}, static_cast<size_t>(top_k)));
    return search_results_to_table(results);
    DDB_SAFE_END("agentOS::recall")
}

// ─── agentOS::graphAddNode(nodeJson) ─────────────────────────

ConstantSP agentOSGraphAddNode(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::graphAddNode(nodeJson)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::graphAddNode", usage);

    auto j = parse_json(args[0]->getString());

    std::string node_id = j.value("id", "");
    if (node_id.empty())
        throw IllegalArgumentException("agentOS::graphAddNode", "'id' is required");

    std::string node_type = j.value("type", "entity");

    // properties → content (JSON 序列化)
    std::string content_str;
    if (j.contains("properties") && j["properties"].is_object()) {
        content_str = j["properties"].dump();
    } else {
        content_str = j.value("content", "");
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    memory::GraphNode node;
    node.id = node_id;
    node.type = node_type;
    node.content = std::move(content_str);

    auto result = graph.add_node(std::move(node));
    unwrap_or_throw("agentOS::graphAddNode", std::move(result));

    return new String(node_id);
    DDB_SAFE_END("agentOS::graphAddNode")
}

// ─── agentOS::graphAddEdge(edgeJson) ─────────────────────────

ConstantSP agentOSGraphAddEdge(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::graphAddEdge(edgeJson)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::graphAddEdge", usage);

    auto j = parse_json(args[0]->getString());

    std::string source = j.value("source", "");
    std::string target = j.value("target", "");
    std::string relation = j.value("relation", "related_to");

    if (source.empty() || target.empty())
        throw IllegalArgumentException("agentOS::graphAddEdge",
            "'source' and 'target' are required");

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    memory::GraphEdge edge;
    edge.source_id = source;
    edge.target_id = target;
    edge.relation = relation;
    edge.weight = j.value("weight", 1.0f);

    auto result = graph.add_edge(std::move(edge));
    unwrap_or_throw("agentOS::graphAddEdge", std::move(result));

    return new Bool(true);
    DDB_SAFE_END("agentOS::graphAddEdge")
}

// ─── agentOS::graphQuery(nodeIdOrQuery, [maxResults]) ────────

ConstantSP agentOSGraphQuery(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::graphQuery(nodeId, [maxResults])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::graphQuery", usage);

    std::string query = args[0]->getString();
    if (query.empty())
        throw IllegalArgumentException("agentOS::graphQuery", usage);

    int max_results = 10;
    if (args.size() >= 2 && args[1]->getType() != DT_VOID) {
        max_results = args[1]->getInt();
        if (max_results <= 0) max_results = 10;
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    // 先尝试直接按节点 ID 查邻接边
    auto edges_result = graph.get_edges(query);
    if (!edges_result.has_value()) {
        // 不是精确节点 ID，尝试 k-hop 子图搜索（k=1）
        auto subgraph_result = graph.k_hop_search(query, 1);
        if (!subgraph_result.has_value() || subgraph_result->edges.empty()) {
            // 返回空表
            vector<string> colNames{"source", "relation", "target", "weight"};
            vector<DATA_TYPE> colTypes{DT_STRING, DT_STRING, DT_STRING, DT_DOUBLE};
            return Util::createTable(colNames, colTypes, 0, 0);
        }
        edges_result = std::move(subgraph_result->edges);
    }

    auto& edges = *edges_result;
    int n = static_cast<int>(std::min(edges.size(), static_cast<size_t>(max_results)));

    vector<string> colNames{"source", "relation", "target", "weight"};
    vector<DATA_TYPE> colTypes{DT_STRING, DT_STRING, DT_STRING, DT_DOUBLE};
    TableSP table = Util::createTable(colNames, colTypes, 0, n);

    VectorSP cSrc = Util::createVector(DT_STRING, n);
    VectorSP cRel = Util::createVector(DT_STRING, n);
    VectorSP cTgt = Util::createVector(DT_STRING, n);
    VectorSP cWeight = Util::createVector(DT_DOUBLE, n);

    for (int i = 0; i < n; ++i) {
        auto idx = static_cast<size_t>(i);
        cSrc->setString(i, edges[idx].source_id);
        cRel->setString(i, edges[idx].relation);
        cTgt->setString(i, edges[idx].target_id);
        cWeight->setDouble(i, static_cast<double>(edges[idx].weight));
    }

    vector<ConstantSP> cols{cSrc, cRel, cTgt, cWeight};
    INDEX insertedRows;
    string errMsg;
    table->append(cols, insertedRows, errMsg);

    return table;
    DDB_SAFE_END("agentOS::graphQuery")
}

// ─── agentOS::health() ──────────────────────────────────────

ConstantSP agentOSHealth(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;

    if (!PluginRuntime::instance().is_initialized()) {
        return new String("{\"healthy\":false,\"error\":\"not initialized\"}");
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();
    auto status = os.health();
    return new String(status.to_json());
    DDB_SAFE_END("agentOS::health")
}

// ─── agentOS::status() ──────────────────────────────────────

ConstantSP agentOSStatus(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;

    if (!PluginRuntime::instance().is_initialized()) {
        return new String("AgentOS: not initialized");
    }

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();
    return new String(os.status());
    DDB_SAFE_END("agentOS::status")
}

// ─── agentOS::registerTool(schemaJson, callbackName) ─────────

ConstantSP agentOSRegisterTool(Heap* heap, vector<ConstantSP>& args) {
    const string usage = "Usage: agentOS::registerTool(schemaJson, callbackFuncName)";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::registerTool", usage);
    if (!args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::registerTool", usage + " schemaJson must be a string.");
    if (!args[1]->isScalar() || args[1]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::registerTool", usage + " callbackFuncName must be a string.");

    auto schema_json = parse_json(args[0]->getString());
    std::string callback_name = args[1]->getString();

    if (callback_name.empty())
        throw IllegalArgumentException("agentOS::registerTool", "callbackFuncName must not be empty.");

    tools::ToolSchema schema;
    schema.id = schema_json.value("id", "");
    schema.description = schema_json.value("description", "");

    if (schema.id.empty())
        throw IllegalArgumentException("agentOS::registerTool", "'id' is required in schema.");

    if (schema_json.contains("params") && schema_json["params"].is_array()) {
        for (const auto& p : schema_json["params"]) {
            tools::ParamDef param;
            param.name = p.value("name", "");
            param.description = p.value("description", "");
            param.required = p.value("required", true);

            std::string type_str = p.value("type", "string");
            if (type_str == "int" || type_str == "integer") {
                param.type = tools::ParamType::Integer;
            } else if (type_str == "float" || type_str == "number") {
                param.type = tools::ParamType::Float;
            } else if (type_str == "bool" || type_str == "boolean") {
                param.type = tools::ParamType::Boolean;
            } else {
                param.type = tools::ParamType::String;
            }
            schema.params.push_back(std::move(param));
        }
    }

    schema.timeout_ms = schema_json.value("timeout_ms", 30000u);
    schema.is_dangerous = schema_json.value("is_dangerous", false);

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    // 捕获 heap 和 callback_name，通过 Session 执行 DolphinDB 函数
    // NOTE: heap 指针在当前调用栈有效；长期使用需 copy Session
    SessionSP session = heap->currentSession()->copy();
    session->setUser(heap->currentSession()->getUser());

    os.register_tool(std::move(schema),
        [callback_name, session](const tools::ParsedArgs& args) -> tools::ToolResult {
            try {
                // 解析参数为 JSON
                nlohmann::json args_json;
                for (const auto& [key, value] : args.values) {
                    args_json[key] = value;
                }

                // 通过 Session 解析并调用 DolphinDB 端定义的函数
                FunctionDefSP func = session->getFunctionDef(callback_name);
                if (func.isNull()) {
                    return tools::ToolResult{
                        .success = false,
                        .output = "",
                        .error = "DolphinDB function '" + callback_name + "' not found"
                    };
                }

                // 将 JSON args 转换为 DolphinDB String 参数
                vector<ConstantSP> ddb_args;
                ddb_args.push_back(new String(args_json.dump()));

                ConstantSP result = func->call(session->getHeap().get(), ddb_args);
                return tools::ToolResult{
                    .success = true,
                    .output = result->getString(),
                    .error = ""
                };
            } catch (const std::exception& e) {
                return tools::ToolResult{
                    .success = false,
                    .output = "",
                    .error = std::string("DolphinDB callback error: ") + e.what()
                };
            }
        });

    return new Bool(true);
    DDB_SAFE_END("agentOS::registerTool")
}

// ─── agentOS::createAgent(configJson) ────────────────────────

ConstantSP agentOSCreateAgent(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::createAgent(configJson)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::createAgent", usage);

    auto cfg_json = parse_json(args[0]->getString());

    AgentConfig cfg;
    cfg.name = cfg_json.value("name", "ddb_agent");
    cfg.role_prompt = cfg_json.value("role_prompt",
        "You are a helpful AI assistant integrated with DolphinDB.");
    cfg.security_role = cfg_json.value("security_role", "standard");
    cfg.context_limit = cfg_json.value("context_limit", 8192u);
    cfg.persist_memory = cfg_json.value("persist_memory", false);

    if (cfg_json.contains("allowed_tools") && cfg_json["allowed_tools"].is_array()) {
        for (const auto& t : cfg_json["allowed_tools"]) {
            cfg.allowed_tools.push_back(t.get<std::string>());
        }
    }

    DDB_SAFE_BEGIN
    long long handle = PluginRuntime::instance().create_agent(cfg);
    return new Long(handle);
    DDB_SAFE_END("agentOS::createAgent")
}

// ─── agentOS::destroyAgent(handle) ───────────────────────────

ConstantSP agentOSDestroyAgent(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::destroyAgent(handle)";
    if (args.empty())
        throw IllegalArgumentException("agentOS::destroyAgent", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    bool ok = PluginRuntime::instance().destroy_agent(handle);
    return new Bool(ok);
    DDB_SAFE_END("agentOS::destroyAgent")
}

// ─── agentOS::askAsync(question, [systemPrompt]) ─────────────
//
// 异步发起 LLM 流式请求，立即返回 requestId。
// 后台线程通过 stream_infer 逐 token 写入 AsyncRequest。
// 前端轮询 agentOS::poll(requestId) 获取增量内容。

ConstantSP agentOSAskAsync(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::askAsync(question, [systemPrompt])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::askAsync", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::askAsync", usage);

    std::string system_prompt;
    if (args.size() >= 2 && !args[1]->isNull() && args[1]->getType() == DT_STRING) {
        system_prompt = args[1]->getString();
    }

    // 确保 AgentOS 已初始化
    auto& os = PluginRuntime::instance().os();
    (void)os;

    // 创建异步请求
    auto& mgr = AsyncRequestManager::instance();
    std::string rid = mgr.create();
    auto req_ptr = mgr.find(rid);

    // 捕获值到后台线程（不捕获引用，线程生命周期独立）
    std::string sys_prompt = system_prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB."
        : system_prompt;

    req_ptr->worker = std::thread([req_ptr, question, sys_prompt]() {
        try {
            auto& os = PluginRuntime::instance().os();

            kernel::LLMRequest llm_req;
            llm_req.messages.push_back(kernel::Message::system(sys_prompt));
            llm_req.messages.push_back(kernel::Message::user(question));

            auto result = os.kernel().stream_infer(llm_req,
                [&req_ptr](std::string_view token) {
                    req_ptr->append_token(token);
                });

            if (result.has_value()) {
                // 如果 stream_infer 返回了内容但 token handler 没收到
                // （某些后端可能不走 streaming），补充到 content
                auto& resp = result.value();
                std::lock_guard lk(req_ptr->mu);
                if (req_ptr->content.empty() && !resp.content.empty()) {
                    req_ptr->content = resp.content;
                    req_ptr->delta = resp.content;
                }
            } else {
                auto& err = result.error();
                req_ptr->mark_error(
                    "AgentOS error [" +
                    std::to_string(static_cast<int>(err.code)) +
                    "]: " + err.message);
                return;
            }
            req_ptr->mark_done();
        } catch (const std::exception& e) {
            req_ptr->mark_error(std::string("Internal error: ") + e.what());
        } catch (...) {
            req_ptr->mark_error("Unknown internal error");
        }
    });

    // 返回带 __stream__ 标记的 dict，前端据此进入流式渲染模式
    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);
    dict->set(new String("__stream__"), new Bool(true));
    dict->set(new String("requestId"),  new String(rid));
    dict->set(new String("status"),     new String("streaming"));

#ifdef AGENTOS_ENABLE_SSE
    // 生成一次性 SSE 令牌，有效期 60 秒
    if (SSEServer::instance().is_running()) {
        std::string token = SSETokenManager::instance().generate(rid, 60);
        std::string sse_url = SSEServer::instance().base_url() + "/sse";
        dict->set(new String("sseUrl"), new String(sse_url));
        dict->set(new String("token"),  new String(token));
    }
#endif

    return dict;
}

// ─── agentOS::poll(requestId) ────────────────────────────────
//
// 纯增量返回：streaming 期间只传 delta，完成时才带 content
//
// 返回 DolphinDB Dictionary:
//   status : STRING  ("streaming" | "done" | "error")
//   delta  : STRING  (本次 poll 新增的 tokens，增量)
//   content: STRING  (仅 done/error 时返回完整内容，streaming 时为空)
//   done   : BOOL    (是否已完成)
//   error  : STRING  (错误信息，无错误时为空)

ConstantSP agentOSPoll(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::poll(requestId)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::poll", usage);

    std::string rid = args[0]->getString();
    if (rid.empty())
        throw IllegalArgumentException("agentOS::poll", usage);

    auto req = AsyncRequestManager::instance().find(rid);
    if (!req) {
        throw IllegalArgumentException("agentOS::poll",
            "Unknown requestId: " + rid + ". Call agentOS::askAsync first.");
    }

    auto [status, delta, content, error] = req->poll();

    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);

    std::string status_str;
    bool done = false;
    switch (status) {
        case AsyncRequest::Status::Running:
            status_str = "streaming";
            break;
        case AsyncRequest::Status::Done:
            status_str = "done";
            done = true;
            break;
        case AsyncRequest::Status::Error:
            status_str = "error";
            done = true;
            break;
    }

    dict->set(new String("status"),  new String(status_str));
    dict->set(new String("delta"),   new String(delta));
    // streaming 期间不传 content（省带宽），完成/出错时才带全量
    dict->set(new String("content"), new String(done ? content : ""));
    dict->set(new String("done"),    new Bool(done));
    dict->set(new String("error"),   new String(error));

    // 完成后自动清理（join worker thread）
    if (done && req->worker.joinable()) {
        req->worker.join();
    }

    return dict;
}

// ─── agentOS::cancelAsync(requestId) ─────────────────────────

ConstantSP agentOSCancelAsync(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::cancelAsync(requestId)";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::cancelAsync", usage);

    std::string rid = args[0]->getString();
    AsyncRequestManager::instance().remove(rid);
    return new Bool(true);
}

// ═══════════════════════════════════════════════════════════════
// RAG: KnowledgeBase 系列函数
// ═══════════════════════════════════════════════════════════════

// ─── agentOS::createKB([configJson]) ─────────────────────────
//
// 创建知识库实例，返回 handle (LONG)。
// configJson 可选字段:
//   vector_dim       : INT    (默认 1536)
//   max_chunks       : INT    (默认 100000)
//   embedding_model  : STRING (默认 "text-embedding-3-small")
//   chunk_size       : INT    (默认 500)
//   chunk_overlap    : INT    (默认 50)

ConstantSP agentOSCreateKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    DDB_SAFE_BEGIN

    auto& os = PluginRuntime::instance().os();

    // 默认参数
    uint32_t vector_dim = 1536;
    size_t max_chunks = 100000;
    std::string embedding_model = "text-embedding-3-small";
    size_t chunk_size = 500;
    size_t chunk_overlap = 50;

    // 解析可选 configJson
    if (!args.empty() && !args[0]->isNull() && args[0]->getType() == DT_STRING) {
        std::string cfg_str = args[0]->getString();
        if (!cfg_str.empty()) {
            auto cfg = nlohmann::json::parse(cfg_str, nullptr, false);
            if (!cfg.is_discarded()) {
                if (cfg.contains("vector_dim"))      vector_dim      = cfg["vector_dim"].get<uint32_t>();
                if (cfg.contains("max_chunks"))      max_chunks      = cfg["max_chunks"].get<size_t>();
                if (cfg.contains("embedding_model")) embedding_model = cfg["embedding_model"].get<std::string>();
                if (cfg.contains("chunk_size"))      chunk_size      = cfg["chunk_size"].get<size_t>();
                if (cfg.contains("chunk_overlap"))   chunk_overlap   = cfg["chunk_overlap"].get<size_t>();
            }
        }
    }

    // 获取 LLM backend 用于 embedding
    // 由于 LLMKernel 现持有 unique_ptr，使用一个无 deleter 的 shared_ptr 包装引用
    auto llm = std::shared_ptr<kernel::ILLMBackend>(&os.kernel().backend(), [](kernel::ILLMBackend*){});

    auto kb = std::make_shared<knowledge::KnowledgeBase>(
        llm, vector_dim, max_chunks, embedding_model);
    kb->set_chunk_params(chunk_size, chunk_overlap);

    long long handle = KBManager::instance().create(std::move(kb));
    return new Long(handle);

    DDB_SAFE_END("agentOS::createKB")
}

// ─── agentOS::destroyKB(handle) ──────────────────────────────

ConstantSP agentOSDestroyKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::destroyKB(handle)";
    if (args.empty()) throw IllegalArgumentException("agentOS::destroyKB", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    bool ok = KBManager::instance().destroy(handle);
    if (!ok) throw IllegalArgumentException("agentOS::destroyKB", "Unknown KB handle: " + std::to_string(handle));
    return new Bool(true);
    DDB_SAFE_END("agentOS::destroyKB")
}

// ─── agentOS::saveKB(handle, dirPath) ────────────────────────

ConstantSP agentOSSaveKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::saveKB(handle, dirPath)";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::saveKB", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string dir_path = args[1]->getString();

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::saveKB", "Unknown KB handle: " + std::to_string(handle));

    bool ok = kb->save(dir_path);
    if (!ok) throw RuntimeException("agentOS::saveKB: failed to save to " + dir_path);
    return new Bool(true);
    DDB_SAFE_END("agentOS::saveKB")
}

// ─── agentOS::loadKB(handle, dirPath) ────────────────────────

ConstantSP agentOSLoadKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::loadKB(handle, dirPath)";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::loadKB", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string dir_path = args[1]->getString();

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::loadKB", "Unknown KB handle: " + std::to_string(handle));

    bool ok = kb->load(dir_path);
    if (!ok) throw RuntimeException("agentOS::loadKB: failed to load from " + dir_path);
    return new Bool(true);
    DDB_SAFE_END("agentOS::loadKB")
}

// ─── agentOS::ingest(handle, docId, text) ────────────────────

ConstantSP agentOSIngest(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::ingest(handle, docId, text)";
    if (args.size() < 3) throw IllegalArgumentException("agentOS::ingest", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string doc_id = args[1]->getString();
    std::string text = args[2]->getString();

    if (doc_id.empty()) throw IllegalArgumentException("agentOS::ingest", "docId must not be empty");
    if (text.empty()) throw IllegalArgumentException("agentOS::ingest", "text must not be empty");

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::ingest", "Unknown KB handle: " + std::to_string(handle));

    auto result = kb->ingest_text(doc_id, text);
    if (!result.has_value()) {
        throw RuntimeException("agentOS::ingest: " + result.error().message);
    }
    return new Int(static_cast<int>(result.value()));
    DDB_SAFE_END("agentOS::ingest")
}

// ─── agentOS::ingestDir(handle, dirPath) ─────────────────────

ConstantSP agentOSIngestDir(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::ingestDir(handle, dirPath)";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::ingestDir", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string dir_path = args[1]->getString();

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::ingestDir", "Unknown KB handle: " + std::to_string(handle));

    kb->ingest_directory(dir_path);
    return new Bool(true);
    DDB_SAFE_END("agentOS::ingestDir")
}

// ─── agentOS::removeDoc(handle, docId) ───────────────────────

ConstantSP agentOSRemoveDoc(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::removeDoc(handle, docId)";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::removeDoc", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string doc_id = args[1]->getString();

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::removeDoc", "Unknown KB handle: " + std::to_string(handle));

    bool ok = kb->remove_document(doc_id);
    return new Bool(ok);
    DDB_SAFE_END("agentOS::removeDoc")
}

// ─── agentOS::search(handle, query, [topK], [graphHops]) ─────
//
// 返回 TABLE: doc_id(STRING), chunk_id(STRING), content(STRING),
//             score(DOUBLE), graph_context(STRING)

ConstantSP agentOSSearch(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::search(handle, query, [topK], [graphHops])";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::search", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string query = args[1]->getString();
    size_t top_k = 5;
    int graph_hops = 0;

    if (args.size() >= 3 && !args[2]->isNull()) top_k = static_cast<size_t>(args[2]->getInt());
    if (args.size() >= 4 && !args[3]->isNull()) graph_hops = args[3]->getInt();

    if (query.empty()) throw IllegalArgumentException("agentOS::search", "query must not be empty");

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::search", "Unknown KB handle: " + std::to_string(handle));

    auto results = kb->search(query, top_k, graph_hops);

    // 构建 DolphinDB Table
    size_t n = results.size();
    auto col_doc_id        = Util::createVector(DT_STRING, n);
    auto col_chunk_id      = Util::createVector(DT_STRING, n);
    auto col_content       = Util::createVector(DT_STRING, n);
    auto col_score         = Util::createVector(DT_DOUBLE, n);
    auto col_graph_context = Util::createVector(DT_STRING, n);

    for (size_t i = 0; i < n; ++i) {
        col_doc_id->setString(i, results[i].doc_id);
        col_chunk_id->setString(i, results[i].chunk_id);
        col_content->setString(i, results[i].content);
        col_score->setDouble(i, results[i].score);
        col_graph_context->setString(i, results[i].graph_context);
    }

    vector<std::string> col_names = {"doc_id", "chunk_id", "content", "score", "graph_context"};
    vector<ConstantSP> cols = {col_doc_id, col_chunk_id, col_content, col_score, col_graph_context};
    return Util::createTable(col_names, cols);
    DDB_SAFE_END("agentOS::search")
}

// ─── agentOS::askWithKB(handle, question, [topK], [systemPrompt]) ──
//
// RAG 对话: 检索知识库 → 拼接上下文 → LLM 生成
// 返回 STRING — LLM 回答

ConstantSP agentOSAskWithKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::askWithKB(handle, question, [topK], [systemPrompt])";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::askWithKB", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string question = args[1]->getString();
    size_t top_k = 5;
    std::string system_prompt;

    if (args.size() >= 3 && !args[2]->isNull()) top_k = static_cast<size_t>(args[2]->getInt());
    if (args.size() >= 4 && !args[3]->isNull() && args[3]->getType() == DT_STRING) {
        system_prompt = args[3]->getString();
    }

    if (question.empty()) throw IllegalArgumentException("agentOS::askWithKB", "question must not be empty");

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::askWithKB", "Unknown KB handle: " + std::to_string(handle));

    auto& os = PluginRuntime::instance().os();

    // Step 1: 检索知识库
    auto results = kb->search(question, top_k);

    // Step 2: 拼接检索结果为 context
    std::string context;
    for (size_t i = 0; i < results.size(); ++i) {
        context += "[" + std::to_string(i + 1) + "] (来源: " + results[i].doc_id + ")\n";
        context += results[i].content + "\n\n";
        if (!results[i].graph_context.empty()) {
            context += "相关知识图谱:\n" + results[i].graph_context + "\n\n";
        }
    }

    // Step 3: 构建 RAG prompt
    std::string rag_system = system_prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB. "
          "Answer questions based on the provided context. "
          "If the context doesn't contain relevant information, say so."
        : system_prompt;

    std::string rag_user = "Based on the following context, answer the question.\n\n"
                           "## Context\n" + context +
                           "## Question\n" + question;

    // Step 4: LLM 推理
    kernel::LLMRequest llm_req;
    llm_req.messages.push_back(kernel::Message::system(rag_system));
    llm_req.messages.push_back(kernel::Message::user(rag_user));

    std::string full_response;
    auto result = os.kernel().stream_infer(llm_req,
        [&full_response](std::string_view token) {
            full_response += std::string(token);
        });

    if (!result.has_value()) {
        auto& err = result.error();
        throw RuntimeException("agentOS::askWithKB LLM error: " + err.message);
    }

    if (full_response.empty() && result.value().content.size() > 0) {
        full_response = result.value().content;
    }

    return new String(full_response.empty() ? "(no response)" : full_response);
    DDB_SAFE_END("agentOS::askWithKB")
}

// ─── agentOS::kbInfo(handle) ─────────────────────────────────
//
// 返回 DICTIONARY: chunk_count, doc_count, embedding_model, chunk_size, chunk_overlap

ConstantSP agentOSKBInfo(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::kbInfo(handle)";
    if (args.empty()) throw IllegalArgumentException("agentOS::kbInfo", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::kbInfo", "Unknown KB handle: " + std::to_string(handle));

    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);
    dict->set(new String("chunk_count"),    new Long(static_cast<long long>(kb->chunk_count())));
    dict->set(new String("doc_count"),      new Long(static_cast<long long>(kb->document_count())));
    dict->set(new String("embedding_model"),new String(kb->embedding_model()));
    dict->set(new String("chunk_size"),     new Int(static_cast<int>(kb->chunk_size())));
    dict->set(new String("chunk_overlap"),  new Int(static_cast<int>(kb->chunk_overlap())));
    return dict;
    DDB_SAFE_END("agentOS::kbInfo")
}

// ─── agentOS::askWithKBAsync(handle, question, [topK], [systemPrompt]) ──
//
// 异步 RAG 对话：检索知识库 → 后台线程流式 LLM 生成
// 返回 dict: {__stream__: true, requestId, status, sseUrl?, token?}

ConstantSP agentOSAskWithKBAsync(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::askWithKBAsync(handle, question, [topK], [systemPrompt])";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::askWithKBAsync", usage);

    DDB_SAFE_BEGIN
    long long handle = args[0]->getLong();
    std::string question = args[1]->getString();
    size_t top_k = 5;
    std::string system_prompt;

    if (args.size() >= 3 && !args[2]->isNull()) top_k = static_cast<size_t>(args[2]->getInt());
    if (args.size() >= 4 && !args[3]->isNull() && args[3]->getType() == DT_STRING) {
        system_prompt = args[3]->getString();
    }

    if (question.empty()) throw IllegalArgumentException("agentOS::askWithKBAsync", "question must not be empty");

    auto kb = KBManager::instance().find(handle);
    if (!kb) throw IllegalArgumentException("agentOS::askWithKBAsync", "Unknown KB handle: " + std::to_string(handle));

    auto& os = PluginRuntime::instance().os();
    (void)os;

    // Step 1: 检索知识库（同步，通常毫秒级）
    auto search_results = kb->search(question, top_k);

    // 拼接 context
    std::string context;
    for (size_t i = 0; i < search_results.size(); ++i) {
        context += "[" + std::to_string(i + 1) + "] (来源: " + search_results[i].doc_id + ")\n";
        context += search_results[i].content + "\n\n";
        if (!search_results[i].graph_context.empty()) {
            context += "相关知识图谱:\n" + search_results[i].graph_context + "\n\n";
        }
    }

    std::string rag_system = system_prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB. "
          "Answer questions based on the provided context. "
          "If the context doesn't contain relevant information, say so."
        : system_prompt;

    std::string rag_user = "Based on the following context, answer the question.\n\n"
                           "## Context\n" + context +
                           "## Question\n" + question;

    // Step 2: 创建异步请求，后台线程流式生成
    auto& mgr = AsyncRequestManager::instance();
    std::string rid = mgr.create();
    auto req_ptr = mgr.find(rid);

    req_ptr->worker = std::thread([req_ptr, rag_system, rag_user]() {
        try {
            auto& os = PluginRuntime::instance().os();

            kernel::LLMRequest llm_req;
            llm_req.messages.push_back(kernel::Message::system(rag_system));
            llm_req.messages.push_back(kernel::Message::user(rag_user));

            auto result = os.kernel().stream_infer(llm_req,
                [&req_ptr](std::string_view token) {
                    req_ptr->append_token(token);
                });

            if (result.has_value()) {
                auto& resp = result.value();
                std::lock_guard lk(req_ptr->mu);
                if (req_ptr->content.empty() && !resp.content.empty()) {
                    req_ptr->content = resp.content;
                    req_ptr->delta = resp.content;
                }
            } else {
                auto& err = result.error();
                req_ptr->mark_error("LLM error: " + err.message);
                return;
            }
            req_ptr->mark_done();
        } catch (const std::exception& e) {
            req_ptr->mark_error(std::string("Internal error: ") + e.what());
        } catch (...) {
            req_ptr->mark_error("Unknown internal error");
        }
    });

    // Step 3: 返回 __stream__ dict
    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);
    dict->set(new String("__stream__"), new Bool(true));
    dict->set(new String("requestId"),  new String(rid));
    dict->set(new String("status"),     new String("streaming"));

#ifdef AGENTOS_ENABLE_SSE
    if (SSEServer::instance().is_running()) {
        std::string token = SSETokenManager::instance().generate(rid, 60);
        std::string sse_url = SSEServer::instance().base_url() + "/sse";
        dict->set(new String("sseUrl"), new String(sse_url));
        dict->set(new String("token"),  new String(token));
    }
#endif

    return dict;
    DDB_SAFE_END("agentOS::askWithKBAsync")
}

} // extern "C"

} // namespace agentos::dolphindb
