#pragma once
// ============================================================
// AgentOS :: DolphinDB Plugin Adapter
// 将 AgentOS 核心能力封装为 DolphinDB 插件函数
//
// DolphinDB 插件约束：
//   1. operator 函数：(Heap*, const ConstantSP&, const ConstantSP&)
//   2. system 函数：  (Heap*, vector<ConstantSP>&)
//   3. command 函数： void(Heap*, vector<ConstantSP>&)
//   4. 所有导出必须 extern "C"
//   5. 同进程模型 — 崩溃即全局崩溃，子线程必须 catch 所有异常
//   6. 后台线程需用 DolphinDB Thread 类，且 copy Session
// ============================================================

#include <CoreConcept.h>   // ConstantSP, TableSP, Heap, Session, etc.
#include <ScalarImp.h>     // String, Int, Float, Bool scalars
#include <Util.h>          // createVector, createTable, createResource, etc.
#include <Exceptions.h>    // IllegalArgumentException

// AgentOS 头文件（通过 cmake -DAGENTOS_NO_DUCKDB=ON 排除 DuckDB 依赖）
#include <agentos/agentos.hpp>
#include <agentos/skills/skill_registry.hpp>

#include <atomic>
#include <thread>
#include <deque>
#include <condition_variable>
#include <random>
#include <chrono>
#include <future>

// SSE 服务：通过 cmake -DAGENTOS_ENABLE_SSE=ON 开启
// 需要 cpp-httplib（header-only）: third_party/httplib/httplib.h
#ifdef AGENTOS_ENABLE_SSE
#include <httplib.h>
#endif

using namespace ddb;
using std::vector;

namespace agentos::dolphindb {

#include "dolphindb_macros.hpp"

// ─────────────────────────────────────────────────────────────
// § D.1  全局单例：持有 AgentOS 实例
// ─────────────────────────────────────────────────────────────

/// 全局 AgentOS 实例管理器（进程级单例）
/// DolphinDB 同进程加载 .so，插件生命周期 = DolphinDB 进程生命周期
class PluginRuntime {
public:
    static PluginRuntime& instance() {
        static PluginRuntime inst;
        return inst;
    }

    /// 初始化 AgentOS。幂等：重复调用返回已有实例。
    bool init(const std::string& config_json);

    /// 关闭 AgentOS，释放所有资源
    void close();

    /// 获取 AgentOS 实例（未初始化时 throw IllegalArgumentException）
    AgentOS& os();

    /// 是否已初始化
    bool is_initialized() const noexcept;

    /// 按 DolphinDB 端 agent handle (long long) 查找 Agent
    std::shared_ptr<Agent> find_agent(long long handle) const;

    /// 创建 Agent，返回 handle
    long long create_agent(const AgentConfig& cfg);

    /// 销毁 Agent
    bool destroy_agent(long long handle);

private:
    PluginRuntime() = default;
    ~PluginRuntime() { close(); }

    PluginRuntime(const PluginRuntime&) = delete;
    PluginRuntime& operator=(const PluginRuntime&) = delete;

    mutable std::mutex mu_;
    std::unique_ptr<AgentOS> os_;
    std::unique_ptr<kernel::ILLMBackend> backend_;

    // Agent handle 映射
    std::unordered_map<long long, std::shared_ptr<Agent>> agents_;
    long long next_handle_{1};
};

// ─────────────────────────────────────────────────────────────
// § D.1b 异步请求管理器 — 支持 web 流式输出
// ─────────────────────────────────────────────────────────────

/// 单个异步请求的状态
struct AsyncRequest {
    enum class Status { Running, Done, Error };

    mutable std::mutex mu;
    Status status{Status::Running};

    // 已生成的全部内容
    std::string content;
    // 上次 poll 以来新增的 delta（poll 后清空）
    std::string delta;
    // 错误信息（status==Error 时有效）
    std::string error;

    std::thread worker;

    /// 追加一个 token（由后台线程调用）
    void append_token(std::string_view token) {
        std::lock_guard lk(mu);
        content += std::string(token);
        delta += std::string(token);
    }

    /// 标记完成
    void mark_done() {
        std::lock_guard lk(mu);
        status = Status::Done;
    }

