#pragma once
// ============================================================
// AgentOS :: DolphinDB Plugin Adapter
// 将 AgentOS 核心能力封装为 DolphinDB 插件函数
//
// DolphinDB 插件约束：
//   1. operator 函数：最多 2 个参数（ConstantSP lhs, ConstantSP rhs）
//   2. system 函数：Heap* + vector<ConstantSP>
//   3. 所有函数在 DolphinDB 工作线程上调用
//   4. 仅插件线程可以 throw（不得在回调中抛出）
//   5. 同进程模型 — 崩溃即全局崩溃
// ============================================================

#include <CoreConcept.h>      // DolphinDB SDK: ConstantSP, TableSP, etc.
#include <ScalarImp.h>        // String, Int, Float, Bool scalars
#include <Util.h>             // createString, createTable, etc.

#include <agentos/agent.hpp>  // 完整 AgentOS API
#include <agentos/agentos.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agentos::dolphindb {

// ─────────────────────────────────────────────────────────────
// § D.1  全局单例：持有 AgentOS 实例
// ─────────────────────────────────────────────────────────────

/// 全局 AgentOS 实例管理器（进程级单例）
/// DolphinDB 同进程加载 .so，插件生命周期 = DolphinDB 进程生命周期
/// 用 shared_ptr + mutex 保证线程安全初始化/销毁
class PluginRuntime {
public:
    static PluginRuntime& instance() {
        static PluginRuntime inst;
        return inst;
    }

    /// 初始化 AgentOS。幂等：重复调用返回已有实例。
    /// @param config_json JSON 格式的配置
    /// @return true if newly initialized, false if already running
    bool init(const std::string& config_json);

    /// 关闭 AgentOS，释放所有资源
    void close();

    /// 获取 AgentOS 实例（未初始化时 throw）
    AgentOS& os();

    /// 是否已初始化
    bool is_initialized() const noexcept;

    /// 按 DolphinDB 端 agent handle (int) 查找 Agent
    std::shared_ptr<Agent> find_agent(int handle) const;

    /// 创建 Agent，返回 handle
    int create_agent(const AgentConfig& cfg);

    /// 销毁 Agent
    bool destroy_agent(int handle);

private:
    PluginRuntime() = default;
    ~PluginRuntime() { close(); }

    PluginRuntime(const PluginRuntime&) = delete;
    PluginRuntime& operator=(const PluginRuntime&) = delete;

    mutable std::mutex mu_;
    std::unique_ptr<AgentOS> os_;
    std::unique_ptr<kernel::ILLMBackend> backend_;

    // Agent handle 映射（DolphinDB 端用 int handle 引用 Agent）
    std::unordered_map<int, std::shared_ptr<Agent>> agents_;
    int next_handle_{1};
};

// ─────────────────────────────────────────────────────────────
// § D.2  类型转换工具
// ─────────────────────────────────────────────────────────────

/// 从 ConstantSP 提取 string（支持 STRING / CHAR[]）
std::string to_string(const ConstantSP& val);

/// 从 ConstantSP 提取 int
int to_int(const ConstantSP& val);

/// 从 ConstantSP 提取 float
float to_float(const ConstantSP& val);

/// 从 ConstantSP 提取 JSON 对象
nlohmann::json to_json(const ConstantSP& val);

/// AgentOS Result<T> → ConstantSP（错误时 throw DolphinDB 异常）
template<typename T>
T unwrap_or_throw(Result<T>&& result) {
    if (result.has_value()) {
        return std::move(result).value();
    }
    throw RuntimeException(
        "AgentOS error [" + std::to_string(static_cast<int>(result.error().code)) +
        "]: " + result.error().message);
}

/// void 特化
inline void unwrap_or_throw(Result<void>&& result) {
    if (!result.has_value()) {
        throw RuntimeException(
            "AgentOS error [" + std::to_string(static_cast<int>(result.error().code)) +
            "]: " + result.error().message);
    }
}

/// 创建 DolphinDB 字符串标量
ConstantSP make_string(const std::string& s);

/// 创建 DolphinDB int 标量
ConstantSP make_int(int v);

/// 创建 DolphinDB bool 标量
ConstantSP make_bool(bool v);

/// 创建 DolphinDB float 标量
ConstantSP make_float(float v);

/// SearchResult 列表 → DolphinDB Table
/// 列: content(STRING), score(FLOAT), source(STRING), timestamp(LONG)
TableSP search_results_to_table(
    const std::vector<memory::SearchResult>& results);

/// LLMResponse → DolphinDB Dict
/// keys: content, finish_reason, prompt_tokens, completion_tokens, tool_calls_json
ConstantSP llm_response_to_dict(const kernel::LLMResponse& resp);

