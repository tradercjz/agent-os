// ============================================================
// AgentOS :: DolphinDB Plugin Adapter — Implementation
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
        LOG_INFO("AgentOS DolphinDB plugin: already initialized, skipping");
        return false;
    }

    // 解析配置
    nlohmann::json cfg;
    try {
        cfg = nlohmann::json::parse(config_json);
    } catch (const std::exception& e) {
        throw RuntimeException(
            std::string("agentOS_init: invalid JSON config: ") + e.what());
    }

    // 提取 LLM 配置
    std::string api_key = cfg.value("api_key", "");
    std::string base_url = cfg.value("base_url", "https://api.openai.com/v1");
    std::string model = cfg.value("model", "gpt-4o-mini");

    // 选择后端
    if (api_key.empty()) {
        // Mock 模式（无 API key 时降级为 Mock）
        LOG_WARN("AgentOS DolphinDB plugin: no api_key provided, using MockBackend");
        backend_ = std::make_unique<kernel::MockLLMBackend>("dolphindb-mock");
    } else {
        backend_ = std::make_unique<kernel::OpenAIBackend>(
            std::move(api_key), std::move(base_url), std::move(model));
    }

    // AgentOS 配置
    AgentOS::Config os_cfg;
    os_cfg.scheduler_threads = cfg.value("scheduler_threads", 4u);
    os_cfg.tpm_limit = cfg.value("tpm_limit", 100000u);

    // DolphinDB 环境下使用确定性目录
    std::string data_dir = cfg.value("data_dir", "/tmp/agentos_dolphindb");
    os_cfg.snapshot_dir = data_dir + "/snapshots";
    os_cfg.ltm_dir = data_dir + "/ltm";
    os_cfg.enable_security = cfg.value("enable_security", true);

    os_ = std::make_unique<AgentOS>(std::move(backend_), os_cfg);

    LOG_INFO("AgentOS DolphinDB plugin: initialized successfully");
    return true;
}

void PluginRuntime::close() {
    std::lock_guard lk(mu_);
    if (!os_) return;

    // 先销毁所有 Agent
    for (auto& [handle, agent] : agents_) {
        os_->destroy_agent(agent->id());
    }
    agents_.clear();

    os_->graceful_shutdown();
    os_.reset();
    LOG_INFO("AgentOS DolphinDB plugin: closed");
}

AgentOS& PluginRuntime::os() {
    std::lock_guard lk(mu_);
    if (!os_) {
        throw RuntimeException("AgentOS not initialized. Call agentOS_init() first.");
    }
    return *os_;
}

bool PluginRuntime::is_initialized() const noexcept {
    std::lock_guard lk(mu_);
    return os_ != nullptr;
}

std::shared_ptr<Agent> PluginRuntime::find_agent(int handle) const {
    std::lock_guard lk(mu_);
    auto it = agents_.find(handle);
    if (it == agents_.end()) return nullptr;
    return it->second;
}

int PluginRuntime::create_agent(const AgentConfig& cfg) {
    std::lock_guard lk(mu_);
    if (!os_) {
        throw RuntimeException("AgentOS not initialized");
    }
    auto agent = os_->create_agent(cfg);
    int handle = next_handle_++;
    agents_[handle] = agent;
    return handle;
}

bool PluginRuntime::destroy_agent(int handle) {
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

std::string to_string(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return "";
    }
    return val->getString();
}

int to_int(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return 0;
    }
    return val->getInt();
}

float to_float(const ConstantSP& val) {
    if (val.isNull() || val->isNull() || val->getType() == DT_VOID) {
        return 0.0f;
    }
    return val->getFloat();
}

nlohmann::json to_json(const ConstantSP& val) {
    std::string s = to_string(val);
    if (s.empty()) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(s);
    } catch (...) {
        throw RuntimeException("Invalid JSON: " + s);
    }
}

ConstantSP make_string(const std::string& s) {
    return Util::createString(s);
}

ConstantSP make_int(int v) {
    return Util::createInt(v);
}

ConstantSP make_bool(bool v) {
    return Util::createBool(v);
}