    /// 标记失败
    void mark_error(const std::string& msg) {
        std::lock_guard lk(mu);
        status = Status::Error;
        error = msg;
    }

    /// poll: 取出 delta，返回 {status, delta, content, error}
    std::tuple<Status, std::string, std::string, std::string> poll() {
        std::lock_guard lk(mu);
        std::string d = std::move(delta);
        delta.clear();
        return {status, std::move(d), content, error};
    }
};

/// 管理所有异步请求（进程级单例）
class AsyncRequestManager {
public:
    static AsyncRequestManager& instance() {
        static AsyncRequestManager inst;
        return inst;
    }

    /// 创建新请求，返回 requestId
    std::string create() {
        std::lock_guard lk(mu_);
        std::string rid = "req_" + std::to_string(next_id_++);
        requests_[rid] = std::make_shared<AsyncRequest>();
        return rid;
    }

    /// 查找请求
    std::shared_ptr<AsyncRequest> find(const std::string& rid) {
        std::lock_guard lk(mu_);
        auto it = requests_.find(rid);
        return (it != requests_.end()) ? it->second : nullptr;
    }

    /// 移除已完成的请求（释放资源）
    void remove(const std::string& rid) {
        std::lock_guard lk(mu_);
        auto it = requests_.find(rid);
        if (it != requests_.end()) {
            // detach worker thread if joinable
            if (it->second->worker.joinable()) {
                it->second->worker.detach();
            }
            requests_.erase(it);
        }
    }

    /// 清理所有已完成的请求
    void cleanup_done() {
        std::lock_guard lk(mu_);
        for (auto it = requests_.begin(); it != requests_.end(); ) {
            std::lock_guard rlk(it->second->mu);
            if (it->second->status != AsyncRequest::Status::Running) {
                if (it->second->worker.joinable()) {
                    it->second->worker.detach();
                }
                it = requests_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    AsyncRequestManager() = default;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<AsyncRequest>> requests_;
    long long next_id_{1};
};

// ─────────────────────────────────────────────────────────────
// § D.1c  SSE 一次性令牌管理器
// ─────────────────────────────────────────────────────────────

/// SSE 连接令牌：绑定 requestId，一次性使用，带过期时间
struct SSEToken {
    std::string requestId;
    std::chrono::steady_clock::time_point expires_at;
    bool consumed{false};
};

/// SSE 令牌管理（进程级单例）
class SSETokenManager {
public:
    static SSETokenManager& instance() {
        static SSETokenManager inst;
        return inst;
    }

    /// 生成一次性 token，绑定 requestId，有效期 ttl_seconds 秒
    std::string generate(const std::string& requestId, int ttl_seconds = 60) {
        std::lock_guard lk(mu_);
        cleanup_expired_();
        std::string token = random_token_();
        tokens_[token] = SSEToken{
            requestId,
            std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds),
            false
        };
        return token;
    }

    /// 验证并消费 token。成功返回 requestId，失败返回空字符串。
    /// 一次性：验证成功后 token 立即失效。
    std::string validate_and_consume(const std::string& token) {
        std::lock_guard lk(mu_);
        auto it = tokens_.find(token);
        if (it == tokens_.end()) return "";

        auto& t = it->second;
        // 已过期或已消费
        if (t.consumed || std::chrono::steady_clock::now() > t.expires_at) {
            tokens_.erase(it);
            return "";
        }
        t.consumed = true;
        std::string rid = t.requestId;
        tokens_.erase(it);
        return rid;
    }

private:
    SSETokenManager() = default;
    std::mutex mu_;
    std::unordered_map<std::string, SSEToken> tokens_;

    /// 生成随机 token（32 字节 hex = 64 字符）
    std::string random_token_() {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, 15);
        const char hex[] = "0123456789abcdef";
        std::string token;
        token.reserve(64);
        for (int i = 0; i < 64; ++i)
            token += hex[dist(rng)];
        return token;
    }

    /// 清理过期 token
    void cleanup_expired_() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = tokens_.begin(); it != tokens_.end(); ) {
            if (it->second.consumed || now > it->second.expires_at)
                it = tokens_.erase(it);
            else
                ++it;
        }
    }
};

