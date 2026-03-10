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
#include <iostream>
#include <sstream>

namespace agentos::dolphindb {

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

    auto& os = PluginRuntime::instance().os();

    AgentConfig cfg;
    cfg.name = "ddb_ask_temp";
    cfg.role_prompt = system_prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB."
        : system_prompt;
    cfg.context_limit = 8192;

    auto agent = os.create_agent(cfg);
    auto result = agent->run(question);
    os.destroy_agent(agent->id());

    std::string answer = unwrap_or_throw("agentOS::ask", std::move(result));
    return new String(answer);
}

// ─── agentOS::askStream(question, [systemPrompt]) ────────────

ConstantSP agentOSAskStream(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::askStream(question, [systemPrompt])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::askStream", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::askStream", usage);

    std::string system_prompt;
    if (args.size() >= 2 && args[1]->getType() == DT_STRING) {
        system_prompt = args[1]->getString();
    }

    auto& os = PluginRuntime::instance().os();

    kernel::LLMRequest req;
    req.messages.push_back(kernel::Message::system(
        system_prompt.empty()
            ? "You are a helpful AI assistant integrated with DolphinDB."
            : system_prompt));
    req.messages.push_back(kernel::Message::user(question));

    std::string full_response;
    auto result = os.kernel().stream_infer(req,
        [&full_response](std::string_view token) {
            std::cout << token << std::flush;
            full_response += std::string(token);
        });
    std::cout << std::endl;

    auto resp = unwrap_or_throw("agentOS::askStream", std::move(result));
    return new String(full_response.empty() ? resp.content : full_response);
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
            if (tool_result.has_value()) {
                obs = tool_result->success ? tool_result->output : tool_result->error;
            } else {
                obs = "Error: " + tool_result.error().message;
            }

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

    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    memory::GraphNode node;
    node.id = node_id;
    node.type = node_type;
    node.content = std::move(content_str);

    auto result = graph.add_node(std::move(node));
    unwrap_or_throw("agentOS::graphAddNode", std::move(result));

    return new String(node_id);
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
}

// ─── agentOS::health() ──────────────────────────────────────

ConstantSP agentOSHealth(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;

    if (!PluginRuntime::instance().is_initialized()) {
        return new String("{\"healthy\":false,\"error\":\"not initialized\"}");
    }

    auto& os = PluginRuntime::instance().os();
    auto status = os.health();
    return new String(status.to_json());
}

// ─── agentOS::status() ──────────────────────────────────────

ConstantSP agentOSStatus(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;

    if (!PluginRuntime::instance().is_initialized()) {
        return new String("AgentOS: not initialized");
    }

    auto& os = PluginRuntime::instance().os();
    return new String(os.status());
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

    auto& os = PluginRuntime::instance().os();

    // 捕获 heap 和 callback_name，通过 Session 执行 DolphinDB 函数
    // NOTE: heap 指针在当前调用栈有效；长期使用需 copy Session
    SessionSP session = heap->currentSession()->copy();
    session->setUser(heap->currentSession()->getUser());

    os.register_tool(std::move(schema),
        [callback_name, session](const nlohmann::json& args_json) -> tools::ToolResult {
            try {
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

    long long handle = PluginRuntime::instance().create_agent(cfg);
    return new Long(handle);
}

// ─── agentOS::destroyAgent(handle) ───────────────────────────

ConstantSP agentOSDestroyAgent(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::destroyAgent(handle)";
    if (args.empty())
        throw IllegalArgumentException("agentOS::destroyAgent", usage);

    long long handle = args[0]->getLong();
    bool ok = PluginRuntime::instance().destroy_agent(handle);
    return new Bool(ok);
}

} // extern "C"

} // namespace agentos::dolphindb