ConstantSP make_float(float v) {
    return Util::createFloat(v);
}

TableSP search_results_to_table(
    const std::vector<memory::SearchResult>& results) {
    // 构建列
    size_t n = results.size();
    ConstantSP col_content = Util::createVector(DT_STRING, static_cast<int>(n));
    ConstantSP col_score = Util::createVector(DT_FLOAT, static_cast<int>(n));
    ConstantSP col_source = Util::createVector(DT_STRING, static_cast<int>(n));
    ConstantSP col_timestamp = Util::createVector(DT_LONG, static_cast<int>(n));

    for (size_t i = 0; i < n; ++i) {
        auto idx = static_cast<int>(i);
        col_content->setString(idx, results[i].entry.content);
        col_score->setFloat(idx, results[i].score);
        col_source->setString(idx, results[i].entry.source);
        col_timestamp->setLong(idx, static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                results[i].entry.created_at.time_since_epoch()).count()));
    }

    std::vector<std::string> col_names = {"content", "score", "source", "timestamp"};
    std::vector<ConstantSP> cols = {col_content, col_score, col_source, col_timestamp};
    return Util::createTable(col_names, cols);
}

ConstantSP llm_response_to_dict(const kernel::LLMResponse& resp) {
    // 用 JSON 字符串返回（Dict 在 DolphinDB 中不太好用，STRING JSON 更通用）
    nlohmann::json j;
    j["content"] = resp.content;
    j["finish_reason"] = resp.finish_reason;
    j["prompt_tokens"] = resp.prompt_tokens;
    j["completion_tokens"] = resp.completion_tokens;

    if (!resp.tool_calls.empty()) {
        nlohmann::json tc_arr = nlohmann::json::array();
        for (const auto& tc : resp.tool_calls) {
            tc_arr.push_back({
                {"id", tc.id},
                {"name", tc.name},
                {"args", tc.args_json}
            });
        }
        j["tool_calls"] = tc_arr;
    }

    return make_string(j.dump());
}

// ─────────────────────────────────────────────────────────────
// § D.3  DolphinDB 导出函数实现
// ─────────────────────────────────────────────────────────────

extern "C" {

ConstantSP agentOS_init(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)rhs;
    std::string config = to_string(lhs);
    if (config.empty()) {
        config = "{}";  // 使用默认配置 (Mock 模式)
    }

    bool is_new = PluginRuntime::instance().init(config);
    return make_string(is_new ? "OK: AgentOS initialized" : "OK: AgentOS already running");
}

ConstantSP agentOS_close(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)lhs;
    (void)rhs;
    PluginRuntime::instance().close();
    return make_string("OK: AgentOS closed");
}

ConstantSP agentOS_ask(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string question = to_string(lhs);
    if (question.empty()) {
        throw RuntimeException("agentOS_ask: question must not be empty");
    }

    std::string system_prompt = to_string(rhs);

    auto& os = PluginRuntime::instance().os();

    // 创建临时 Agent
    AgentConfig cfg;
    cfg.name = "ddb_ask_temp";
    cfg.role_prompt = system_prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB."
        : system_prompt;
    cfg.context_limit = 8192;

    auto agent = os.create_agent(cfg);

    // 执行问答
    auto result = agent->run(question);

    // 清理临时 Agent
    os.destroy_agent(agent->id());

    std::string answer = unwrap_or_throw(std::move(result));
    return make_string(answer);
}

ConstantSP agentOS_askStream(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string question = to_string(lhs);
    if (question.empty()) {
        throw RuntimeException("agentOS_askStream: question must not be empty");
    }

    std::string system_prompt = to_string(rhs);

    auto& os = PluginRuntime::instance().os();

    // 使用 LLMKernel 的流式接口
    kernel::LLMRequest req;
    if (!system_prompt.empty()) {
        req.messages.push_back(kernel::Message::system(system_prompt));
    } else {
        req.messages.push_back(kernel::Message::system(
            "You are a helpful AI assistant integrated with DolphinDB."));
    }
    req.messages.push_back(kernel::Message::user(question));

    std::string full_response;
    auto result = os.kernel().stream_infer(req,
        [&full_response](std::string_view token) {
            // 流式输出到 DolphinDB 控制台
            std::cout << token << std::flush;
            full_response += std::string(token);
        });
    std::cout << std::endl;  // 结束行

    auto resp = unwrap_or_throw(std::move(result));
    return make_string(full_response.empty() ? resp.content : full_response);
}