// ─────────────────────────────────────────────────────────────
// § D.1d  内嵌 SSE HTTP 服务（可选，需 AGENTOS_ENABLE_SSE）
// ─────────────────────────────────────────────────────────────

#ifdef AGENTOS_ENABLE_SSE

/// 内嵌 SSE 服务器（进程级单例）
/// 在 agentOS::init 时启动，监听独立端口，提供 /sse 端点。
/// 前端通过 EventSource 连接，服务器直接从 AsyncRequest 读取 delta 推送。
class SSEServer {
public:
    static SSEServer& instance() {
        static SSEServer inst;
        return inst;
    }

    /// 启动 SSE 服务。幂等：已启动时直接返回。
    /// @param port 监听端口（默认 8849）
    /// @param cors_origin CORS 允许的源（默认 "*"）
    void start(int port = 8849, const std::string& cors_origin = "*") {
        std::lock_guard lk(mu_);
        if (running_) return;

        port_ = port;
        cors_origin_ = cors_origin;

        server_thread_ = std::thread([this]() {
            httplib::Server svr;

            // CORS preflight
            svr.Options("/sse", [this](const httplib::Request&, httplib::Response& res) {
                set_cors_headers_(res);
                res.status = 204;
            });

            // SSE 端点: GET /sse?rid=xxx&token=yyy
            svr.Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
                set_cors_headers_(res);

                auto token = req.get_param_value("token");
                auto rid_param = req.get_param_value("rid");

                if (token.empty() || rid_param.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"missing rid or token"})", "application/json");
                    return;
                }

                // 验证一次性 token
                auto validated_rid = SSETokenManager::instance().validate_and_consume(token);
                if (validated_rid.empty() || validated_rid != rid_param) {
                    res.status = 403;
                    res.set_content(R"({"error":"invalid or expired token"})", "application/json");
                    return;
                }

                // 查找 AsyncRequest
                auto req_ptr = AsyncRequestManager::instance().find(validated_rid);
                if (!req_ptr) {
                    res.status = 404;
                    res.set_content(R"({"error":"request not found"})", "application/json");
                    return;
                }

                // SSE 流式输出
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [req_ptr](size_t /*offset*/, httplib::DataSink& sink) {
                        auto [status, delta, content, error] = req_ptr->poll();

                        if (!delta.empty()) {
                            // 转义换行符用于 SSE
                            std::string escaped;
                            for (char c : delta) {
                                if (c == '\n') escaped += "\\n";
                                else if (c == '\r') continue;
                                else escaped += c;
                            }
                            std::string event = "data: {\"delta\":\"" + escaped + "\"}\n\n";
                            sink.write(event.data(), event.size());
                        }

                        if (status == AsyncRequest::Status::Done) {
                            std::string done_event = "data: {\"done\":true}\n\n";
                            sink.write(done_event.data(), done_event.size());
                            sink.done();
                            return false;  // 关闭连接
                        }

                        if (status == AsyncRequest::Status::Error) {
                            std::string err_event = "data: {\"error\":\"" + error + "\"}\n\n";
                            sink.write(err_event.data(), err_event.size());
                            sink.done();
                            return false;
                        }

                        // 50ms 间隔，避免空转
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;  // 继续
                    },
                    // Content provider resource releaser (no-op)
                    [](bool) {}
                );
            });

            // ─── RAG HTTP API 端点 ───────────────────────

            // POST /api/kb/search — 检索知识库
            // Body: {"kb_handle": 1, "query": "...", "top_k": 5}
            svr.Post("/api/kb/search", [this](const httplib::Request& req, httplib::Response& res) {
                set_cors_headers_(res);
                try {
                    auto body = nlohmann::json::parse(req.body);
                    long long handle = body.value("kb_handle", 0LL);
                    std::string query = body.value("query", "");
                    size_t top_k = body.value("top_k", 5);
                    int graph_hops = body.value("graph_hops", 0);

                    auto kb = KBManager::instance().find(handle);
                    if (!kb) {
                        res.status = 404;
                        res.set_content(R"({"error":"KB not found"})", "application/json");
                        return;
                    }

                    auto results = kb->search(query, top_k, graph_hops);
                    nlohmann::json j_results = nlohmann::json::array();
                    for (auto& r : results) {
                        j_results.push_back({
                            {"doc_id", r.doc_id}, {"chunk_id", r.chunk_id},
                            {"content", r.content}, {"score", r.score},
                            {"graph_context", r.graph_context}
                        });
                    }
                    res.set_content(j_results.dump(), "application/json");
                } catch (const std::exception& e) {
                    res.status = 400;
                    res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
                }
            });

            // POST /api/kb/ingest — 灌入文档
            // Body: {"kb_handle": 1, "doc_id": "doc1", "text": "..."}
            svr.Post("/api/kb/ingest", [this](const httplib::Request& req, httplib::Response& res) {
                set_cors_headers_(res);
                try {
                    auto body = nlohmann::json::parse(req.body);
                    long long handle = body.value("kb_handle", 0LL);
                    std::string doc_id = body.value("doc_id", "");
                    std::string text = body.value("text", "");

                    auto kb = KBManager::instance().find(handle);
                    if (!kb) {
                        res.status = 404;
                        res.set_content(R"({"error":"KB not found"})", "application/json");
                        return;
                    }

                    auto result = kb->ingest_text(doc_id, text);
                    if (result.has_value()) {
                        res.set_content(nlohmann::json{{"chunks", result.value()}}.dump(), "application/json");
                    } else {
                        res.status = 500;
                        res.set_content(nlohmann::json{{"error", result.error().message}}.dump(), "application/json");
                    }
                } catch (const std::exception& e) {
                    res.status = 400;
                    res.set_content(nlohmann::json{{"error", e.what()}}.dump(), "application/json");
                }
            });

            // GET /api/kb/ask — RAG 流式对话（SSE）
            // Params: kb_handle, q, top_k, token
            svr.Get("/api/kb/ask", [this](const httplib::Request& req, httplib::Response& res) {
                set_cors_headers_(res);

                long long handle = 0;
                try { handle = std::stoll(req.get_param_value("kb_handle")); } catch (...) {}
                std::string question = req.get_param_value("q");
                size_t top_k = 5;
                try { top_k = std::stoul(req.get_param_value("top_k")); } catch (...) {}

                if (question.empty() || handle == 0) {
                    res.status = 400;
                    res.set_content(R"({"error":"missing kb_handle or q"})", "application/json");
                    return;
                }

                auto kb = KBManager::instance().find(handle);
                if (!kb) {
                    res.status = 404;
                    res.set_content(R"({"error":"KB not found"})", "application/json");
                    return;
                }

                // 先检索（同步，通常很快）
                auto search_results = kb->search(question, top_k);

                // 拼接 context
                std::string context;
                for (size_t i = 0; i < search_results.size(); ++i) {
                    context += "[" + std::to_string(i + 1) + "] (" + search_results[i].doc_id + ")\n";
                    context += search_results[i].content + "\n\n";
                }

                // 创建 AsyncRequest 用于流式生成
                auto& mgr = AsyncRequestManager::instance();
                std::string rid = mgr.create();
                auto req_ptr = mgr.find(rid);

                // 后台线程: LLM 流式生成
                std::thread([req_ptr, question, context]() {
                    try {
                        auto& os = PluginRuntime::instance().os();
                        kernel::LLMRequest llm_req;
                        llm_req.messages.push_back(kernel::Message::system(
                            "You are a helpful AI assistant. Answer based on the provided context."));
                        llm_req.messages.push_back(kernel::Message::user(
                            "Context:\n" + context + "\nQuestion: " + question));

                        auto result = os.kernel().stream_infer(llm_req,
                            [&req_ptr](std::string_view token) { req_ptr->append_token(token); });

                        if (result.has_value()) {
                            std::lock_guard lk(req_ptr->mu);
                            if (req_ptr->content.empty() && !result.value().content.empty()) {
                                req_ptr->content = result.value().content;
                                req_ptr->delta = result.value().content;
                            }
                        } else {
                            req_ptr->mark_error(result.error().message);
                            return;
                        }
                        req_ptr->mark_done();
                    } catch (const std::exception& e) {
                        req_ptr->mark_error(std::string("Error: ") + e.what());
                    }
                }).detach();

                // SSE 流式推送
                auto sources_sent = std::make_shared<bool>(false);
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [req_ptr, search_results, sources_sent](size_t, httplib::DataSink& sink) {
                        // 第一次先推送检索来源
                        if (!*sources_sent) {
                            nlohmann::json sources = nlohmann::json::array();
                            for (auto& r : search_results) {
                                sources.push_back({{"doc_id", r.doc_id}, {"score", r.score}});
                            }
                            std::string src_event = "event: sources\ndata: " + sources.dump() + "\n\n";
                            sink.write(src_event.data(), src_event.size());
                            *sources_sent = true;
                        }

                        auto [status, delta, content, error] = req_ptr->poll();
                        if (!delta.empty()) {
                            std::string escaped;
                            for (char c : delta) {
                                if (c == '\n') escaped += "\\n";
                                else if (c == '\r') continue;
                                else escaped += c;
                            }
                            std::string event = "data: {\"delta\":\"" + escaped + "\"}\n\n";
                            sink.write(event.data(), event.size());
                        }
                        if (status == AsyncRequest::Status::Done) {
                            sink.write("data: {\"done\":true}\n\n", 21);
                            sink.done();
                            return false;
                        }
                        if (status == AsyncRequest::Status::Error) {
                            std::string err_ev = "data: {\"error\":\"" + error + "\"}\n\n";
                            sink.write(err_ev.data(), err_ev.size());
                            sink.done();
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;
                    },
                    [](bool) {}
                );
            });

            // CORS preflight for API endpoints
            svr.Options("/api/kb/(.*)", [this](const httplib::Request&, httplib::Response& res) {
                set_cors_headers_(res);
                res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                res.status = 204;
            });

            // 健康检查
            svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({"status":"ok","service":"agentOS-sse"})", "application/json");
            });

            running_ = true;
            svr.listen("0.0.0.0", port_);
            running_ = false;
        });

        server_thread_.detach();
    }

    bool is_running() const { return running_; }
    int port() const { return port_; }

    /// 获取 SSE 基础 URL（不含路径）
    std::string base_url() const {
        return "http://localhost:" + std::to_string(port_);
    }