// ─────────────────────────────────────────────────────────────
// § D.3  DolphinDB 导出函数声明
// 所有 extern "C" 函数遵循 DolphinDB operator 签名：
//   ConstantSP func(const ConstantSP& lhs, const ConstantSP& rhs)
// lhs / rhs 可为 Void 表示未提供
// ─────────────────────────────────────────────────────────────

extern "C" {

/// agentOS_init(configJson)
/// 初始化 AgentOS 运行时
/// @param lhs: STRING — JSON 配置，包含:
///   - api_key: LLM API key
///   - base_url: API endpoint (可选, 默认 OpenAI)
///   - model: 模型名 (可选, 默认 gpt-4o-mini)
///   - scheduler_threads: 调度器线程数 (可选, 默认 4)
///   - tpm_limit: token/min 限流 (可选, 默认 100000)
/// @param rhs: VOID (未使用)
/// @return STRING "OK" or throws
ConstantSP agentOS_init(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_close()
/// 关闭 AgentOS，释放所有资源
/// @return STRING "OK"
ConstantSP agentOS_close(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_ask(question)
/// 单轮对话：创建临时 ReActAgent，执行 question，返回回答
/// @param lhs: STRING — 用户问题
/// @param rhs: VOID 或 STRING — 可选的 system prompt
/// @return STRING — LLM 回答
ConstantSP agentOS_ask(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_askStream(question)
/// 流式对话：逐 token 打印到 DolphinDB 控制台
/// @param lhs: STRING — 用户问题
/// @param rhs: VOID 或 STRING — 可选的 system prompt
/// @return STRING — 完整回答
ConstantSP agentOS_askStream(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_askTable(question)
/// 对话并返回结构化结果（含 tool_calls 等元数据）
/// @param lhs: STRING — 用户问题
/// @param rhs: VOID 或 STRING — JSON config: {system_prompt, max_steps, agent_handle}
/// @return TABLE — columns: role, content, tool_name, tool_args, tool_result
ConstantSP agentOS_askTable(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_remember(content, importance)
/// 将内容存入长期记忆
/// @param lhs: STRING — 要记忆的内容
/// @param rhs: FLOAT — 重要度 [0.0, 1.0]，默认 0.5
/// @return STRING — memory entry ID
ConstantSP agentOS_remember(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_recall(query, topK)
/// 从记忆中检索相关内容
/// @param lhs: STRING — 查询文本
/// @param rhs: INT — 返回 top-K 条结果，默认 5
/// @return TABLE — columns: content, score, source, timestamp
ConstantSP agentOS_recall(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_graphAddNode(nodeJson)
/// 向知识图谱添加节点
/// @param lhs: STRING — JSON: {id, type, properties: {...}}
///   properties 将被序列化为 JSON 存入 content 字段
/// @param rhs: VOID
/// @return STRING — node ID
ConstantSP agentOS_graphAddNode(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_graphAddEdge(edgeJson)
/// 向知识图谱添加边
/// @param lhs: STRING — JSON: {source, target, relation, properties: {...}}
/// @param rhs: VOID
/// @return STRING — "OK"
ConstantSP agentOS_graphAddEdge(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_graphQuery(query)
/// 图谱查询
/// @param lhs: STRING — 查询文本或 JSON query spec
/// @param rhs: INT — max results, 默认 10
/// @return TABLE — columns: source, relation, target, score
ConstantSP agentOS_graphQuery(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_health()
/// 返回健康检查结果
/// @return STRING — JSON health status
ConstantSP agentOS_health(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_status()
/// 返回系统运行状态摘要
/// @return STRING — status string
ConstantSP agentOS_status(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_registerTool(schemaJson)
/// 注册自定义工具（DolphinDB 函数作为工具）
/// @param lhs: STRING — JSON schema: {id, description, params: [...]}
/// @param rhs: STRING — DolphinDB 回调函数名（在 DolphinDB 端定义的函数）
/// @return STRING — "OK"
ConstantSP agentOS_registerTool(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_createAgent(configJson)
/// 创建持久 Agent（可多轮对话）
/// @param lhs: STRING — JSON config: {name, role_prompt, context_limit, ...}
/// @param rhs: VOID
/// @return INT — agent handle
ConstantSP agentOS_createAgent(const ConstantSP& lhs, const ConstantSP& rhs);

/// agentOS_destroyAgent(handle)
/// 销毁 Agent
/// @param lhs: INT — agent handle
/// @param rhs: VOID
/// @return BOOL — success
ConstantSP agentOS_destroyAgent(const ConstantSP& lhs, const ConstantSP& rhs);

} // extern "C"

} // namespace agentos::dolphindb