ConstantSP agentOS_askTable(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string question = to_string(lhs);
    if (question.empty()) {
        throw RuntimeException("agentOS_askTable: question must not be empty");
    }

    // 解析可选配置
    nlohmann::json opts;
    std::string rhs_str = to_string(rhs);
    if (!rhs_str.empty()) {
        try {
            opts = nlohmann::json::parse(rhs_str);
        } catch (...) {
            // 当作 system_prompt
            opts["system_prompt"] = rhs_str;
        }
    }

    std::string system_prompt = opts.value("system_prompt",
        "You are a helpful AI assistant integrated with DolphinDB.");
    int max_steps = opts.value("max_steps", 10);
    int agent_handle = opts.value("agent_handle", 0);

    auto& os = PluginRuntime::instance().os();
    auto& runtime = PluginRuntime::instance();

    // 收集对话交互记录
    struct InteractionRow {
        std::string role;
        std::string content;
        std::string tool_name;
        std::string tool_args;
        std::string tool_result;
    };
    std::vector<InteractionRow> rows;

    std::shared_ptr<Agent> agent;
    bool temp_agent = false;

    if (agent_handle > 0) {
        agent = runtime.find_agent(agent_handle);
        if (!agent) {
            throw RuntimeException("agentOS_askTable: invalid agent handle " +
                                   std::to_string(agent_handle));
        }
    } else {
        // 创建临时 Agent
        AgentConfig cfg;
        cfg.name = "ddb_table_temp";
        cfg.role_prompt = system_prompt;
        cfg.context_limit = 8192;
        agent = os.create_agent(cfg);
        temp_agent = true;
    }

    // 使用 LLMKernel 直接进行 ReAct 循环并记录每步
    auto& ctx_win = os.ctx().get_window(agent->id(), agent->config().context_limit);
    ctx_win.try_add(kernel::Message::user(question));
    rows.push_back({"user", question, "", "", ""});

    for (int step = 0; step < max_steps; ++step) {
        // Think
        kernel::LLMRequest req;
        req.agent_id = agent->id();
        req.priority = agent->config().priority;
        for (const auto& m : ctx_win.messages()) {
            req.messages.push_back(m);
        }

        // 注入工具
        std::string tj = os.tools().tools_json({});
        if (!tj.empty() && tj != "[]") {
            req.tools_json = tj;
        }

        auto resp = unwrap_or_throw(os.kernel().infer(req));

        if (!resp.wants_tool_call()) {
            // 最终回答
            rows.push_back({"assistant", resp.content, "", "", ""});
            ctx_win.try_add(kernel::Message::assistant(resp.content));
            break;
        }

        // 有工具调用
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

    // 清理临时 Agent
    if (temp_agent) {
        os.destroy_agent(agent->id());
    }

    // 构建结果表
    size_t n = rows.size();
    auto nint = static_cast<int>(n);
    ConstantSP col_role = Util::createVector(DT_STRING, nint);
    ConstantSP col_content = Util::createVector(DT_STRING, nint);
    ConstantSP col_tool_name = Util::createVector(DT_STRING, nint);
    ConstantSP col_tool_args = Util::createVector(DT_STRING, nint);
    ConstantSP col_tool_result = Util::createVector(DT_STRING, nint);

    for (size_t i = 0; i < n; ++i) {
        auto idx = static_cast<int>(i);
        col_role->setString(idx, rows[i].role);
        col_content->setString(idx, rows[i].content);
        col_tool_name->setString(idx, rows[i].tool_name);
        col_tool_args->setString(idx, rows[i].tool_args);
        col_tool_result->setString(idx, rows[i].tool_result);
    }

    std::vector<std::string> names = {"role", "content", "tool_name", "tool_args", "tool_result"};
    std::vector<ConstantSP> cols = {col_role, col_content, col_tool_name, col_tool_args, col_tool_result};
    return Util::createTable(names, cols);
}

ConstantSP agentOS_remember(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string content = to_string(lhs);
    if (content.empty()) {
        throw RuntimeException("agentOS_remember: content must not be empty");
    }

    float importance = 0.5f;
    if (!rhs.isNull() && rhs->getType() != DT_VOID) {
        importance = to_float(rhs);
        // Clamp to [0, 1]
        if (importance < 0.0f) importance = 0.0f;
        if (importance > 1.0f) importance = 1.0f;
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
        std::move(content), emb, "dolphindb", importance);
    std::string id = unwrap_or_throw(std::move(result));
    return make_string(id);
}

ConstantSP agentOS_recall(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string query = to_string(lhs);
    if (query.empty()) {
        throw RuntimeException("agentOS_recall: query must not be empty");
    }

    int top_k = 5;
    if (!rhs.isNull() && rhs->getType() != DT_VOID) {
        top_k = to_int(rhs);
        if (top_k <= 0) top_k = 5;
    }

    auto& os = PluginRuntime::instance().os();

    // 生成查询 embedding
    kernel::EmbeddingRequest emb_req;
    emb_req.inputs.push_back(query);
    auto emb_res = os.kernel().embed(emb_req);

    memory::Embedding emb;
    if (emb_res && !emb_res->embeddings.empty()) {
        emb = std::move(emb_res->embeddings[0]);
    }

    auto results = unwrap_or_throw(
        os.memory().recall(emb, {}, static_cast<size_t>(top_k)));
    return search_results_to_table(results);
}

ConstantSP agentOS_graphAddNode(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)rhs;
    auto j = to_json(lhs);

    std::string node_id = j.value("id", "");
    if (node_id.empty()) {
        throw RuntimeException("agentOS_graphAddNode: 'id' is required");
    }

    std::string node_type = j.value("type", "entity");

    // 将 properties 序列化为 JSON 存入 content 字段
    std::string content_str;
    if (j.contains("properties") && j["properties"].is_object()) {
        content_str = j["properties"].dump();
    }

    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    memory::GraphNode node;
    node.id = node_id;
    node.type = node_type;
    node.content = std::move(content_str);

    auto result = graph.add_node(std::move(node));
    unwrap_or_throw(std::move(result));

    return make_string(node_id);
}

