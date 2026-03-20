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
    if (!table->append(cols, insertedRows, errMsg))
        throw RuntimeException("search_results_to_table: append failed: " + errMsg);

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

// ─── agentOS::init([apiKey], [baseUrl], [model], [threads], [tpmLimit]) ──

ConstantSP agentOSInit(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;

    // 兼容旧 JSON 格式：如果第一个参数以 '{' 开头，走旧逻辑
    if (!args.empty() && args[0]->getType() == DT_STRING) {
        std::string first = args[0]->getString();
        if (!first.empty() && first[0] == '{') {
            bool is_new = PluginRuntime::instance().init(first);
            if (is_new) LOG_INFO("AgentOS initialized via DolphinDB plugin.");
#ifdef AGENTOS_ENABLE_SSE
            try {
                auto cfg = nlohmann::json::parse(first);
                int sse_port = cfg.value("sse_port", 8849);
                std::string sse_cors = cfg.value("sse_cors", std::string("*"));
                SSEServer::instance().start(sse_port, sse_cors);
            } catch (...) { SSEServer::instance().start(8849, "*"); }
#endif
            return new Bool(true);
        }
    }

    // 原生参数模式
    std::string api_key   = !args.empty()     ? extract_string(args[0]) : "";
    std::string base_url  = args.size() > 1   ? extract_string(args[1]) : "";
    std::string model     = args.size() > 2   ? extract_string(args[2]) : "";
    int threads           = args.size() > 3   ? extract_int(args[3])    : 4;
    int tpm_limit         = args.size() > 4   ? extract_int(args[4])    : 100000;

    if (threads <= 0) threads = 4;
    if (tpm_limit <= 0) tpm_limit = 100000;

    // 构造 JSON 传给 PluginRuntime（内部仍然用 JSON 初始化）
    nlohmann::json cfg;
    if (!api_key.empty())  cfg["api_key"]  = api_key;
    if (!base_url.empty()) cfg["base_url"] = base_url;
    if (!model.empty())    cfg["model"]    = model;
    cfg["scheduler_threads"] = threads;
    cfg["tpm_limit"]         = tpm_limit;

    bool is_new = PluginRuntime::instance().init(cfg.dump());
    if (is_new) LOG_INFO("AgentOS initialized via DolphinDB plugin.");

#ifdef AGENTOS_ENABLE_SSE
    SSEServer::instance().start(8849, "*");
    LOG_INFO("AgentOS SSE server started on port 8849");
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

static ConstantSP agentOSAsk_v1(Heap* heap, vector<ConstantSP>& args) {
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

static ConstantSP agentOSAskStream_v1(Heap* heap, vector<ConstantSP>& args) {
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

// ─── agentOS::askTable(question, [prompt], [maxSteps], [agentHandle]) ──

ConstantSP agentOSAskTable(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage =
        "Usage: agentOS::askTable(question, [prompt], [maxSteps], [agentHandle])";
    if (args.empty() || !args[0]->isScalar() || args[0]->getType() != DT_STRING)
        throw IllegalArgumentException("agentOS::askTable", usage);

    std::string question = args[0]->getString();
    if (question.empty())
        throw IllegalArgumentException("agentOS::askTable", usage);

  DDB_SAFE_BEGIN
    std::string system_prompt = "You are a helpful AI assistant integrated with DolphinDB.";
    int max_steps = 10;
    long long agent_handle = 0;

    // 兼容旧 JSON 格式: askTable(question, configJson, handle)
    if (args.size() >= 2 && args[1]->getType() == DT_STRING) {
        std::string second = args[1]->getString();
        if (!second.empty() && second[0] == '{') {
            auto opts = parse_json(second);
            system_prompt = opts.value("system_prompt", system_prompt);
            max_steps = opts.value("max_steps", max_steps);
        } else if (!second.empty()) {
            system_prompt = second;  // 原生参数：直接是 prompt 字符串
        }
    }
    if (args.size() >= 3 && args[2]->getType() != DT_VOID) {
        if (args[2]->getType() == DT_INT || args[2]->getType() == DT_LONG) {
            // 第三个参数：可能是 maxSteps（int）或 agentHandle（long）
            long long val = args[2]->getLong();
            if (val <= 100) {
                max_steps = static_cast<int>(val);  // 小数字 → maxSteps
            } else {
                agent_handle = val;  // 大数字 → agentHandle
            }
        }
    }
    if (args.size() >= 4 && args[3]->getType() != DT_VOID) {
        agent_handle = args[3]->getLong();
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
    if (!table->append(cols, insertedRows, errMsg))
        throw RuntimeException("agentOS::askTable: append failed: " + errMsg);

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

// ─── agentOS::graphAddNode(id, [type], [content]) ────────────

ConstantSP agentOSGraphAddNode(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::graphAddNode(id, [type], [content])";
    if (args.empty())
        throw IllegalArgumentException("agentOS::graphAddNode", usage);

    // 兼容旧 JSON 格式
    if (args.size() == 1 && args[0]->getType() == DT_STRING) {
        std::string first = args[0]->getString();
        if (!first.empty() && first[0] == '{') {
            auto j = parse_json(first);
            std::string node_id = j.value("id", "");
            if (node_id.empty())
                throw IllegalArgumentException("agentOS::graphAddNode", "'id' is required");
            std::string node_type = j.value("type", "entity");
            std::string content_str;
            if (j.contains("properties") && j["properties"].is_object())
                content_str = j["properties"].dump();
            else
                content_str = j.value("content", "");

            DDB_SAFE_BEGIN
            auto& graph = PluginRuntime::instance().os().memory().graph();
            memory::GraphNode node;
            node.id = node_id; node.type = node_type; node.content = std::move(content_str);
            unwrap_or_throw("agentOS::graphAddNode", graph.add_node(std::move(node)));
            return new String(node_id);
            DDB_SAFE_END("agentOS::graphAddNode")
        }
    }

    // 原生参数: graphAddNode(id, [type], [content])
    std::string node_id = extract_string(args[0]);
    if (node_id.empty())
        throw IllegalArgumentException("agentOS::graphAddNode", "id must not be empty");

    std::string node_type = args.size() > 1 ? extract_string(args[1]) : "entity";
    std::string content   = args.size() > 2 ? extract_string(args[2]) : "";

    if (node_type.empty()) node_type = "entity";

    DDB_SAFE_BEGIN
    auto& graph = PluginRuntime::instance().os().memory().graph();
    memory::GraphNode node;
    node.id = node_id;
    node.type = node_type;
    node.content = content;

    unwrap_or_throw("agentOS::graphAddNode", graph.add_node(std::move(node)));
    return new String(node_id);
    DDB_SAFE_END("agentOS::graphAddNode")
}

// ─── agentOS::graphAddEdge(source, target, [relation], [weight]) ──

ConstantSP agentOSGraphAddEdge(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::graphAddEdge(source, target, [relation], [weight])";
    if (args.empty())
        throw IllegalArgumentException("agentOS::graphAddEdge", usage);

    // 兼容旧 JSON 格式
    if (args.size() == 1 && args[0]->getType() == DT_STRING) {
        std::string first = args[0]->getString();
        if (!first.empty() && first[0] == '{') {
            auto j = parse_json(first);
            std::string source = j.value("source", "");
            std::string target = j.value("target", "");
            if (source.empty() || target.empty())
                throw IllegalArgumentException("agentOS::graphAddEdge", "source and target required");

            DDB_SAFE_BEGIN
            auto& graph = PluginRuntime::instance().os().memory().graph();
            memory::GraphEdge edge;
            edge.source_id = source; edge.target_id = target;
            edge.relation = j.value("relation", "related_to");
            edge.weight = j.value("weight", 1.0f);
            unwrap_or_throw("agentOS::graphAddEdge", graph.add_edge(std::move(edge)));
            return new Bool(true);
            DDB_SAFE_END("agentOS::graphAddEdge")
        }
    }

    // 原生参数: graphAddEdge(source, target, [relation], [weight])
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::graphAddEdge", usage);

    std::string source   = extract_string(args[0]);
    std::string target   = extract_string(args[1]);
    std::string relation = args.size() > 2 ? extract_string(args[2]) : "related_to";
    float weight         = args.size() > 3 ? extract_float(args[3])  : 1.0f;

    if (source.empty() || target.empty())
        throw IllegalArgumentException("agentOS::graphAddEdge", "source and target must not be empty");
    if (relation.empty()) relation = "related_to";

    DDB_SAFE_BEGIN
    auto& graph = PluginRuntime::instance().os().memory().graph();
    memory::GraphEdge edge;
    edge.source_id = source;
    edge.target_id = target;
    edge.relation = relation;
    edge.weight = weight;

    unwrap_or_throw("agentOS::graphAddEdge", graph.add_edge(std::move(edge)));
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
    if (!table->append(cols, insertedRows, errMsg))
        throw RuntimeException("agentOS::graphQuery: append failed: " + errMsg);

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

// ─── agentOS::registerTool(name, description, handler, [params]) ──

ConstantSP agentOSRegisterTool(Heap* heap, vector<ConstantSP>& args) {
    const string usage = "Usage: agentOS::registerTool(name, description, handler, [params])";
    if (args.size() < 3)
        throw IllegalArgumentException("agentOS::registerTool", usage);

    // 兼容旧 JSON 格式: registerTool(schemaJson, callbackName)
    // 检测：如果只有 2 个参数且第一个以 '{' 开头
    if (args.size() == 2 && args[0]->getType() == DT_STRING) {
        std::string first = args[0]->getString();
        if (!first.empty() && first[0] == '{') {
            // 旧 JSON 模式
            auto schema_json = parse_json(first);
            std::string callback_name = extract_string(args[1]);

            tools::ToolSchema schema;
            schema.id = schema_json.value("id", "");
            schema.description = schema_json.value("description", "");
            if (schema.id.empty())
                throw IllegalArgumentException("agentOS::registerTool", "'id' is required");

            if (schema_json.contains("params") && schema_json["params"].is_array()) {
                for (const auto& p : schema_json["params"]) {
                    tools::ParamDef param;
                    param.name = p.value("name", "");
                    param.description = p.value("description", "");
                    param.required = p.value("required", true);
                    std::string type_str = p.value("type", "string");
                    if (type_str == "int" || type_str == "integer") param.type = tools::ParamType::Integer;
                    else if (type_str == "float" || type_str == "number") param.type = tools::ParamType::Float;
                    else if (type_str == "bool" || type_str == "boolean") param.type = tools::ParamType::Boolean;
                    else param.type = tools::ParamType::String;
                    schema.params.push_back(std::move(param));
                }
            }
            schema.timeout_ms = schema_json.value("timeout_ms", 30000u);

            // 注册（复用下面的回调逻辑）
            DDB_SAFE_BEGIN
            auto& os = PluginRuntime::instance().os();
            SessionSP session = heap->currentSession()->copy();
            session->setUser(heap->currentSession()->getUser());
            os.register_tool(std::move(schema),
                [callback_name, session](const tools::ParsedArgs& pargs) -> tools::ToolResult {
                    try {
                        nlohmann::json args_json;
                        for (const auto& [key, value] : pargs.values) args_json[key] = value;
                        FunctionDefSP func = session->getFunctionDef(callback_name);
                        if (func.isNull()) return tools::ToolResult{false, "", "Function '" + callback_name + "' not found"};
                        vector<ConstantSP> ddb_args = {new String(args_json.dump())};
                        ConstantSP result = func->call(session->getHeap().get(), ddb_args);
                        return tools::ToolResult{true, result->getString(), ""};
                    } catch (const std::exception& e) {
                        return tools::ToolResult{false, "", std::string("Callback error: ") + e.what()};
                    }
                });
            return new Bool(true);
            DDB_SAFE_END("agentOS::registerTool")
        }
    }

    // 新原生参数模式: registerTool(name, description, handler, [params])
    std::string name = extract_string(args[0]);
    std::string description = extract_string(args[1]);

    if (name.empty())
        throw IllegalArgumentException("agentOS::registerTool", "name must not be empty");

    // args[2] 是 DolphinDB 函数引用（FunctionDef）或函数名字符串
    std::string callback_name;
    FunctionDefSP callback_func;
    if (args[2]->getType() == DT_FUNCTIONDEF) {
        callback_func = args[2];
    } else if (args[2]->getType() == DT_STRING) {
        callback_name = args[2]->getString();
    } else {
        throw IllegalArgumentException("agentOS::registerTool", "handler must be a function or function name string");
    }

    tools::ToolSchema schema;
    schema.id = name;
    schema.description = description;

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    SessionSP session = heap->currentSession()->copy();
    session->setUser(heap->currentSession()->getUser());

    os.register_tool(std::move(schema),
        [callback_name, callback_func, session](const tools::ParsedArgs& pargs) -> tools::ToolResult {
            try {
                nlohmann::json args_json;
                for (const auto& [key, value] : pargs.values) args_json[key] = value;

                FunctionDefSP func = callback_func;
                if (func.isNull() && !callback_name.empty()) {
                    func = session->getFunctionDef(callback_name);
                }
                if (func.isNull()) {
                    return tools::ToolResult{false, "", "Handler function not found"};
                }

                vector<ConstantSP> ddb_args = {new String(args_json.dump())};
                ConstantSP result = func->call(session->getHeap().get(), ddb_args);
                return tools::ToolResult{true, result->getString(), ""};
            } catch (const std::exception& e) {
                return tools::ToolResult{false, "", std::string("Callback error: ") + e.what()};
            }
        });

    return new Bool(true);
    DDB_SAFE_END("agentOS::registerTool")
}

// ─── agentOS::createAgent(configJson) ────────────────────────

static ConstantSP agentOSCreateAgent_v1(Heap* heap, vector<ConstantSP>& args) {
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

static ConstantSP agentOSDestroyAgent_v1(Heap* heap, vector<ConstantSP>& args) {
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

// ─── agentOS::createKB([chunkSize], [chunkOverlap], [embeddingModel]) ──

ConstantSP agentOSCreateKB(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    DDB_SAFE_BEGIN

    auto& os = PluginRuntime::instance().os();

    uint32_t vector_dim = 1536;
    size_t max_chunks = 100000;
    std::string embedding_model = "text-embedding-3-small";
    size_t chunk_size = 500;
    size_t chunk_overlap = 50;

    // 兼容旧 JSON 格式
    if (!args.empty() && !args[0]->isNull() && args[0]->getType() == DT_STRING) {
        std::string cfg_str = args[0]->getString();
        if (!cfg_str.empty() && cfg_str[0] == '{') {
            auto cfg = nlohmann::json::parse(cfg_str, nullptr, false);
            if (!cfg.is_discarded()) {
                if (cfg.contains("vector_dim"))      vector_dim      = cfg["vector_dim"].get<uint32_t>();
                if (cfg.contains("max_chunks"))      max_chunks      = cfg["max_chunks"].get<size_t>();
                if (cfg.contains("embedding_model")) embedding_model = cfg["embedding_model"].get<std::string>();
                if (cfg.contains("chunk_size"))      chunk_size      = cfg["chunk_size"].get<size_t>();
                if (cfg.contains("chunk_overlap"))   chunk_overlap   = cfg["chunk_overlap"].get<size_t>();
            }
        } else if (!cfg_str.empty()) {
            // 第一个参数是 embeddingModel 字符串
            embedding_model = cfg_str;
        }
    }

    // 原生参数: createKB([chunkSize], [chunkOverlap], [embeddingModel])
    if (!args.empty() && args[0]->getType() == DT_INT) {
        chunk_size = static_cast<size_t>(extract_int(args[0]));
    }
    if (args.size() > 1 && args[1]->getType() == DT_INT) {
        chunk_overlap = static_cast<size_t>(extract_int(args[1]));
    }
    if (args.size() > 2 && args[2]->getType() == DT_STRING) {
        std::string em = extract_string(args[2]);
        if (!em.empty()) embedding_model = em;
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

// ═════════════════════════════════════════════════════════════
// § V2 API — 用户中心的 Agent 接口
// ═════════════════════════════════════════════════════════════

/// 从 DolphinDB STRING VECTOR 提取 std::vector<std::string>
static std::vector<std::string> extract_string_vector(const ConstantSP& val) {
    std::vector<std::string> result;
    if (!val || val->getType() == DT_VOID || val->isNull()) return result;
    if (val->isScalar() && val->getType() == DT_STRING) {
        result.push_back(val->getString());
        return result;
    }
    if (val->isVector()) {
        int n = val->size();
        for (int i = 0; i < n; ++i) {
            result.push_back(val->getString(i));
        }
    }
    return result;
}

// ─── agentOS::createAgent(name, [prompt], [tools], [skills],
//         [blockTools], [contextLimit], [isolation], [securityRole]) ──

ConstantSP agentOSCreateAgent(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::createAgent(name, [prompt], [tools], [skills], "
                         "[blockTools], [contextLimit], [isolation], [securityRole])";
    if (args.empty()) throw IllegalArgumentException("agentOS::createAgent", usage);

    std::string first_arg = extract_string(args[0]);

    // 兼容 V1: createAgent('{"name":"xxx",...}') — JSON 字符串
    if (!first_arg.empty() && first_arg[0] == '{') {
        return agentOSCreateAgent_v1(heap, args);
    }

    // V2 模式：createAgent(name, [prompt], ...)
    std::string name = first_arg;
    if (name.empty()) throw IllegalArgumentException("agentOS::createAgent", "name must not be empty");

    std::string prompt = args.size() > 1 ? extract_string(args[1]) : "";
    auto tools = args.size() > 2 ? extract_string_vector(args[2]) : std::vector<std::string>{};
    auto skill_names = args.size() > 3 ? extract_string_vector(args[3]) : std::vector<std::string>{};
    auto block_tools = args.size() > 4 ? extract_string_vector(args[4]) : std::vector<std::string>{};
    int ctx_limit = args.size() > 5 ? extract_int(args[5]) : 8192;
    std::string isolation = args.size() > 6 ? extract_string(args[6]) : "thread";
    std::string sec_role = args.size() > 7 ? extract_string(args[7]) : "standard";

    if (ctx_limit <= 0) ctx_limit = 8192;

    DDB_SAFE_BEGIN
    AgentConfig cfg;
    cfg.name = name;
    cfg.role_prompt = prompt.empty()
        ? "You are a helpful AI assistant integrated with DolphinDB."
        : prompt;
    cfg.allowed_tools = tools;
    cfg.context_limit = static_cast<TokenCount>(ctx_limit);
    cfg.security_role = sec_role;
    cfg.isolation = (isolation == "worktree")
        ? worktree::IsolationMode::Worktree
        : worktree::IsolationMode::Thread;

    long long handle = PluginRuntime::instance().create_agent(cfg);

    // Register blockTool hooks
    if (!block_tools.empty()) {
        AgentHookManager::instance().set_blocked(handle, block_tools);

        auto agent = PluginRuntime::instance().find_agent(handle);
        if (agent) {
            agent->use(Middleware{
                .name = "__block_tools__",
                .before = [handle](HookContext& ctx) {
                    if (ctx.operation == "pre_tool_use") {
                        auto blocked = AgentHookManager::instance().get_blocked(handle);
                        for (const auto& t : blocked) {
                            if (ctx.input == t) {
                                ctx.cancelled = true;
                                ctx.cancel_reason = "Tool '" + t + "' is blocked by policy";
                                return;
                            }
                        }
                    }
                },
                .after = nullptr,
            });
        }
    }

    // Activate skills
    for (const auto& skill_name : skill_names) {
        auto agent = PluginRuntime::instance().find_agent(handle);
        if (agent) {
            auto& sr = skill_registry();
            auto prompt_text = sr.get_prompt(skill_name);
            if (!prompt_text.empty()) {
                auto& os = PluginRuntime::instance().os();
                os.ctx().append(agent->id(), kernel::Message::system(prompt_text));
            }
            (void)sr.activate(skill_name, PluginRuntime::instance().os().tools().registry(), agent->id());
        }
    }

    return new Long(handle);
    DDB_SAFE_END("agentOS::createAgent")
}

ConstantSP agentOSCreateAgent2(Heap* heap, vector<ConstantSP>& args) {
    return agentOSCreateAgent(heap, args);
}

// ─── agentOS::ask(agent, question, [prompt]) ─────────────────

ConstantSP agentOSAsk(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::ask(agent, question, [prompt]) or agentOS::ask(question, [prompt])";
    if (args.empty()) throw IllegalArgumentException("agentOS::ask", usage);

    // 兼容 V1: ask(question, [prompt]) — 第一个参数是 string
    // V2: ask(agent, question, [prompt]) — 第一个参数是 long (handle)
    bool is_v1 = (args[0]->getType() == DT_STRING);

    if (is_v1) {
        // V1 模式：无 agent handle，转发到旧的 agentOSAsk
        return agentOSAsk_v1(heap, args);
    }

    // V2 模式：有 agent handle
    if (args.size() < 2) throw IllegalArgumentException("agentOS::ask", usage);

    long long handle = extract_long(args[0]);
    std::string question = extract_string(args[1]);
    std::string prompt = args.size() > 2 ? extract_string(args[2]) : "";

    if (question.empty())
        throw IllegalArgumentException("agentOS::ask", "question must not be empty");

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::ask: invalid agent handle");

    // Inject prompt if provided (one-time)
    if (!prompt.empty()) {
        auto& os = PluginRuntime::instance().os();
        os.ctx().append(agent->id(), kernel::Message::system(prompt));
    }

    auto result = agent->run(question);
    return new String(unwrap_or_throw("agentOS::ask", std::move(result)));
    DDB_SAFE_END("agentOS::ask")
}

ConstantSP agentOSAsk2(Heap* heap, vector<ConstantSP>& args) {
    return agentOSAsk(heap, args);
}

// ─── agentOS::askStream(agent, question, [prompt], [callback]) ──

ConstantSP agentOSAskStream(Heap* heap, vector<ConstantSP>& args) {
    const string usage = "Usage: agentOS::askStream(agent, question, [prompt], [callback]) or agentOS::askStream(question, [prompt], [callback])";
    if (args.empty()) throw IllegalArgumentException("agentOS::askStream", usage);

    // 兼容 V1: askStream(question, [prompt], [callback])
    bool is_v1 = (args[0]->getType() == DT_STRING);
    if (is_v1) {
        return agentOSAskStream_v1(heap, args);
    }

    // V2 模式：有 agent handle
    if (args.size() < 2) throw IllegalArgumentException("agentOS::askStream", usage);

    long long handle = extract_long(args[0]);
    std::string question = extract_string(args[1]);
    std::string prompt = args.size() > 2 ? extract_string(args[2]) : "";

    // Check for callback function
    FunctionDefSP callback;
    if (args.size() > 3 && args[3]->getType() == DT_FUNCTIONDEF) {
        callback = args[3];
    }

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::askStream: invalid agent handle");

    if (!prompt.empty()) {
        auto& os = PluginRuntime::instance().os();
        os.ctx().append(agent->id(), kernel::Message::system(prompt));
    }

    // For streaming, use think() directly with token callback
    auto& os = PluginRuntime::instance().os();
    std::string full_response;

    kernel::ILLMBackend::TokenCallback token_cb = nullptr;
    SessionSP session;
    if (callback) {
        session = heap->currentSession()->copy();
        session->setUser(heap->currentSession()->getUser());
        token_cb = [&callback, &session, &full_response](std::string_view token) {
            full_response += std::string(token);
            vector<ConstantSP> cb_args = {new String(std::string(token))};
            callback->call(session->getHeap().get(), cb_args);
        };
    }

    auto result = std::dynamic_pointer_cast<AgentBase<ReActAgent>>(agent);
    if (result) {
        auto resp = result->think(question, token_cb);
        if (resp && !token_cb) {
            full_response = resp->content;
        }
    } else {
        // Fallback: run() without streaming
        auto resp = agent->run(question);
        if (resp) full_response = resp.value();
    }

    return new String(full_response);
    DDB_SAFE_END("agentOS::askStream")
}

ConstantSP agentOSAskStream2(Heap* heap, vector<ConstantSP>& args) {
    return agentOSAskStream(heap, args);
}

// ─── agentOS::run(agent, task, [prompt], [timeout], [contextLimit]) ──

ConstantSP agentOSRun(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::run(agent, task, [prompt], [timeout], [contextLimit])";
    if (args.size() < 2) throw IllegalArgumentException("agentOS::run", usage);

    long long handle = extract_long(args[0]);
    std::string task = extract_string(args[1]);
    std::string prompt = args.size() > 2 ? extract_string(args[2]) : "";
    int timeout_ms = args.size() > 3 ? extract_int(args[3]) : 60000;

    if (task.empty())
        throw IllegalArgumentException("agentOS::run", "task must not be empty");
    if (timeout_ms <= 0) timeout_ms = 60000;

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::run: invalid agent handle");

    if (!prompt.empty()) {
        auto& os = PluginRuntime::instance().os();
        os.ctx().append(agent->id(), kernel::Message::system(prompt));
    }

    auto start = Clock::now();
    uint64_t tokens_before = PluginRuntime::instance().os().kernel().metrics().total_tokens.load();

    // Run with timeout
    auto start_run = Clock::now();
    auto result = agent->run(task);
    auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start_run);
    uint64_t tokens_after = PluginRuntime::instance().os().kernel().metrics().total_tokens.load();

    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);

    if (result.has_value()) {
        dict->set(new String("success"), new Bool(true));
        dict->set(new String("output"), new String(result.value()));
        dict->set(new String("error"), new String(""));
    } else {
        dict->set(new String("success"), new Bool(false));
        dict->set(new String("output"), new String(""));
        dict->set(new String("error"), new String(result.error().message));
    }

    dict->set(new String("durationMs"), new Long(static_cast<long long>(elapsed.count())));
    dict->set(new String("tokensUsed"), new Long(static_cast<long long>(tokens_after - tokens_before)));

    return dict;
    DDB_SAFE_END("agentOS::run")
}

// ─── agentOS::save(agent, [metadata]) ────────────────────────

ConstantSP agentOSSave(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::save(agent, [metadata])";
    if (args.empty()) throw IllegalArgumentException("agentOS::save", usage);

    long long handle = extract_long(args[0]);
    std::string metadata = args.size() > 1 ? extract_string(args[1]) : "";

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::save: invalid agent handle");

    auto& os = PluginRuntime::instance().os();

    // Collect middleware names
    std::vector<std::string> mw_names; // Middleware names not easily extractable; leave empty

    // Serialize config as JSON
    nlohmann::json cfg_json;
    cfg_json["name"] = agent->config().name;
    cfg_json["role_prompt"] = agent->config().role_prompt;
    cfg_json["context_limit"] = agent->config().context_limit;
    cfg_json["security_role"] = agent->config().security_role;

    auto result = os.ctx().save_session(agent->id(), cfg_json.dump(), mw_names, metadata);
    auto path = unwrap_or_throw("agentOS::save", std::move(result));

    // Extract session ID from path: "session_{agentId}_{sessionId}.bin"
    std::string filename = path.filename().string();
    // Remove "session_" prefix and ".bin" suffix
    std::string prefix = "session_" + std::to_string(agent->id()) + "_";
    if (filename.size() <= prefix.size() + 4)
        throw RuntimeException("agentOS::save: unexpected session filename: " + filename);
    std::string sid = filename.substr(prefix.size(), filename.size() - prefix.size() - 4);

    return new String(sid);
    DDB_SAFE_END("agentOS::save")
}

// ─── agentOS::resume(sessionId) ──────────────────────────────

ConstantSP agentOSResume(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::resume(sessionId)";
    if (args.empty()) throw IllegalArgumentException("agentOS::resume", usage);

    std::string session_id = extract_string(args[0]);
    if (session_id.empty())
        throw IllegalArgumentException("agentOS::resume", "sessionId must not be empty");

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    // Try to find the session by scanning all agent IDs
    // Session ID format: "{agentId}_{timestamp}_{seq}"
    // Extract agent ID from session ID
    auto underscore = session_id.find('_');
    if (underscore == std::string::npos)
        throw RuntimeException("agentOS::resume: invalid sessionId format");

    AgentId original_agent_id = std::stoull(session_id.substr(0, underscore));

    auto state = os.ctx().load_session(original_agent_id, session_id);
    auto session_state = unwrap_or_throw("agentOS::resume", std::move(state));

    // Recreate agent from saved config
    AgentConfig cfg;
    try {
        auto cfg_json = nlohmann::json::parse(session_state.config_json);
        cfg.name = cfg_json.value("name", "resumed_agent");
        cfg.role_prompt = cfg_json.value("role_prompt", "");
        cfg.context_limit = cfg_json.value("context_limit", 8192u);
        cfg.security_role = cfg_json.value("security_role", "standard");
    } catch (...) {
        cfg.name = "resumed_agent";
    }

    long long handle = PluginRuntime::instance().create_agent(cfg);

    // Restore context messages
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (agent) {
        auto& win = os.ctx().get_window(agent->id(), cfg.context_limit);
        win.reset();
        for (const auto& msg : session_state.context.messages) {
            win.try_add(msg);
        }
    }

    return new Long(handle);
    DDB_SAFE_END("agentOS::resume")
}

// ─── agentOS::destroy(agent) ─────────────────────────────────

ConstantSP agentOSDestroy(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::destroy(agent)";
    if (args.empty()) throw IllegalArgumentException("agentOS::destroy", usage);

    long long handle = extract_long(args[0]);

    DDB_SAFE_BEGIN
    AgentHookManager::instance().remove_agent(handle);
    bool ok = PluginRuntime::instance().destroy_agent(handle);
    return new Bool(ok);
    DDB_SAFE_END("agentOS::destroy")
}

// ─── agentOS::info(agent) ────────────────────────────────────

ConstantSP agentOSInfo(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::info(agent)";
    if (args.empty()) throw IllegalArgumentException("agentOS::info", usage);

    long long handle = extract_long(args[0]);

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::info: invalid agent handle");

    auto& os = PluginRuntime::instance().os();
    auto& win = os.ctx().get_window(agent->id(), agent->config().context_limit);

    DictionarySP dict = Util::createDictionary(DT_STRING, nullptr, DT_ANY, nullptr);
    dict->set(new String("name"), new String(agent->config().name));
    dict->set(new String("prompt"), new String(agent->config().role_prompt));
    dict->set(new String("contextLimit"), new Int(static_cast<int>(agent->config().context_limit)));
    dict->set(new String("securityRole"), new String(agent->config().security_role));
    dict->set(new String("messageCount"), new Int(static_cast<int>(win.message_count())));
    dict->set(new String("workDir"), new String(agent->work_dir().string()));
    dict->set(new String("isolation"),
        new String(agent->config().isolation == worktree::IsolationMode::Worktree
                   ? "worktree" : "thread"));

    // Active skills
    auto active = skill_registry().active_skills(agent->id());
    VectorSP skills_vec = Util::createVector(DT_STRING, 0, static_cast<int>(active.size()));
    for (const auto& s : active) {
        const char* skill_name = s.c_str();
        skills_vec->appendString(&skill_name, 1);
    }
    dict->set(new String("skills"), skills_vec);

    // Blocked tools
    auto blocked = AgentHookManager::instance().get_blocked(handle);
    VectorSP blocked_vec = Util::createVector(DT_STRING, 0, static_cast<int>(blocked.size()));
    for (const auto& t : blocked) {
        const char* tool_name = t.c_str();
        blocked_vec->appendString(&tool_name, 1);
    }
    dict->set(new String("blockTools"), blocked_vec);

    return dict;
    DDB_SAFE_END("agentOS::info")
}

// ─── agentOS::registerSkill(name, keywords, [prompt], [tools]) ──

ConstantSP agentOSRegisterSkill(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::registerSkill(name, keywords, [prompt], [tools])";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::registerSkill", usage);

    std::string name = extract_string(args[0]);
    auto keywords = extract_string_vector(args[1]);
    std::string prompt = args.size() > 2 ? extract_string(args[2]) : "";
    auto tool_names = args.size() > 3 ? extract_string_vector(args[3]) : std::vector<std::string>{};

    if (name.empty())
        throw IllegalArgumentException("agentOS::registerSkill", "name must not be empty");
    if (keywords.empty())
        throw IllegalArgumentException("agentOS::registerSkill", "keywords must not be empty");

    DDB_SAFE_BEGIN
    skills::SkillDef skill;
    skill.name = name;
    skill.description = prompt.empty() ? ("Skill: " + name) : prompt;
    skill.keywords = keywords;
    skill.prompt_injection = prompt;

    // Link existing registered tools by name
    auto& os = PluginRuntime::instance().os();
    for (const auto& tn : tool_names) {
        auto tool = os.tools().registry().find(tn);
        if (tool) {
            skill.tools.push_back(tool->schema());
            // Wrap the tool's execute as a ToolFn
            auto tool_ptr = tool;
            skill.tool_fns.push_back(
                [tool_ptr](const tools::ParsedArgs& pa, std::stop_token st) {
                    return tool_ptr->execute(pa, st);
                });
        }
    }

    skill_registry().register_skill(std::move(skill));
    return new Bool(true);
    DDB_SAFE_END("agentOS::registerSkill")
}

// ─── agentOS::blockTool(agent, tool, [reason]) ───────────────

ConstantSP agentOSBlockTool(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::blockTool(agent, tool, [reason])";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::blockTool", usage);

    long long handle = extract_long(args[0]);
    std::string tool = extract_string(args[1]);

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::blockTool: invalid agent handle");

    AgentHookManager::instance().add_blocked(handle, tool);
    return new Bool(true);
    DDB_SAFE_END("agentOS::blockTool")
}

// ─── agentOS::unblockTool(agent, tool) ───────────────────────

ConstantSP agentOSUnblockTool(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::unblockTool(agent, tool)";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::unblockTool", usage);

    long long handle = extract_long(args[0]);
    std::string tool = extract_string(args[1]);

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::unblockTool: invalid agent handle");

    AgentHookManager::instance().remove_blocked(handle, tool);
    return new Bool(true);
    DDB_SAFE_END("agentOS::unblockTool")
}

// ─── agentOS::activateSkill(agent, skillName) ────────────────

ConstantSP agentOSActivateSkill(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::activateSkill(agent, skillName)";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::activateSkill", usage);

    long long handle = extract_long(args[0]);
    std::string skill_name = extract_string(args[1]);

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::activateSkill: invalid agent handle");

    auto& os = PluginRuntime::instance().os();
    auto& sr = skill_registry();

    // Inject prompt
    auto prompt = sr.get_prompt(skill_name);
    if (!prompt.empty()) {
        os.ctx().append(agent->id(), kernel::Message::system(prompt));
    }

    unwrap_void_or_throw("agentOS::activateSkill",
        sr.activate(skill_name, os.tools().registry(), agent->id()));

    return new Bool(true);
    DDB_SAFE_END("agentOS::activateSkill")
}

// ─── agentOS::deactivateSkill(agent, skillName) ──────────────

ConstantSP agentOSDeactivateSkill(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    const string usage = "Usage: agentOS::deactivateSkill(agent, skillName)";
    if (args.size() < 2)
        throw IllegalArgumentException("agentOS::deactivateSkill", usage);

    long long handle = extract_long(args[0]);
    std::string skill_name = extract_string(args[1]);

    DDB_SAFE_BEGIN
    auto agent = PluginRuntime::instance().find_agent(handle);
    if (!agent) throw RuntimeException("agentOS::deactivateSkill: invalid agent handle");

    auto& os = PluginRuntime::instance().os();
    unwrap_void_or_throw("agentOS::deactivateSkill",
        skill_registry().deactivate(skill_name, os.tools().registry(), agent->id()));

    return new Bool(true);
    DDB_SAFE_END("agentOS::deactivateSkill")
}

// ─── agentOS::sessions() ────────────────────────────────────

ConstantSP agentOSSessions(Heap* heap, vector<ConstantSP>& args) {
    (void)heap;
    (void)args;

    DDB_SAFE_BEGIN
    auto& os = PluginRuntime::instance().os();

    // Scan snapshot directory for session files
    std::vector<std::string> session_ids;
    std::vector<std::string> agent_names;
    std::vector<long long> message_counts;

    auto& ctx = os.ctx();
    // Scan snapshot dir for all session files matching "session_*.bin"
    std::string snap_dir = os.config().snapshot_dir;
    namespace fs = std::filesystem;

    if (fs::exists(snap_dir)) {
        for (const auto& entry : fs::directory_iterator(snap_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string fname = entry.path().filename().string();
            if (!fname.starts_with("session_") || !fname.ends_with(".bin")) continue;

            // Parse: session_{agentId}_{sessionId}.bin
            if (fname.size() <= 12) continue;
            std::string body = fname.substr(8, fname.size() - 12); // remove "session_" and ".bin"
            auto first_us = body.find('_');
            if (first_us == std::string::npos) continue;

            AgentId aid = std::stoull(body.substr(0, first_us));
            std::string sid = body.substr(first_us + 1);

            // Load to get details
            auto state = ctx.load_session(aid, sid);
            if (state) {
                session_ids.push_back(sid);
                try {
                    auto cfg = nlohmann::json::parse(state->config_json);
                    agent_names.push_back(cfg.value("name", "unknown"));
                } catch (...) {
                    agent_names.push_back("unknown");
                }
                message_counts.push_back(static_cast<long long>(state->context.messages.size()));
            }
        }
    }

    // Build table
    int n = static_cast<int>(session_ids.size());
    VectorSP col_sid = Util::createVector(DT_STRING, 0, n);
    VectorSP col_name = Util::createVector(DT_STRING, 0, n);
    VectorSP col_msgs = Util::createVector(DT_LONG, 0, n);

    for (int i = 0; i < n; ++i) {
        const char* sid = session_ids[i].c_str();
        const char* name = agent_names[i].c_str();
        col_sid->appendString(&sid, 1);
        col_name->appendString(&name, 1);
        col_msgs->appendLong(&message_counts[i], 1);
    }

    vector<string> col_names = {"sessionId", "agentName", "messageCount"};
    vector<ConstantSP> cols = {col_sid, col_name, col_msgs};
    return Util::createTable(col_names, cols);
    DDB_SAFE_END("agentOS::sessions")
}

} // extern "C"

} // namespace agentos::dolphindb