private:
    SSEServer() = default;
    std::mutex mu_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    int port_{8849};
    std::string cors_origin_{"*"};

    void set_cors_headers_(httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", cors_origin_);
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
    }
};

#endif // AGENTOS_ENABLE_SSE

// ─────────────────────────────────────────────────────────────
// § D.1e  KnowledgeBase 实例管理器
// ─────────────────────────────────────────────────────────────

/// 管理多个 KnowledgeBase 实例（进程级单例）
/// DolphinDB 端通过 handle (long long) 引用知识库
class KBManager {
public:
    static KBManager& instance() {
        static KBManager inst;
        return inst;
    }

    /// 创建知识库，返回 handle
    long long create(std::shared_ptr<knowledge::KnowledgeBase> kb) {
        std::lock_guard lk(mu_);
        long long h = next_handle_++;
        kbs_[h] = std::move(kb);
        return h;
    }

    /// 查找知识库
    std::shared_ptr<knowledge::KnowledgeBase> find(long long handle) {
        std::lock_guard lk(mu_);
        auto it = kbs_.find(handle);
        return (it != kbs_.end()) ? it->second : nullptr;
    }

    /// 销毁知识库
    bool destroy(long long handle) {
        std::lock_guard lk(mu_);
        return kbs_.erase(handle) > 0;
    }

private:
    KBManager() = default;
    std::mutex mu_;
    std::unordered_map<long long, std::shared_ptr<knowledge::KnowledgeBase>> kbs_;
    long long next_handle_{1};
};