ConstantSP agentOS_graphAddEdge(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)rhs;
    auto j = to_json(lhs);

    std::string source = j.value("source", "");
    std::string target = j.value("target", "");
    std::string relation = j.value("relation", "related_to");

    if (source.empty() || target.empty()) {
        throw RuntimeException("agentOS_graphAddEdge: 'source' and 'target' are required");
    }

    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    memory::GraphEdge edge;
    edge.source_id = source;
    edge.target_id = target;
    edge.relation = relation;
    edge.weight = j.value("weight", 1.0f);

    auto result = graph.add_edge(std::move(edge));
    unwrap_or_throw(std::move(result));

    return make_string("OK");
}

ConstantSP agentOS_graphQuery(const ConstantSP& lhs, const ConstantSP& rhs) {
    std::string query = to_string(lhs);
    if (query.empty()) {
        throw RuntimeException("agentOS_graphQuery: query must not be empty");
    }

    int max_results = 10;
    if (!rhs.isNull() && rhs->getType() != DT_VOID) {
        max_results = to_int(rhs);
        if (max_results <= 0) max_results = 10;
    }

    auto& os = PluginRuntime::instance().os();
    auto& graph = os.memory().graph();

    // 查询节点的所有边
    // 先尝试直接按节点 ID 查邻接边
    auto edges_result = graph.get_edges(query);
    if (!edges_result.has_value()) {
        // 不是精确节点 ID，尝试 k-hop 子图搜索（k=1）
        auto subgraph_result = graph.k_hop_search(query, 1);
        if (!subgraph_result.has_value() || subgraph_result->edges.empty()) {
            // 返回空表
            auto col_src = Util::createVector(DT_STRING, 0);
            auto col_rel = Util::createVector(DT_STRING, 0);
            auto col_tgt = Util::createVector(DT_STRING, 0);
            auto col_score = Util::createVector(DT_FLOAT, 0);
            std::vector<std::string> names = {"source", "relation", "target", "score"};
            std::vector<ConstantSP> cols = {col_src, col_rel, col_tgt, col_score};
            return Util::createTable(names, cols);
        }
        edges_result = std::move(subgraph_result->edges);
    }

    auto& edges = *edges_result;
    auto n = static_cast<int>(std::min(edges.size(), static_cast<size_t>(max_results)));

    auto col_src = Util::createVector(DT_STRING, n);
    auto col_rel = Util::createVector(DT_STRING, n);
    auto col_tgt = Util::createVector(DT_STRING, n);
    auto col_score = Util::createVector(DT_FLOAT, n);

    for (int i = 0; i < n; ++i) {
        col_src->setString(i, edges[static_cast<size_t>(i)].source_id);
        col_rel->setString(i, edges[static_cast<size_t>(i)].relation);
        col_tgt->setString(i, edges[static_cast<size_t>(i)].target_id);
        col_score->setFloat(i, edges[static_cast<size_t>(i)].weight);
    }

    std::vector<std::string> names = {"source", "relation", "target", "score"};
    std::vector<ConstantSP> cols = {col_src, col_rel, col_tgt, col_score};
    return Util::createTable(names, cols);
}

