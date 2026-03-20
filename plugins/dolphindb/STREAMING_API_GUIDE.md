# AgentOS DolphinDB Plugin — Web Streaming API Integration Guide

> v1.0 | 2026-03-10 | For Frontend Engineering Team

## 1. Overview

DolphinDB 插件函数是同步阻塞的——函数返回前，Web 客户端拿不到任何中间结果。为实现流式输出，插件提供 **Async + Poll** 模式：

- `askAsync` 在后台线程启动 LLM 流式推理，立即返回 `requestId`
- `poll` 每次调用返回自上次以来新增的 `delta` token，取走即清空
- 前端以 ~100ms 间隔轮询 `poll`，将 `delta` 追加到 UI

## 2. Architecture

```
  Browser / Web UI                       DolphinDB Server + Plugin
  ┌───────────────────────┐              ┌──────────────────────────┐
  │                       │  askAsync    │                          │
  │  1. User sends query ──────────────> │  Spawn background thread │
  │                       │  <-- rid --- │  running stream_infer()  │
  │                       │              │         │                │
  │  2. Start polling     │              │    token ──> AsyncRequest│
  │     setInterval(100)  │  poll(rid)   │         │   (mutex buf)  │
  │         ├─────────────────────────>  │         │                │
  │         │<── {delta} ─────────────── │  return delta & clear    │
  │         │             │              │         │                │
  │  3. Append delta      │              │    ...more tokens...     │
  │     to DOM            │  poll(rid)   │         │                │
  │         ├─────────────────────────>  │         │                │
  │         │<── {done:T} ────────────── │  mark_done()             │
  │                       │              │                          │
  │  4. Stop polling      │              │  Thread joins & cleanup  │
  └───────────────────────┘              └──────────────────────────┘
```

## 3. API Reference

### 3.1 `agentOS::askAsync(question, [systemPrompt])`

发起异步 LLM 请求，立即返回。

| Parameter | Type | Description |
|-----------|------|-------------|
| question | `STRING` | 用户问题（必填） |
| systemPrompt | `STRING` | 可选 system prompt |

**Returns:** `STRING` — requestId (如 `"req_1"`, `"req_2"`)

```dolphindb
rid = agentOS::askAsync("什么是 DolphinDB？")
rid = agentOS::askAsync("分析趋势", "你是金融分析师")
```

### 3.2 `agentOS::poll(requestId)`

获取增量 token。每次调用返回 delta 并清空缓冲区。

**Returns:** `DICTIONARY`，包含以下字段：

| Key | Type | Description |
|-----|------|-------------|
| status | `STRING` | `"streaming"` / `"done"` / `"error"` |
| delta | `STRING` | 本次新增的 token（取后清空，纯增量） |
| content | `STRING` | **仅 `done`/`error` 时返回完整内容，`streaming` 时为空** |
| done | `BOOL` | 是否已完成（done 或 error 时为 true） |
| error | `STRING` | 错误信息（无错误时为空字符串） |

> **Important:** poll 是纯增量的。`delta` 每次只返回新 token，取走即清空；`content` 仅在生成结束时返回一次完整内容，streaming 期间为空（省带宽）。前端应自行拼接 delta 构建显示文本。

### 3.3 `agentOS::cancelAsync(requestId)`

取消/清理异步请求。后台线程 detach 后自然结束，结果被丢弃。

**Returns:** `BOOL` — 始终 true

## 4. Frontend Integration

### 4.1 JavaScript 封装类

```javascript
class AgentOSStream {
  constructor(ddbConn, pollInterval = 100) {
    this.conn = ddbConn;
    this.pollInterval = pollInterval;
  }

  async askStreaming(question, systemPrompt, onToken, onDone, onError) {
    // Step 1: 发起异步请求
    const promptArg = systemPrompt ? `, "${systemPrompt}"` : "";
    const rid = await this.conn.run(
      `agentOS::askAsync("${question}"${promptArg})`
    );

    // Step 2: 轮询（纯增量，前端自行拼接 delta）
    let accumulated = "";
    const timer = setInterval(async () => {
      try {
        const r = await this.conn.run(`agentOS::poll("${rid}")`);

        if (r.delta && r.delta.length > 0) {
          accumulated += r.delta;
          onToken(r.delta, accumulated);
        }

        if (r.done) {
          clearInterval(timer);
          // done 时 r.content 有完整内容，可用于校验
          const fullContent = r.content || accumulated;
          if (r.status === "error") {
            onError?.(r.error);
          } else {
            onDone?.(fullContent);
          }
        }
      } catch (err) {
        clearInterval(timer);
        onError?.(err.message);
      }
    }, this.pollInterval);

    // 返回取消句柄
    return {
      requestId: rid,
      cancel: () => {
        clearInterval(timer);
        this.conn.run(`agentOS::cancelAsync("${rid}")`);
      }
    };
  }
}
```

### 4.2 Usage Example

