# AgentOS — C++23 LLM Agent Operating System

> **LLM as Kernel** — 以大语言模型为内核的轻量级 Agent 操作系统
> A lightweight, modular Agent OS written in modern C++23, inspired by AIOS, MemGPT, Autellix, and IsolateGPT.

---

## 概览 / Overview

AgentOS 将 LLM 视为 CPU、上下文窗口视为 RAM、工具调用视为系统调用，在此抽象之上构建了一套完整的多 Agent 运行时：

```
┌─────────────────────────────────────────────────────────────┐
│                        AgentOS Facade                        │
├──────────┬──────────┬──────────┬──────────┬────────┬────────┤
│  Kernel  │Scheduler │ Context  │  Memory  │ Tools  │Security│
│  (LLM)   │  (DAG)   │ Manager  │ (3-tier) │Manager │ (ECL)  │
├──────────┴──────────┴──────────┴──────────┴────────┴────────┤
│                      Agent Bus (Hub)                         │
└─────────────────────────────────────────────────────────────┘
         ↕               ↕               ↕
    ReActAgent      CustomAgent     SubAgent...
```

| 概念     | 类比        |
|----------|-------------|
| LLM      | CPU         |
| Context  | RAM (虚拟内存) |
| Tools    | 系统调用    |
| Agent    | 进程        |
| Scheduler| 进程调度器  |
| Memory   | 存储层次    |
| Security | 内核保护环  |

---

## 架构模块 / Architecture

### Module 1 — LLM Kernel (`kernel/llm_kernel.hpp`)

LLM 推理内核，提供速率限制、后端抽象和指标采集。

- **TokenBucketRateLimiter** — 令牌桶限流（TPM quota）
- **ILLMBackend** — 可插拔后端接口
  - `MockLLMBackend` — 基于规则的本地 Mock，用于测试
  - `OpenAIBackend` — 调用 OpenAI / compatible API
- **LLMKernel** — 统一门面，含 `KernelMetrics`（请求数、token 数、延迟）

```cpp
auto backend = std::make_unique<kernel::MockLLMBackend>();
kernel::LLMKernel kern(std::move(backend), /*tpm_limit=*/10000);

kernel::LLMRequest req;
req.messages = { kernel::Message::user("Hello!") };
auto resp = kern.infer(req);
// resp->content, resp->tool_calls
```

### Module 2 — Scheduler (`scheduler/scheduler.hpp`)

优先级调度器 + 依赖 DAG，支持多 Agent 并发任务编排。

- **DependencyGraph** — 有向无环图，DFS 环检测，`boost_critical_path()` 防止优先级反转
- **Scheduler** — 优先队列 + 线程池，`jthread` dispatcher 定期死锁探测
- 支持 `wait_for(task_id)` 同步等待

```cpp
scheduler::Scheduler sched(scheduler::SchedulerPolicy::Priority, /*threads=*/4);
sched.start();

auto t1 = scheduler::Scheduler::new_task_id();
auto task = std::make_shared<scheduler::AgentTaskDescriptor>();
task->id   = t1;
task->work = [] { /* ... */ };
task->priority = Priority::High;
sched.submit(task);
sched.wait_for(t1);
```

### Module 3 — Context Manager (`context/context.hpp`)

上下文窗口管理，内置 LRU 驱逐和快照持久化，灵感来自 MemGPT。

- **ContextWindow** — Token 预算管理，超出时 LRU 驱逐非 system 消息
- **ContextSnapshot** — 序列化/反序列化上下文至磁盘（文本格式）
- **ContextManager** — per-agent 窗口映射，`snapshot()` / `restore()` / `compress()`

```cpp
context::ContextManager ctx("/tmp/snapshots");
auto& win = ctx.get_window(agent_id, /*token_limit=*/4096);
win.try_add(kernel::Message::user("问题"));
ctx.snapshot(agent_id);   // 持久化
ctx.restore(agent_id);    // 恢复
```

### Module 4 — Memory System (`memory/memory.hpp`)

三层记忆体系，模拟人类认知模型。

| 层次 | 类          | 容量  | 驱逐策略              |
|------|-------------|-------|----------------------|
| L0   | WorkingMemory  | 32   | LRU by `accessed_at` |
| L1   | ShortTermMemory| 512  | 重要性/访问频率       |
| L2   | LongTermMemory | 无限  | 文件持久化（importance > 0.7 写入）|

- 朴素 N-gram 嵌入（64 维）+ 余弦相似度检索
- `remember()` — 写入全部层（L2 按重要性门控）
- `recall(query, k)` — 合并三层结果，去重排序

