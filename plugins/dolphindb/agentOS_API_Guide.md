# AgentOS DolphinDB Plugin — API 对接指南

> **版本**: 3.00.5.0 · **协议**: SSE + REST · **默认端口**: 8849

---

## 目录

1. [架构概览](#1-架构概览)
2. [快速开始](#2-快速开始)
3. [DolphinDB 函数参考](#3-dolphindb-函数参考)
4. [Web Shell 流式协议](#4-web-shell-流式协议)
5. [HTTP RESTful API](#5-http-restful-api)
6. [RAG 知识库使用指南](#6-rag-知识库使用指南)
7. [配置参考](#7-配置参考)
8. [部署清单](#8-部署清单)

---

## 1. 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                    DolphinDB Server                          │
│  ┌────────────────────────────────────────────────────────┐  │
│  │              PluginAgentOS (libPluginAgentOS.so)        │  │
│  │  ┌──────────┐  ┌──────────┐  ┌────────────────────┐   │  │
│  │  │ LLM 推理  │  │ 记忆系统  │  │ 知识图谱 / RAG KB  │   │  │
│  │  └────┬─────┘  └────┬─────┘  └────────┬───────────┘   │  │
│  │       └──────────────┴────────────────┘                │  │
│  │                      │                                  │  │
│  │              ┌───────┴────────┐                         │  │
│  │              │  SSE Server    │ ← HTTP :8849            │  │
│  │              │  (cpp-httplib) │                         │  │
│  │              └───────┬────────┘                         │  │
│  └──────────────────────┼─────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          │               │               │
    ┌─────┴─────┐  ┌─────┴─────┐  ┌──────┴──────┐
    │ Web Shell  │  │  Python   │  │  外部系统    │
    │ (前端 SSE) │  │  Client   │  │  (cURL etc) │
    └───────────┘  └───────────┘  └─────────────┘
```

**核心组件**:

- **PluginRuntime** — 全局单例，管理 AgentOS 内核生命周期
- **AsyncRequestManager** — 异步请求状态与增量 token 管理
- **SSETokenManager** — 一次性 token 生成 / 验证，60 秒过期
- **SSEServer** — 内嵌 HTTP 服务（需编译 `AGENTOS_ENABLE_SSE=ON`）
- **KBManager** — 多知识库实例管理（handle 方式）

---

## 2. 快速开始

### 2.1 加载插件

```dolphindb
loadPlugin("/path/to/PluginAgentOS.txt")
```

### 2.2 初始化（Mock 模式 / 生产模式）

```dolphindb
// Mock 模式 — 无需 API Key，用于开发测试
agentOS::init()

// 生产模式 — OpenAI
agentOS::init("sk-xxx", "https://api.openai.com/v1", "gpt-4o-mini")

// 生产模式 — OpenAI 兼容（SiliconFlow / DeepSeek / 本地 Ollama）
agentOS::init("sk-xxx", "https://api.siliconflow.cn/v1", "deepseek-chat")
```

### 2.3 第一次对话

```dolphindb
agentOS::ask("什么是 DolphinDB？")
// → "DolphinDB 是一个高性能分布式时序数据库..."

agentOS::ask("帮我写一个均线策略", "你是一个量化分析师")
// → 带自定义 system prompt 的回答
```

---

## 3. DolphinDB 函数参考

### 3.1 系统生命周期

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::init([apiKey [, baseUrl [, model [, threads [, tpmLimit]]]]])` | STRING... | BOOL | 初始化 AgentOS，幂等 |
| `agentOS::close()` | 无 | VOID | 关闭并释放所有资源 |
| `agentOS::health()` | 无 | STRING (JSON) | 健康检查 `{"healthy":true,...}` |
| `agentOS::status()` | 无 | STRING | 运行状态摘要 |

### 3.2 单轮对话

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::ask(question [, systemPrompt])` | STRING, STRING | STRING | 同步 LLM 推理 |
| `agentOS::askStream(question [, systemPrompt [, callback]])` | STRING, STRING, FUNCTION | STRING | 流式推理，callback 逐 token 回调 |
| `agentOS::askTable(question [, configJson [, agentHandle]])` | STRING, STRING, LONG | TABLE | 结构化返回（含 tool call 记录） |

**askTable 返回表结构**:

| 列名 | 类型 | 说明 |
|------|------|------|
| role | STRING | assistant / tool |
| content | STRING | 回答内容 |
| tool_name | STRING | 工具名称（如有） |
| tool_args | STRING | 工具参数 JSON |
| tool_result | STRING | 工具执行结果 |

### 3.3 异步流式（Web 集成）

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::askAsync(question [, systemPrompt])` | STRING, STRING | DICT | 发起异步请求，返回 `__stream__` 标记 dict |
| `agentOS::poll(requestId)` | STRING | DICT | 拉取增量 token |
| `agentOS::cancelAsync(requestId)` | STRING | BOOL | 取消异步请求 |

**askAsync 返回 dict 结构**:

```json
{
  "__stream__": true,
  "requestId": "req-xxxxxxxx",
  "status": "running",
  "sseUrl": "http://localhost:8849/sse",
  "token": "abc123..."
}
```

**poll 返回 dict 结构**:

```json
{
  "status": "running|done|error",
  "delta": "新增的 token（读后清空）",
  "content": "完整累积内容",
  "done": false,
  "error": ""
}
```

### 3.4 记忆系统

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::remember(content [, importance [, source]])` | STRING, FLOAT, STRING | STRING | 存储长期记忆，返回 ID |
| `agentOS::recall(query [, topK])` | STRING, INT | TABLE | 检索记忆 |

**recall 返回表**: `content | score | source | created_at`

### 3.5 知识图谱

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::graphAddNode(id, type, content)` | STRING, STRING, STRING | STRING | 添加节点，返回 ID |
| `agentOS::graphAddEdge(source, target, relation [, weight])` | STRING, STRING, STRING, DOUBLE | BOOL | 添加边 |
| `agentOS::graphQuery(nodeId [, maxResults])` | STRING, INT | TABLE | 查询关系 |

**示例**:
```dolphindb
agentOS::graphAddNode("AAPL", "stock", "Apple Inc.")
agentOS::graphAddEdge("AAPL", "tech_sector", "belongs_to", 1.0)
```

**graphQuery 返回表**: `source | relation | target | weight`

### 3.6 工具注册

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::registerTool(name, description, handler [, params])` | STRING, STRING, FUNCTION, STRING | BOOL | 注册 DolphinDB 函数为 Agent 工具 |

**示例**:

```dolphindb
def queryPrice(paramsJson) {
    params = parseExpr(paramsJson).eval()
    return exec last price from loadTable("dfs://market", "trades") where symbol = params["symbol"]
}
agentOS::registerTool("query_stock_price", "查询股票最新价格", queryPrice)
```

### 3.7 Agent 管理

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::createAgent(name [, prompt, tools, skills, blockTools, contextLimit, isolation, maxSteps])` | STRING... | LONG | 创建持久 Agent，返回 handle |
| `agentOS::destroy(handle)` | LONG | BOOL | 销毁 Agent |

**示例**:

```dolphindb
agent = agentOS::createAgent(
    name = "quant_agent",
    prompt = "你是量化分析师",
    tools = ["query_price"],
    contextLimit = 16384
)
```

### 3.8 RAG 知识库

| 函数 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `agentOS::createKB([chunkSize [, chunkOverlap [, embeddingModel]]])` | INT, INT, STRING | LONG | 创建知识库，返回 handle |
| `agentOS::destroyKB(handle)` | LONG | BOOL | 销毁知识库 |
| `agentOS::saveKB(handle, dirPath)` | LONG, STRING | BOOL | 持久化到磁盘 |
| `agentOS::loadKB(handle, dirPath)` | LONG, STRING | BOOL | 从磁盘加载 |
| `agentOS::kbInfo(handle)` | LONG | DICT | 知识库状态信息 |
| `agentOS::ingest(handle, docId, text)` | LONG, STRING, STRING | INT | 导入文本，返回 chunk 数 |
| `agentOS::ingestDir(handle, dirPath)` | LONG, STRING | BOOL | 批量导入目录 (.md/.txt) |
| `agentOS::removeDoc(handle, docId)` | LONG, STRING | BOOL | 移除文档 |
| `agentOS::search(handle, query [, topK [, graphHops]])` | LONG, STRING, INT, INT | TABLE | 混合检索（BM25 + HNSW + RRF） |
| `agentOS::askWithKB(handle, question [, topK [, systemPrompt]])` | LONG, STRING, INT, STRING | STRING | RAG 同步对话 |
| `agentOS::askWithKBAsync(handle, question [, topK [, systemPrompt]])` | LONG, STRING, INT, STRING | DICT | RAG 流式对话（`__stream__` dict） |

**createKB 参数**:

```dolphindb
// createKB([chunkSize], [chunkOverlap], [embeddingModel])
kb = agentOS::createKB(500, 50, "text-embedding-3-small")
// 全部可选，默认: chunkSize=500, chunkOverlap=50, embeddingModel="text-embedding-3-small"
```

**search 返回表**: `doc_id | chunk_id | content | score | graph_context`

**kbInfo 返回 dict**: `{chunk_count, doc_count, embedding_model, chunk_size, chunk_overlap}`

---

## 4. Web Shell 流式协议

### 4.1 `__stream__` 协议

DolphinDB Web Shell 前端渲染基于 `DdbObj.form` 属性。`askAsync` 返回的 dict 包含 `__stream__: true` 标记，前端检测到此标记后自动进入流式模式。

**前端检测逻辑（TypeScript）**:

```typescript
const result = await ddb.run("agentOS::askAsync('你的问题')")

// 检测 __stream__ 标记
if (result.form === DdbForm.dict) {
    const dict = result.to_dict()
    if (dict['__stream__'] === true) {
        const requestId = dict['requestId']
        const sseUrl = dict['sseUrl']    // 可选
        const token = dict['token']      // 可选

        if (sseUrl && token) {
            // 方式 A: SSE 推送（推荐）
            connectSSE(sseUrl, requestId, token)
        } else {
            // 方式 B: 轮询 fallback
            startPolling(requestId)
        }
    }
}
```

### 4.2 方式 A — SSE 推送（推荐）

真正的 Server-Push，延迟最低。前端通过 EventSource 连接内嵌 SSE 服务器。

```typescript
function connectSSE(sseUrl: string, rid: string, token: string) {
    const es = new EventSource(`${sseUrl}?rid=${rid}&token=${token}`)

    es.onmessage = (e) => {
        const data = JSON.parse(e.data)
        if (data.done) {
            es.close()
            // 流式结束
            return
        }
        if (data.error) {
            es.close()
            console.error(data.error)
            return
        }
        // 增量 token 追加到 UI
        appendToOutput(data.delta)
    }

    es.onerror = () => {
        es.close()
        // 可 fallback 到轮询
    }
}
```

### 4.3 方式 B — Poll 轮询

不依赖额外端口，通过 `ddb.run()` 定时拉取。

```typescript
async function startPolling(rid: string) {
    while (true) {
        const r = await ddb.run(`agentOS::poll("${rid}")`)
        const dict = r.to_dict()

        if (dict['delta']) {
            appendToOutput(dict['delta'])
        }
        if (dict['done'] === true) break
        if (dict['error']) {
            console.error(dict['error'])
            break
        }
        await sleep(200)  // 200ms 间隔
    }
    // 清理
    await ddb.run(`agentOS::cancelAsync("${rid}")`)
}
```

### 4.4 SSE vs Poll 对比

| 特性 | SSE 推送 | Poll 轮询 |
|------|---------|----------|
| 延迟 | ~即时 | ~200ms |
| 网络开销 | 低（长连接） | 高（频繁请求） |
| 额外端口 | 需要 8849 | 不需要 |
| 防火墙兼容 | 需开放端口 | 完全兼容 |
| 安全机制 | 一次性 token | 走 DolphinDB 认证 |
| 实现复杂度 | 中 | 低 |

---

## 5. HTTP RESTful API

SSE 服务器同时提供 REST 端点，供 Python / cURL / 外部系统直接调用，无需 DolphinDB 客户端。

> **前提**: 编译时启用 `AGENTOS_ENABLE_SSE=ON`，默认端口 `8849`

### 5.1 健康检查

```
GET /health
```

**响应**:

```json
{"status": "ok", "service": "agentOS-sse"}
```

### 5.2 SSE 流式端点

```
GET /sse?rid={requestId}&token={token}
```

连接后以 `text/event-stream` 推送:

```
data: {"delta":"你","done":false}

data: {"delta":"好","done":false}

data: {"delta":"","done":true}
```

### 5.3 知识库搜索

```
POST /api/kb/search
Content-Type: application/json

{
    "kb_handle": 1,
    "query": "DolphinDB 分区策略",
    "top_k": 5,
    "graph_hops": 0
}
```

**响应**:

```json
[
    {
        "doc_id": "partition_guide.md",
        "chunk_id": 3,
        "content": "RANGE 分区适合时间序列数据...",
        "score": 0.87,
        "graph_context": ""
    }
]
```

### 5.4 知识库导入

```
POST /api/kb/ingest
Content-Type: application/json

{
    "kb_handle": 1,
    "doc_id": "my_doc",
    "text": "这里是文档内容..."
}
```

**响应**:

```json
{"chunks": 12}
```

### 5.5 RAG 流式问答

```
GET /api/kb/ask?kb_handle=1&q=如何选择分区方案&top_k=5&token=xxx
```

**响应**（Event Stream）:

```
event: sources
data: [{"doc_id":"guide.md","content":"...","score":0.9}]

data: {"delta":"根据","done":false}

data: {"delta":"您的","done":false}

data: {"delta":"","done":true}
```

首先发送 `event: sources` 携带检索来源，然后逐 token 推送。

### 5.6 调用示例

**Python**:

```python
import requests
import sseclient

# 搜索
resp = requests.post("http://localhost:8849/api/kb/search", json={
    "kb_handle": 1,
    "query": "分区策略",
    "top_k": 5
})
results = resp.json()

# RAG 流式问答
resp = requests.get("http://localhost:8849/api/kb/ask", params={
    "kb_handle": 1, "q": "如何优化查询", "top_k": 5
}, stream=True)
client = sseclient.SSEClient(resp)
for event in client.events():
    if event.event == "sources":
        print("来源:", event.data)
    else:
        data = json.loads(event.data)
        if data["done"]:
            break
        print(data["delta"], end="", flush=True)
```

**cURL**:

```bash
# 健康检查
curl http://localhost:8849/health

# 搜索
curl -X POST http://localhost:8849/api/kb/search \
  -H "Content-Type: application/json" \
  -d '{"kb_handle":1,"query":"分区策略","top_k":5}'

# 导入文档
curl -X POST http://localhost:8849/api/kb/ingest \
  -H "Content-Type: application/json" \
  -d '{"kb_handle":1,"doc_id":"doc1","text":"文档内容..."}'

# RAG 流式问答
curl -N "http://localhost:8849/api/kb/ask?kb_handle=1&q=如何分区&top_k=5"
```

---

## 6. RAG 知识库使用指南

### 6.1 完整流程

```dolphindb
// 1. 创建知识库
kb = agentOS::createKB(500, 50, "text-embedding-3-small")

// 2. 导入数据
agentOS::ingest(kb, "intro", "DolphinDB 是高性能分布式时序数据库...")
agentOS::ingest(kb, "partition", "DolphinDB 支持 RANGE / HASH / VALUE / LIST 四种分区方式...")
agentOS::ingestDir(kb, "/data/docs/")  // 批量导入目录

// 3. 查看状态
agentOS::kbInfo(kb)
// → {chunk_count: 42, doc_count: 5, embedding_model: "text-embedding-3-small", ...}

// 4. 检索
agentOS::search(kb, "分区策略", 3)
// → 返回 Table: doc_id | chunk_id | content | score | graph_context

// 5. RAG 对话（同步）
agentOS::askWithKB(kb, "如何选择分区方案？")
// → "根据您的数据特征，建议使用 RANGE 分区..."

// 6. RAG 对话（流式）
agentOS::askWithKBAsync(kb, "帮我优化这个查询")
// → {__stream__: true, requestId: "req-xxx", sseUrl: "...", token: "..."}

// 7. 持久化
agentOS::saveKB(kb, "/data/kb_backup/")

// 8. 重新加载
kb2 = agentOS::createKB()
agentOS::loadKB(kb2, "/data/kb_backup/")

// 9. 删除文档
agentOS::removeDoc(kb, "intro")

// 10. 销毁
agentOS::destroyKB(kb)
```

### 6.2 检索原理

混合检索采用 BM25 + HNSW + RRF（Reciprocal Rank Fusion）三路融合:

```
┌──────────┐     ┌───────────┐
│  BM25    │     │   HNSW    │
│ (关键词)  │     │ (向量语义) │
└────┬─────┘     └─────┬─────┘
     │                  │
     └────────┬─────────┘
              │
        ┌─────┴──────┐
        │  RRF Fusion │
        │  (排序融合)  │
        └─────┬──────┘
              │
        ┌─────┴──────┐
        │  Top-K 结果  │
        └────────────┘
```

- **BM25**: 基于词频的精确匹配，擅长关键词命中
- **HNSW**: 基于 embedding 向量的近似最近邻检索，擅长语义相似
- **RRF**: 将两路结果通过倒数排名融合，综合最优

---

## 7. 配置参考

### 7.1 agentOS::init 参数

```dolphindb
agentOS::init([apiKey], [baseUrl], [model], [threads], [tpmLimit])
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `apiKey` | STRING | — | LLM API 密钥（空则 Mock 模式） |
| `baseUrl` | STRING | `https://api.openai.com/v1` | API 端点 |
| `model` | STRING | `gpt-4o-mini` | 模型名称 |
| `threads` | INT | `4` | 调度线程数 |
| `tpmLimit` | INT | `100000` | TPM 限流 |

### 7.2 createKB 参数

```dolphindb
agentOS::createKB([chunkSize], [chunkOverlap], [embeddingModel])
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `chunkSize` | INT | `500` | 分块大小（字符） |
| `chunkOverlap` | INT | `50` | 块间重叠 |
| `embeddingModel` | STRING | `text-embedding-3-small` | Embedding 模型 |

### 7.3 编译选项

| CMake 选项 | 默认值 | 说明 |
|-----------|--------|------|
| `AGENTOS_ENABLE_SSE` | `OFF` | 启用内嵌 SSE 服务器（需 cpp-httplib） |

---

## 8. 部署清单

### 编译 & 部署

- [ ] 下载 [cpp-httplib](https://github.com/yhirose/cpp-httplib) 的 `httplib.h` 到 `third_party/` 或系统 include 路径
- [ ] CMake 构建: `cmake -DAGENTOS_ENABLE_SSE=ON ..`
- [ ] 产出 `libPluginAgentOS.so` + `PluginAgentOS.txt` 拷贝至 DolphinDB plugins 目录
- [ ] 配置 DolphinDB `dolphindb.cfg` 允许加载插件

### 网络 & 安全

- [ ] 如使用 SSE，开放端口 8849（或自定义端口）
- [ ] 配置 `sse_cors_origin` 为实际前端域名（生产环境不建议用 `*`）
- [ ] 确认 DolphinDB 服务器可访问 LLM API 端点

### 验证

- [ ] `loadPlugin` 成功
- [ ] `agentOS::init(...)` 返回 `true`
- [ ] `agentOS::health()` 返回 `{"healthy":true,...}`
- [ ] `agentOS::ask("hello")` 返回正常
- [ ] `curl http://localhost:8849/health` 返回 `{"status":"ok"}`

---

## 附录: PluginAgentOS.txt 函数清单（41 个）

```
agentOS,libPluginAgentOS.so,3.00.5.0

# 系统
agentOSInit,init,system,0,5,0
agentOSClose,close,command,0,0,0
agentOSHealth,health,system,0,0,0
agentOSStatus,status,system,0,0,0

# Agent 管理
agentOSCreateAgent,createAgent,system,1,8,0
agentOSDestroy,destroy,system,1,1,0
agentOSInfo,info,system,1,1,0

# 对话
agentOSAsk,ask,system,1,3,0
agentOSAskStream,askStream,system,1,4,0
agentOSAskTable,askTable,system,1,4,0
agentOSRun,run,system,2,5,0

# 异步流式
agentOSAskAsync,askAsync,system,1,2,0
agentOSPoll,poll,system,1,1,0
agentOSCancelAsync,cancelAsync,system,1,1,0

# 会话
agentOSSave,save,system,1,2,0
agentOSResume,resume,system,1,1,0
agentOSSessions,sessions,system,0,0,0

# 工具 & 技能
agentOSRegisterTool,registerTool,system,2,4,0
agentOSRegisterSkill,registerSkill,system,2,4,0
agentOSBlockTool,blockTool,system,2,3,0
agentOSUnblockTool,unblockTool,system,2,2,0
agentOSActivateSkill,activateSkill,system,2,2,0
agentOSDeactivateSkill,deactivateSkill,system,2,2,0

# 记忆
agentOSRemember,remember,system,1,3,0
agentOSRecall,recall,system,1,2,0

# 知识图谱
agentOSGraphAddNode,graphAddNode,system,1,3,0
agentOSGraphAddEdge,graphAddEdge,system,2,4,0
agentOSGraphQuery,graphQuery,system,1,2,0

# RAG 知识库
agentOSCreateKB,createKB,system,0,3,0
agentOSDestroyKB,destroyKB,system,1,1,0
agentOSSaveKB,saveKB,system,2,2,0
agentOSLoadKB,loadKB,system,2,2,0
agentOSIngest,ingest,system,3,3,0
agentOSIngestDir,ingestDir,system,2,2,0
agentOSRemoveDoc,removeDoc,system,2,2,0
agentOSSearch,search,system,2,4,0
agentOSAskWithKB,askWithKB,system,2,4,0
agentOSAskWithKBAsync,askWithKBAsync,system,2,4,0
agentOSKBInfo,kbInfo,system,1,1,0
```