```javascript
const stream = new AgentOSStream(ddbConnection);
const outputEl = document.getElementById("llm-output");
outputEl.textContent = "";

const handle = await stream.askStreaming(
  "什么是 DolphinDB？",
  "用中文回答",
  // onToken
  (delta, fullContent) => {
    outputEl.textContent += delta;
  },
  // onDone
  (content) => {
    console.log("Complete:", content.length, "chars");
  },
  // onError
  (err) => {
    outputEl.textContent += "\n[Error: " + err + "]";
  }
);

// 取消按钮
cancelBtn.onclick = () => handle.cancel();
```

### 4.3 React Component

```jsx
function StreamingChat({ conn }) {
  const [text, setText] = useState("");
  const [loading, setLoading] = useState(false);
  const handleRef = useRef(null);

  const ask = async (question) => {
    setText("");
    setLoading(true);

    const stream = new AgentOSStream(conn);
    handleRef.current = await stream.askStreaming(
      question, null,
      (delta) => setText(prev => prev + delta),
      ()      => setLoading(false),
      (err)   => { setText(prev => prev + "\n[Error: " + err + "]"); setLoading(false); }
    );
  };

  const cancel = () => {
    handleRef.current?.cancel();
    setLoading(false);
  };

  return (
    <div className="chat-container">
      <div className="output">
        {text}
        {loading && <span className="cursor blink">|</span>}
      </div>
      {loading && <button onClick={cancel}>Stop</button>}
    </div>
  );
}
```

### 4.4 Python Client

适用于 Streamlit / Dash 等 Python 前端：

```python
import dolphindb as ddb
import time

s = ddb.session()
s.connect("localhost", 8848, "admin", "123456")

# 发起异步请求
rid = s.run('agentOS::askAsync("什么是 DolphinDB？")')

# 轮询
while True:
    r = s.run(f"agentOS::poll('{rid}')")
    if r["delta"]:
        print(r["delta"], end="", flush=True)
    if r["done"]:
        break
    time.sleep(0.1)

print()
# 完整内容: r["content"]
```

## 5. Performance & Best Practices

### 5.1 Polling Interval

| Interval | 效果 | 注意 |
|----------|------|------|
| 50ms | 非常流畅 | DDB 调用量大，server 负载高 |
| **100ms（推荐）** | **流畅度和效率平衡** | **token 会有轻微批量** |
| 200ms | 负载低 | 输出有明显分块感 |
| 500ms | 最低负载 | 体验卡顿 |

### 5.2 Thread Safety

`AsyncRequest` 内部用 `std::mutex` 保护所有共享状态。**支持并发多个请求**——每个 requestId 独立缓冲区。`AsyncRequestManager` 是进程级单例，有独立的 mutex。

### 5.3 Resource Cleanup

| 场景 | 行为 |
|------|------|
| `poll` 返回 `done=true` | worker thread 自动 join，可安全丢弃 rid |
| 调用 `cancelAsync(rid)` | 请求从管理器移除，worker detach 后台结束 |
| 两者都没做 | worker 自然结束，AsyncRequest 保留在内存直到 `agentOS::close()` |

### 5.4 Error Handling

生成过程中的错误被捕获到 AsyncRequest 中。下次 poll 返回：

```json
{
  "status": "error",
  "delta": "",
  "content": "生成到一半的内容...",
  "done": true,
  "error": "AgentOS error [3]: API timeout"
}
```

前端检查 `status === "error"` 并展示错误。`content` 仍然包含出错前已生成的内容。

## 6. Complete API Summary

| Function | Return | Description |
|----------|--------|-------------|
| `agentOS::init([config])` | BOOL | 初始化 AgentOS |
| `agentOS::close()` | void | 关闭并释放资源 |
| `agentOS::ask(q, [sys])` | STRING | 同步单轮对话 |
| `agentOS::askStream(q, [sys], [cb])` | STRING | 回调式流式对话 |
| **`agentOS::askAsync(q, [sys])`** | **STRING** | **异步请求，返回 requestId** |
| **`agentOS::poll(rid)`** | **DICT** | **获取增量 token** |
| **`agentOS::cancelAsync(rid)`** | **BOOL** | **取消异步请求** |
| `agentOS::askTable(q, [cfg], [h])` | TABLE | 多步对话（含工具调用） |
| `agentOS::remember(c, [imp], [src])` | STRING | 存入长期记忆 |
| `agentOS::recall(q, [topK])` | TABLE | 检索记忆 |
| `agentOS::graphAddNode(json)` | STRING | 添加知识图谱节点 |
| `agentOS::graphAddEdge(json)` | BOOL | 添加知识图谱边 |
| `agentOS::graphQuery(id, [max])` | TABLE | 查询知识图谱 |
| `agentOS::health()` | STRING | 健康检查 (JSON) |
| `agentOS::status()` | STRING | 系统状态摘要 |
| `agentOS::registerTool(s, cb)` | BOOL | 注册 DDB 函数为工具 |
| `agentOS::createAgent2(name, ...)` | LONG | 显式 V2 创建 Agent |
| `agentOS::ask2(h, q, [sys])` | STRING | 持久 Agent 同步对话 |
| `agentOS::askStream2(h, q, [sys], [cb])` | STRING | 持久 Agent 流式对话 |
| `agentOS::destroy(h)` | BOOL | 销毁 Agent |