// ─────────────────────────────────────────────────────────────
// § D.1f  Skill 注册表（进程级单例）
// ─────────────────────────────────────────────────────────────

/// 进程级 SkillRegistry，被新版 API 使用
inline skills::SkillRegistry& skill_registry() {
    static skills::SkillRegistry reg;
    return reg;
}

/// Agent 级 hook 数据：记录每个 agent 的 blockTools 列表
class AgentHookManager {
public:
    static AgentHookManager& instance() {
        static AgentHookManager inst;
        return inst;
    }

    void set_blocked(long long handle, std::vector<std::string> tools) {
        std::lock_guard lk(mu_);
        blocked_tools_[handle] = std::move(tools);
    }

    std::vector<std::string> get_blocked(long long handle) const {
        std::lock_guard lk(mu_);
        auto it = blocked_tools_.find(handle);
        return it != blocked_tools_.end() ? it->second : std::vector<std::string>{};
    }

    void add_blocked(long long handle, const std::string& tool) {
        std::lock_guard lk(mu_);
        auto& v = blocked_tools_[handle];
        if (std::find(v.begin(), v.end(), tool) == v.end()) {
            v.push_back(tool);
        }
    }

    void remove_blocked(long long handle, const std::string& tool) {
        std::lock_guard lk(mu_);
        auto& v = blocked_tools_[handle];
        v.erase(std::remove(v.begin(), v.end(), tool), v.end());
    }

