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

#include <atomic>
#include <thread>
#include <deque>
#include <condition_variable>

using namespace ddb;
using std::vector;

namespace agentos::dolphindb {

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

/// agentOS::init([configJson])
/// 初始化 AgentOS 运行时
/// @param args[0]: STRING — JSON 配置（可选），默认空 = Mock 模式
/// @return BOOL true
ConstantSP agentOSInit(Heap* heap, vector<ConstantSP>& args);

/// agentOS::close()
/// 关闭 AgentOS，释放所有资源 (command: void 返回)
void agentOSClose(Heap* heap, vector<ConstantSP>& args);

/// agentOS::ask(question, [systemPrompt])
/// 单轮对话
/// @return STRING — LLM 回答
ConstantSP agentOSAsk(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askStream(question, [systemPrompt], [callbackFunc])
/// 流式对话 — callbackFunc 每收到一个 token 被调用一次
/// 例: agentOS::askStream("问题", , print)
/// @return STRING — 完整回答
ConstantSP agentOSAskStream(Heap* heap, vector<ConstantSP>& args);

/// agentOS::askTable(question, [configJson], [agentHandle])
/// 对话并返回结构化结果
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

/// agentOS::graphAddNode(nodeJson)
/// @return STRING — node ID
ConstantSP agentOSGraphAddNode(Heap* heap, vector<ConstantSP>& args);

/// agentOS::graphAddEdge(edgeJson)
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

/// agentOS::registerTool(schemaJson, callbackFuncName)
/// @return BOOL true
ConstantSP agentOSRegisterTool(Heap* heap, vector<ConstantSP>& args);

/// agentOS::createAgent(configJson)
/// @return LONG — agent handle（可作为 resource 管理）
ConstantSP agentOSCreateAgent(Heap* heap, vector<ConstantSP>& args);

/// agentOS::destroyAgent(handle)
/// @return BOOL — success
ConstantSP agentOSDestroyAgent(Heap* heap, vector<ConstantSP>& args);

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

} // extern "C"

} // namespace agentos::dolphindb