```cpp
memory::MemorySystem mem("/tmp/ltm");
mem.remember("用户偏好暗色主题", "agent_1", /*importance=*/0.8f);
auto results = mem.recall("界面偏好", /*k=*/5);
```

### Module 5 — Tool Manager (`tools/tool_manager.hpp`)

工具注册、发现与沙箱隔离执行。

- **ToolSchema** — 元数据（名称/描述/参数/危险标记），生成 OpenAI function-calling JSON
- **ITool** / **ToolResult** — 工具接口
- **内置工具**：
  - `kv_store` — 进程内键值存储
  - `shell_exec` — 白名单 Shell 命令（echo/date/pwd/ls 等）
  - `http_fetch` — 受限 HTTP GET（10s 超时，100KB 限制）
- **ToolRegistry** — 底层注册表，支持 `register_fn()` 注册 lambda 工具
- **ToolManager** — 默认入口，负责内置工具注册、参数校验、超时与调度

```cpp
tools::ToolManager tool_mgr;

// 注册自定义工具
tool_mgr.registry().register_fn(
    ToolSchema{.id="my_tool", .description="...", .params={...}},
    [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok("result");
    });

// 执行工具调用
auto result = tool_mgr.dispatch(kernel::ToolCallRequest{
    .name = "kv_store",
    .args_json = R"({"op":"set","key":"x","value":"42"})"
});
```

### Module 6 — Security Layer (`security/security.hpp`)

多层纵深防御：RBAC + 污点追踪 + 注入检测 + ECL。

#### § 6.1 RBAC — 基于角色的访问控制

| 角色       | 权限                                          |
|------------|-----------------------------------------------|
| readonly   | ToolReadOnly, MemoryRead, AgentObserve        |
| standard   | + ToolWrite, MemoryWrite, AgentCreate         |
| privileged | + ToolDangerous, MemoryDelete, AgentKill      |
| admin      | 全部权限 (0xFFFFFFFF)                          |

#### § 6.2 TaintTracker — 数据流污点追踪

信任级别：`Trusted < UserInput < External < Untrusted`

外部数据（HTTP 响应、文件）自动标记为 `External`，禁止流入 `shell_exec`、`code_exec`、`send_email` 等敏感工具。

#### § 6.3 InjectionDetector — 注入检测

内置 14 条英/中 pattern（"ignore previous instructions"、"忽略之前的指令" 等）+ 启发式指令频率检测。

#### § 6.4 ECL — 执行控制层

每次工具调用前按顺序执行：RBAC → 污点检查 → 注入检测 → 人工审批（可选）

```cpp
security::SecurityManager sec([](AgentId id, std::string_view tool, std::string_view args) {
    // 返回 true 批准，false 拒绝高危操作
    return true;
});
sec.grant(agent_id, "standard");
auto ok = sec.authorize_tool(agent_id, "shell_exec", args_json);
```

### Agent Bus (`bus/agent_bus.hpp`)

Hub-and-Spoke 消息总线，支持点对点、发布订阅和同步 RPC。

- **Channel** — per-agent 消息队列，`recv(timeout)` 阻塞接收
- **AgentBus**：
  - `send()` — 点对点路由（自动注入扫描 + 敏感词 redact）
  - `publish()` — 主题广播
  - `call()` — 同步 RPC（带超时）
  - `add_monitor()` — 旁路监听，用于可观测性
  - 完整 `audit_trail_` 记录

---

## Agent 执行模型

### ReAct 循环

```
用户输入
    │
    ▼
┌─────────┐    has_tool_call     ┌──────────┐
│  Think  │ ──────────────────► │   Act    │
│ (LLM)   │                     │ (Tools)  │
└─────────┘ ◄────────────────── └──────────┘
    │           Observe                │
    │         (append result)          │
    │                                  │
    ▼ (no tool call)
 返回结果 → 写入 Memory
```

最多执行 `MAX_STEPS = 10` 轮，防止无限循环。

### 自定义 Agent

```cpp
class MyAgent : public agentos::Agent {
public:
    MyAgent(AgentId id, AgentConfig cfg, AgentOS* os)
        : Agent(id, std::move(cfg), os) {}

    Result<std::string> run(std::string input) override {
        auto resp = think(input);                  // LLM 推理
        if (!resp) return make_unexpected(resp.error());

        for (auto& tc : resp->tool_calls) {
            auto result = act(tc);                 // 工具执行（含安全检查）
        }

        remember("处理了: " + input, 0.7f);         // 写入记忆
        return resp->content;
    }
};
```

---

## 快速开始 / Quick Start

### 依赖

| 工具    | 版本要求     |
|---------|-------------|
| GCC     | ≥ 11（推荐 ≥ 13） |
| CMake   | ≥ 3.25       |
| libcurl | 构建必需 |
| sqlite3 | 构建必需 |

