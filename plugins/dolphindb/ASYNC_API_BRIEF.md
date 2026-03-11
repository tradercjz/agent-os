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

## 为什么不用 SSE / WebSocket / 额外代理？

插件函数全部运行在 DolphinDB Server 进程内，前端通过 `ddb.run()` 已经和 Server 有稳定连接。在这个连接上做 poll 循环：

- **零部署成本**：不加端口、不加服务、不改网络拓扑
- **复用已有认证**：poll 走的和普通脚本执行一样的 session
- **对旧前端无感**：不识别 `__stream__` 的前端就当普通 dict 展示，不报错
- **延迟可控**：同进程内 poll 开销极低（微秒级内存读取），瓶颈在网络 RTT

如果未来需要进一步优化（比如减少 HTTP 请求次数），可以考虑 DolphinDB 的 streaming publish 机制或在 Web 层升级为 WebSocket。但当前方案已足够满足流式渲染需求。

---

## 完整示例（DolphinDB 脚本）

```dolphindb
// 加载插件
loadPlugin("/path/to/PluginAgentOS.txt")

// 同步调用（阻塞，等完整结果）
result = agentOS::ask("什么是DolphinDB？")

// 异步流式调用
streamObj = agentOS::askAsync("帮我分析最近的交易数据")
// streamObj 是 dict: {__stream__: true, requestId: "...", status: "streaming"}

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
| **插件（已完成）** | `askAsync` 返回 `{__stream__: true, requestId, status}` dict；`poll` 返回增量 delta |
| **前端** | 检测 `__stream__` 标记 → 自动 poll 循环 → 增量拼接渲染 → done 时停止 |
| **后端 / 运维** | 无额外工作 |
