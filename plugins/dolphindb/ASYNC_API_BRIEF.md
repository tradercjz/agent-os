# agentOS 流式输出方案 — 前后端协同简报

## 背景

agentOS 插件运行在 DolphinDB Server 进程内，所有函数（`ask`、`askAsync`、`poll` 等）通过脚本执行调用。DolphinDB Web 前端根据返回对象的 `form` 属性（scalar / dict / table / vector…）自动路由到对应的渲染组件。

流式输出的核心问题：LLM 生成是逐 token 的，但 DolphinDB 的脚本执行是「请求 → 完整响应」模型。我们需要一种机制，让前端在现有的 `ddb.run()` 通道上实现逐字渲染。

---

## 方案：`__stream__` 标记协议

### 整体流程

```
用户在 Web Shell 执行脚本:
    agentOS::askAsync("帮我分析最近的交易数据")
                │
                ▼
    DolphinDB 返回 dict (form = DdbForm.dict):
    ┌──────────────────────────────────┐
    │  __stream__ : true               │  ← 流式标记
    │  requestId  : "a1b2c3d4-..."     │
    │  status     : "streaming"        │
    └──────────────────────────────────┘
                │
                ▼
    前端检测到 __stream__ == true
    自动进入流式轮询模式（用户无感知）
                │
                ▼
    前端内部循环调用 ddb.run('agentOS::poll("a1b2c3d4-...")'):
    ┌──────────────────────────────────┐
    │  第1次: {delta:"根据",  done:false}│  → 追加渲染 "根据"
    │  第2次: {delta:"你的",  done:false}│  → 追加渲染 "你的"
    │  第3次: {delta:"交易",  done:false}│  → 追加渲染 "交易"
    │  ...                              │
    │  第N次: {delta:"。",    done:true, │  → 渲染完成
    │          content:"完整回答..."}    │
    └──────────────────────────────────┘
```

不需要额外端口、不需要 SSE/WebSocket、不需要代理层，完全走现有 DDB 连接。

---

## 插件接口定义

### 1. askAsync(question [, systemPrompt]) → dict

发起异步推理。后台线程开始调用 LLM，立即返回一个 dict。

```dolphindb
result = agentOS::askAsync("帮我分析最近的交易数据")
// result 是一个 dict:
//   __stream__ = true
//   requestId  = "a1b2c3d4-..."
//   status     = "streaming"
```

**关键**：返回类型是 `dict`（`DdbForm.dict`），不是 `STRING`。前端以此判断是否进入流式模式。

### 2. poll(requestId) → dict

轮询增量结果。

| 字段 | 类型 | 说明 |
|------|------|------|
| status | STRING | `"streaming"` / `"done"` / `"error"` |
| delta | STRING | 本次新增的 token（增量，读后即清） |
| content | STRING | streaming 时为空；done/error 时返回完整内容 |
| done | BOOL | 是否结束 |
| error | STRING | 错误信息，正常时为空 |

`delta` 是纯增量的——每次 poll 只返回上次 poll 之后新生成的 token，读取后自动清空。前端需要自行拼接。`content` 在 streaming 期间为空字符串，仅在 `done=true` 或 `error` 时返回一次完整内容，用于校验。

### 3. cancelAsync(requestId) → bool

取消进行中的请求，释放资源。

---

## 前端改动说明

### 需要改动的位置

在 `src/shell/model.ts` 现有的结果处理逻辑中，**增加一个 dict 类型的前置判断**：

```typescript
// === 现有代码（不动） ===
if (
    ddbobj.form === DdbForm.chart ||
    ddbobj.form === DdbForm.dict ||   // dict 会走到这里
    ddbobj.form === DdbForm.matrix ||
    // ...
)
    // === 新增：在 set result 之前检查 __stream__ ===
    if (ddbobj.form === DdbForm.dict) {
        const streamFlag = ddbobj.value?.get?.('__stream__');
        if (streamFlag?.value === true) {
            this.handleStreamResponse(ddbobj);
            return;
        }
    }
    // === 原有逻辑 ===
    this.set({ result: { type: 'object', data: ddbobj } })
```