    void remove_agent(long long handle) {
        std::lock_guard lk(mu_);
        blocked_tools_.erase(handle);
    }

private:
    AgentHookManager() = default;
    mutable std::mutex mu_;
    std::unordered_map<long long, std::vector<std::string>> blocked_tools_;
};

// ─────────────────────────────────────────────────────────────
// § D.2  类型转换工具
// ─────────────────────────────────────────────────────────────

/// 安全提取 string（VOID / null → ""）
std::string extract_string(const ConstantSP& val);

/// 安全提取 int（VOID / null → 0）
int extract_int(const ConstantSP& val);

/// 安全提取 float（VOID / null → 0.0f）
float extract_float(const ConstantSP& val);

/// 安全提取 long long（VOID / null → 0）
long long extract_long(const ConstantSP& val);

/// 字符串转 JSON（空 → {}）
nlohmann::json parse_json(const std::string& s);

/// AgentOS Result<T> → T（错误时 throw IllegalArgumentException）
template<typename T>
T unwrap_or_throw(const std::string& func_name, Result<T>&& result) {
    if (result.has_value()) {
        return std::move(result).value();
    }
    const auto& err = result.error();
    throw IllegalArgumentException(func_name,
        "AgentOS error [" + std::to_string(static_cast<int>(err.code)) +
        "]: " + err.message);
    // This line will never be reached due to the throw above
    __builtin_unreachable();
}

/// void 特化
inline void unwrap_void_or_throw(const std::string& func_name, Result<void>&& result) {
    if (!result.has_value()) {
        const auto& err = result.error();
        throw IllegalArgumentException(func_name,
            "AgentOS error [" + std::to_string(static_cast<int>(err.code)) +
            "]: " + err.message);
    }
}

/// SearchResult 列表 → DolphinDB Table
/// 列: content(STRING), score(DOUBLE), source(STRING), created_at(LONG)
TableSP search_results_to_table(
    const std::vector<memory::SearchResult>& results);

// ─────────────────────────────────────────────────────────────
// § D.3  DolphinDB 导出函数声明
// 全部使用 system 函数签名：(Heap*, vector<ConstantSP>&)
// close 使用 command 签名：void(Heap*, vector<ConstantSP>&)
// ─────────────────────────────────────────────────────────────