ConstantSP agentOS_health(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)lhs;
    (void)rhs;

    if (!PluginRuntime::instance().is_initialized()) {
        return make_string("{\"healthy\":false,\"error\":\"not initialized\"}");
    }

    auto& os = PluginRuntime::instance().os();
    auto status = os.health();
    return make_string(status.to_json());
}

ConstantSP agentOS_status(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)lhs;
    (void)rhs;

    if (!PluginRuntime::instance().is_initialized()) {
        return make_string("AgentOS: not initialized");
    }

    auto& os = PluginRuntime::instance().os();
    return make_string(os.status());
}

ConstantSP agentOS_registerTool(const ConstantSP& lhs, const ConstantSP& rhs) {
    auto schema_json = to_json(lhs);
    std::string callback_name = to_string(rhs);

    if (callback_name.empty()) {
        throw RuntimeException("agentOS_registerTool: callback function name is required");
    }

    // 解析 ToolSchema
    tools::ToolSchema schema;
    schema.id = schema_json.value("id", "");
    schema.description = schema_json.value("description", "");

    if (schema.id.empty()) {
        throw RuntimeException("agentOS_registerTool: 'id' is required in schema");
    }

    // 解析参数定义
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

    // 注册工具：handler 通过 DolphinDB 回调函数名执行
    // NOTE: 实际回调需要通过 DolphinDB Heap 执行脚本
    // 这里提供一个占位 handler，实际集成时需要调用 DolphinDB 内部 API
    os.register_tool(std::move(schema),
        [callback_name](const nlohmann::json& args) -> tools::ToolResult {
            // TODO: 通过 DolphinDB Heap::currentHeap() 执行回调
            // 当前为占位实现：返回回调名和参数
            return tools::ToolResult{
                .success = true,
                .output = fmt::format("DolphinDB callback '{}' called with args: {}",
                                      callback_name, args.dump()),
                .error = ""
            };
        });

    return make_string("OK: tool '" + schema_json.value("id", "") + "' registered");
}

ConstantSP agentOS_createAgent(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)rhs;
    auto cfg_json = to_json(lhs);

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

    int handle = PluginRuntime::instance().create_agent(cfg);
    return make_int(handle);
}

ConstantSP agentOS_destroyAgent(const ConstantSP& lhs, const ConstantSP& rhs) {
    (void)rhs;
    int handle = to_int(lhs);
    bool ok = PluginRuntime::instance().destroy_agent(handle);
    return make_bool(ok);
}

} // extern "C"

} // namespace agentos::dolphindb