### 流式渲染逻辑（新增方法）

```typescript
async handleStreamResponse(ddbobj: DdbObj) {
    const requestId = ddbobj.value.get('requestId').value;
    let accumulated = '';

    // 进入流式渲染状态
    this.set({ result: { type: 'stream', text: '' } });

    while (true) {
        // 通过现有 ddb 连接调用 poll
        const pollResult = await this.ddb.run(
            `agentOS::poll("${requestId}")`
        );

        const delta  = pollResult.value.get('delta').value;
        const done   = pollResult.value.get('done').value;
        const error  = pollResult.value.get('error').value;

        if (delta) {
            accumulated += delta;
            // 增量更新 UI（逐字渲染）
            this.set({ result: { type: 'stream', text: accumulated } });
        }

        if (done) {
            if (error) {
                this.set({ result: { type: 'stream', text: accumulated, error } });
            }
            break;
        }

        // 轮询间隔 50~100ms，可根据体验调整
        await sleep(50);
    }
}
```

### 渲染组件

前端需要新增一个 `type: 'stream'` 的结果渲染组件（或复用终端区域），逐步显示 `text` 内容。建议支持 Markdown 渲染（LLM 输出通常包含 Markdown 格式）。

---

## 方案 B（推荐）：SSE 真推送

插件可选内嵌一个轻量 HTTP 服务（基于 [cpp-httplib](https://github.com/yhirose/cpp-httplib)），提供标准 SSE 端点。前端用 `EventSource` 直连，token 一出来立刻推送，无需轮询。

### 开启方式

编译时加 `-DAGENTOS_ENABLE_SSE=ON`，并将 `httplib.h` 放到 `third_party/httplib/` 目录：

```bash
# 下载 cpp-httplib（header-only，单文件）
wget -O third_party/httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

# 编译时开启 SSE
cmake -DAGENTOS_ENABLE_SSE=ON -DAGENTOS_NO_DUCKDB=ON ..
```

### SSE 开启后 askAsync 返回值变化

```dolphindb
result = agentOS::askAsync("帮我分析最近的交易数据")
// 返回 dict:
//   __stream__ = true
//   requestId  = "req_42"
//   status     = "streaming"
//   sseUrl     = "http://localhost:8849/sse"    ← 新增
//   token      = "a3f7...8e1d"                  ← 新增（一次性令牌）
```

相比方案 A 多了两个字段：`sseUrl`（SSE 端点地址）和 `token`（一次性安全令牌）。

### 安全机制

- **token 绑定 requestId**：每个 token 只能消费对应的 requestId
- **一次性**：首次 SSE 连接后 token 立即失效，防重放
- **自动过期**：60 秒内未使用自动作废
- **内存存储**：进程重启自动清空，无持久化风险

### 前端对接（SSE 优先，poll 兜底）

```typescript
async handleStreamResponse(ddbobj: DdbObj) {
    const requestId = ddbobj.value.get('requestId').value;
    const sseUrl    = ddbobj.value.get('sseUrl')?.value;
    const token     = ddbobj.value.get('token')?.value;

    let accumulated = '';
    this.set({ result: { type: 'stream', text: '' } });

    // 优先 SSE，无 sseUrl 时降级为 poll
    if (sseUrl && token) {
        await this.streamViaSSE(sseUrl, requestId, token, accumulated);
    } else {
        await this.streamViaPoll(requestId, accumulated);
    }
}

// 方案 B: SSE 真推送
async streamViaSSE(sseUrl: string, rid: string, token: string, accumulated: string) {
    return new Promise<void>((resolve, reject) => {
        const url = `${sseUrl}?rid=${rid}&token=${encodeURIComponent(token)}`;
        const es = new EventSource(url);

        es.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                if (data.delta) {
                    accumulated += data.delta;
                    this.set({ result: { type: 'stream', text: accumulated } });
                }
                if (data.done) {
                    es.close();
                    resolve();
                }
                if (data.error) {
                    es.close();
                    this.set({ result: { type: 'stream', text: accumulated, error: data.error } });
                    resolve();
                }
            } catch (e) {
                // 非 JSON 数据，忽略
            }
        };

        es.onerror = () => {
            es.close();
            // SSE 断开时降级为 poll 完成剩余部分
            this.streamViaPoll(rid, accumulated).then(resolve);
        };
    });
}

// 方案 A: poll 兜底
async streamViaPoll(rid: string, accumulated: string) {
    while (true) {
        const pollResult = await this.ddb.run(`agentOS::poll("${rid}")`);
        const delta = pollResult.value.get('delta').value;
        const done  = pollResult.value.get('done').value;
        const error = pollResult.value.get('error').value;

        if (delta) {
            accumulated += delta;
            this.set({ result: { type: 'stream', text: accumulated } });
        }
        if (done) {
            if (error) {
                this.set({ result: { type: 'stream', text: accumulated, error } });
            }
            break;
        }
        await sleep(50);
    }
}
```

### 配置

在 `agentOS::init` 中传入 API Key 即可，SSE 端口默认 8849：

```dolphindb
agentOS::init("sk-xxx")
```

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| sse_port | 8849 | SSE 服务监听端口 |
| sse_cors | `*` | CORS 允许的源，生产环境建议设为具体域名 |

---

## 两种方案对比

| | 方案 A: Poll | 方案 B: SSE（推荐） |
|--|-------------|-------------------|
| 编译选项 | 默认可用 | 需 `-DAGENTOS_ENABLE_SSE=ON` |
| 额外依赖 | 无 | cpp-httplib（header-only 单文件） |
| 额外端口 | 无 | 需要一个（默认 8849） |
| 延迟 | 轮询间隔 + RTT | 仅网络传输延迟 |
| 空请求 | 大量无效 poll | 无（服务端推送） |
| 体验 | 可接受，略有顿挫 | 流畅，与 ChatGPT 一致 |
| 降级 | — | SSE 断开自动降级为 poll |

**建议**：生产环境开启 SSE，开发/测试可先用 poll 快速调通。两个方案可以共存，前端代码自动选择。

---

## 完整示例（DolphinDB 脚本）

```dolphindb
// 加载插件
loadPlugin("/path/to/PluginAgentOS.txt")

// 初始化（开启 SSE 时会自动启动 SSE 服务）
agentOS::init("sk-xxx")

// 单轮同步调用（阻塞，等完整结果；持久 Agent 场景请用 ask2）
result = agentOS::ask("什么是DolphinDB？")

// 异步流式调用
streamObj = agentOS::askAsync("帮我分析最近的交易数据")
// streamObj 是 dict:
//   __stream__ = true
//   requestId  = "req_1"
//   status     = "streaming"
//   sseUrl     = "http://localhost:8849/sse"    (SSE 开启时)
//   token      = "a3f7...8e1d"                  (SSE 开启时)

// 手动 poll（一般由前端自动执行，此处仅演示）
pollResult = agentOS::poll(streamObj["requestId"])
// pollResult: {status: "streaming", delta: "根据你的", content: "", done: false, error: ""}

// 取消
agentOS::cancelAsync(streamObj["requestId"])
```

---

## 总结

| 角色 | 要做的事 |
|------|---------|
| **插件（已完成）** | `askAsync` 返回 `{__stream__, requestId, status, sseUrl?, token?}`；`poll` 返回增量 delta；SSE 服务端推送 |
| **前端** | 检测 `__stream__` → 有 `sseUrl` 用 EventSource 连接，无则降级 poll → 增量渲染 |
| **后端 / 运维** | 编译时加 `-DAGENTOS_ENABLE_SSE=ON`，确保 SSE 端口可访问 |