可选依赖：

- `DuckDB`：默认启用；如未安装，可通过 `-DAGENTOS_NO_DUCKDB=ON` 关闭
- `cppjieba`：默认启用，用于中文分词支持
- DolphinDB SDK：仅构建插件时需要

> **注意**：项目当前以 `C++23` 编译，并通过 `core/compat.hpp` 补齐部分库能力；文档中的旧 `C++20` 表述已不再准确。

### 编译

```bash
git clone <repo-url> cpp-agent-os
cd cpp-agent-os

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 运行 Demo

```bash
./build/agentos_demo
```

Demo 涵盖全部 8 个功能区域的演示：LLM Kernel、Scheduler、Context Manager、Memory System、Tool Manager、Security Layer、Agent Bus 和完整系统集成。

### 接入 OpenAI API

```cpp
#include <agentos/agentos.hpp>

auto os = agentos::AgentOSBuilder()
    .openai(std::getenv("OPENAI_API_KEY"), "gpt-4o-mini")
    .threads(2)
    .build();

auto agent = os->agent("assistant")
    .prompt("你是一个有用的助手。")
    .tools({"kv_store", "http_fetch"})
    .create();

auto result = agent->run("今天天气怎么样？");
```

---

## 目录结构 / Project Layout

```
cpp-agent-os/
├── CMakeLists.txt
├── README.md
├── include/
│   └── agentos/
│       ├── core/
│       │   ├── compat.hpp          # 兼容层与辅助 polyfill
│       │   ├── types.hpp           # 核心类型、错误体系、Result<T>
│       │   └── task.hpp            # Task<T> 抽象
│       ├── kernel/
│       │   └── llm_kernel.hpp      # LLM 内核 + 后端抽象
│       ├── scheduler/
│       │   └── scheduler.hpp       # 优先级调度 + DAG
│       ├── context/
│       │   └── context.hpp         # 上下文窗口管理
│       ├── memory/
│       │   └── memory.hpp          # 三层记忆体系
│       ├── tools/
│       │   └── tool_manager.hpp    # 工具注册与执行
│       ├── security/
│       │   └── security.hpp        # RBAC + 污点 + ECL
│       ├── bus/
│       │   └── agent_bus.hpp       # Agent 消息总线
│       ├── mcp/
│       │   └── mcp_server.hpp      # MCP server 适配层
│       ├── headless/
│       │   └── runner.hpp          # 无头执行入口
│       ├── session/
│       │   └── session.hpp         # 会话持久化
│       ├── skills/
│       │   └── registry.hpp        # 技能注册
│       ├── hooks/
│       │   └── hook_manager.hpp    # Hook 扩展点
│       └── agent.hpp               # Agent 基类 + AgentOS 门面
├── src/
│   ├── kernel/llm_kernel.cpp
│   ├── scheduler/scheduler.cpp
│   ├── context/context.cpp
│   ├── memory/memory.cpp
│   ├── tools/tool_manager.cpp
│   ├── tools/tool_learner.cpp
│   ├── security/security.cpp
│   ├── bus/agent_bus.cpp
│   ├── tracing/tracer.cpp
│   └── worktree/worktree_manager.cpp
└── examples/
    ├── demo.cpp                    # 完整功能演示
    ├── openai_demo.cpp             # OpenAI / compatible API 示例
    ├── mcp_demo.cpp                # MCP server 示例
    └── headless_demo.cpp           # 无头运行示例
```

---

## 设计亮点 / Design Highlights

- **模块化运行时**：核心能力覆盖 agent、tools、memory、session、hooks、MCP 与 headless 入口
- **Header-first Public API**：公共接口集中在 `include/agentos/`，但核心实现仍在 `src/`
- **C++23 构建基线**：当前 CMake 以 `C++23` 编译，面向现代编译器工具链
- **可插拔后端**：`ILLMBackend` 接口可对接任意 LLM API
- **纵深安全**：RBAC + Taint + Injection Detection + ECL 四道防线
- **可观测性**：内置 `KernelMetrics`、ECL `audit_log`、Bus `audit_trail`

---

## 参考资料 / References

- [AIOS: LLM Agent Operating System](https://arxiv.org/abs/2403.16971)
- [MemGPT: Towards LLMs as Operating Systems](https://arxiv.org/abs/2310.08560)
- [Autellix: An Efficient Serving Engine for LLM Agents](https://arxiv.org/abs/2502.13965)
- [IsolateGPT: Execution Isolation Architecture for LLM-Based Agentic Systems](https://arxiv.org/abs/2410.03227)
- [Pancake: Compilable and Lightweight Agent Scheduling Framework](https://arxiv.org/abs/2503.05012)

---

## License

MIT