extern "C" {

/// 特殊入口 —— loadPlugin 后自动调用
/// args[0] = 模块名 (String), args[1] = libHandle (Long)
ConstantSP initialize(Heap* heap, vector<ConstantSP>& args);

/// 特殊入口 —— 版本查询
ConstantSP version(Heap* heap, vector<ConstantSP>& args);

/// 特殊入口 —— 权限 / 插件信息
ConstantSP pluginInfo(Heap* heap, vector<ConstantSP>& args);

/// agentOS::init([apiKey], [baseUrl], [model], [threads], [tpmLimit])
/// 初始化 AgentOS 运行时
/// @param args: 位置参数，全部可选，无参数 = Mock 模式
/// @return BOOL true
ConstantSP agentOSInit(Heap* heap, vector<ConstantSP>& args);

/// agentOS::close()
/// 关闭 AgentOS，释放所有资源 (command: void 返回)
void agentOSClose(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askTable(question, [prompt], [maxSteps], [agentHandle])
/// 结构化对话，返回含工具调用记录的表
/// @return TABLE — columns: role, content, tool_name, tool_args, tool_result
ConstantSP agentOSAskTable(Heap* heap, vector<ConstantSP>& args);

/// agentOS::remember(content, [importance], [source])
/// 存入长期记忆
/// @return STRING — memory entry ID
ConstantSP agentOSRemember(Heap* heap, vector<ConstantSP>& args);

/// agentOS::recall(query, [topK])
/// 从记忆中检索
/// @return TABLE — columns: content, score, source, created_at
ConstantSP agentOSRecall(Heap* heap, vector<ConstantSP>& args);

/// agentOS::graphAddNode(id, type, content)
/// @return STRING — node ID
ConstantSP agentOSGraphAddNode(Heap* heap, vector<ConstantSP>& args);

/// agentOS::graphAddEdge(source, target, relation, [weight])
/// @return BOOL true
ConstantSP agentOSGraphAddEdge(Heap* heap, vector<ConstantSP>& args);

/// agentOS::graphQuery(nodeId, [maxResults])
/// @return TABLE — columns: source, relation, target, weight
ConstantSP agentOSGraphQuery(Heap* heap, vector<ConstantSP>& args);

/// agentOS::health()
/// @return STRING — JSON health status
ConstantSP agentOSHealth(Heap* heap, vector<ConstantSP>& args);

/// agentOS::status()
/// @return STRING — 系统状态摘要
ConstantSP agentOSStatus(Heap* heap, vector<ConstantSP>& args);

/// agentOS::registerTool(name, description, handler, [params])
/// @return BOOL true
ConstantSP agentOSRegisterTool(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askAsync(question, [systemPrompt])
/// 异步发起 LLM 请求，立即返回 requestId
/// 配合 agentOS::poll(requestId) 实现 web 端流式输出
/// @return STRING — requestId
ConstantSP agentOSAskAsync(Heap* heap, vector<ConstantSP>& args);

/// agentOS::poll(requestId)
/// 轮询异步请求的增量内容
/// @return DICTIONARY — {status, delta, content, done, error}
ConstantSP agentOSPoll(Heap* heap, vector<ConstantSP>& args);

/// agentOS::cancelAsync(requestId)
/// 取消/清理异步请求
/// @return BOOL
ConstantSP agentOSCancelAsync(Heap* heap, vector<ConstantSP>& args);

// ─── RAG: KnowledgeBase 系列函数 ────────────────────────────

/// agentOS::createKB([chunkSize], [chunkOverlap], [embeddingModel])
/// 创建知识库实例
/// @return LONG — KB handle
ConstantSP agentOSCreateKB(Heap* heap, vector<ConstantSP>& args);

/// agentOS::destroyKB(handle)
/// 销毁知识库
/// @return BOOL
ConstantSP agentOSDestroyKB(Heap* heap, vector<ConstantSP>& args);

/// agentOS::saveKB(handle, dirPath)
/// 持久化知识库到磁盘
/// @return BOOL
ConstantSP agentOSSaveKB(Heap* heap, vector<ConstantSP>& args);

/// agentOS::loadKB(handle, dirPath)
/// 从磁盘加载知识库
/// @return BOOL
ConstantSP agentOSLoadKB(Heap* heap, vector<ConstantSP>& args);

/// agentOS::ingest(handle, docId, text)
/// 向知识库灌入文本
/// @return INT — 成功灌入的 chunk 数
ConstantSP agentOSIngest(Heap* heap, vector<ConstantSP>& args);

/// agentOS::ingestDir(handle, dirPath)
/// 批量灌入目录下的 .md/.txt 文件
/// @return BOOL
ConstantSP agentOSIngestDir(Heap* heap, vector<ConstantSP>& args);

/// agentOS::removeDoc(handle, docId)
/// 从知识库中删除文档
/// @return BOOL
ConstantSP agentOSRemoveDoc(Heap* heap, vector<ConstantSP>& args);

/// agentOS::search(handle, query, [topK], [graphHops])
/// 混合检索（BM25 + HNSW + RRF）
/// @return TABLE — columns: doc_id, chunk_id, content, score, graph_context
ConstantSP agentOSSearch(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askWithKB(handle, question, [topK], [systemPrompt])
/// RAG 对话：自动检索知识库 + 拼接上下文 + LLM 生成
/// @return STRING — LLM 回答
ConstantSP agentOSAskWithKB(Heap* heap, vector<ConstantSP>& args);

/// agentOS::kbInfo(handle)
/// 查询知识库状态
/// @return DICTIONARY — {chunk_count, doc_count, embedding_model, chunk_size, chunk_overlap}
ConstantSP agentOSKBInfo(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askWithKBAsync(handle, question, [topK], [systemPrompt])
/// 异步 RAG 对话：检索知识库 + 流式 LLM 生成
/// 返回 dict: {__stream__: true, requestId, status, sseUrl?, token?}
ConstantSP agentOSAskWithKBAsync(Heap* heap, vector<ConstantSP>& args);

// ─── 新版用户中心 API（V2）──────────────────────────────────

/// agentOS::createAgent(name, [prompt], [tools], [skills], [blockTools],
///                      [contextLimit], [isolation], [securityRole])
/// 原生参数风格创建 Agent
/// @return LONG — agent handle
ConstantSP agentOSCreateAgent(Heap* heap, vector<ConstantSP>& args);

/// agentOS::ask(agent, question, [prompt])
/// 单轮/多轮对话（保持上下文）
/// @return STRING — LLM 回答
ConstantSP agentOSAsk(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askStream(agent, question, [prompt], [callback])
/// 流式对话
/// @return STRING — 完整回答
ConstantSP agentOSAskStream(Heap* heap, vector<ConstantSP>& args);

/// agentOS::run(agent, task, [prompt], [timeout], [contextLimit])
/// 结构化执行任务
/// @return DICTIONARY — {success, output, error, durationMs, tokensUsed}
ConstantSP agentOSRun(Heap* heap, vector<ConstantSP>& args);

/// agentOS::save(agent, [metadata])
/// 保存 agent 会话
/// @return STRING — sessionId
ConstantSP agentOSSave(Heap* heap, vector<ConstantSP>& args);

/// agentOS::resume(sessionId)
/// 恢复已保存的会话，返回新 agent handle
/// @return LONG — agent handle
ConstantSP agentOSResume(Heap* heap, vector<ConstantSP>& args);

/// agentOS::destroy(agent)
/// 销毁 agent
/// @return BOOL
ConstantSP agentOSDestroy(Heap* heap, vector<ConstantSP>& args);

/// agentOS::info(agent)
/// 查看 agent 状态
/// @return DICTIONARY — {name, prompt, tools, skills, workDir, ...}
ConstantSP agentOSInfo(Heap* heap, vector<ConstantSP>& args);

/// agentOS::registerSkill(name, keywords, [prompt], [tools])
/// 注册技能
/// @return BOOL
ConstantSP agentOSRegisterSkill(Heap* heap, vector<ConstantSP>& args);

/// agentOS::blockTool(agent, tool, [reason])
/// 运行时禁用工具
/// @return BOOL
ConstantSP agentOSBlockTool(Heap* heap, vector<ConstantSP>& args);

/// agentOS::unblockTool(agent, tool)
/// 运行时启用工具
/// @return BOOL
ConstantSP agentOSUnblockTool(Heap* heap, vector<ConstantSP>& args);

/// agentOS::activateSkill(agent, skillName)
/// 运行时激活技能
/// @return BOOL
ConstantSP agentOSActivateSkill(Heap* heap, vector<ConstantSP>& args);

/// agentOS::deactivateSkill(agent, skillName)
/// 运行时去激活技能
/// @return BOOL
ConstantSP agentOSDeactivateSkill(Heap* heap, vector<ConstantSP>& args);

/// agentOS::sessions()
/// 列出所有已保存的会话
/// @return TABLE — columns: sessionId, agentName, savedAt, messageCount
ConstantSP agentOSSessions(Heap* heap, vector<ConstantSP>& args);

} // extern "C"

} // namespace agentos::dolphindb
