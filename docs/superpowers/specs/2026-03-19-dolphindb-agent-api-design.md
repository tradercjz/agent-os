# DolphinDB Agent API — 用户中心设计

**Date:** 2026-03-19
**Status:** Approved

## 设计原则

以用户使用场景驱动，不暴露内部概念（Hooks/Skills/Session/Worktree）。
用户心智模型：**创建 Agent → 执行任务 → 管理生命周期**。

## 核心 API（8 个函数）

| 函数 | 用途 | 返回 |
|------|------|------|
| `createAgent(name, ...)` | 创建并配置 agent | handle (LONG) |
| `ask(agent, question)` | 单轮/多轮问答 | STRING |
| `askStream(agent, question, callback)` | 流式问答 | STRING |
| `run(agent, task, ...)` | 结构化执行 | DICT |
| `save(agent)` | 保存会话 | STRING (sessionId) |
| `resume(sessionId)` | 恢复会话 | handle (LONG) |
| `destroy(agent)` | 销毁 agent | VOID |
| `info(agent)` | 查看 agent 状态 | DICT |

## 参数规范

所有函数同时支持位置参数和命名参数 (key=value)。

### createAgent

```
createAgent(name, prompt="", tools=[], skills=[], blockTools=[],
            contextLimit=8192, isolation="thread", securityRole="standard")
```

| 参数 | 类型 | 必选 | 默认值 | 说明 |
|------|------|------|--------|------|
| name | STRING | 是 | — | Agent 名称 |
| prompt | STRING | 否 | "" | System prompt / 角色设定 |
| tools | STRING VECTOR | 否 | [] | 允许使用的工具列表 |
| skills | STRING VECTOR | 否 | [] | 激活的技能列表 |
| blockTools | STRING VECTOR | 否 | [] | 禁用的工具列表 |
| contextLimit | INT | 否 | 8192 | 上下文窗口 token 上限 |
| isolation | STRING | 否 | "thread" | "thread" or "worktree" |
| securityRole | STRING | 否 | "standard" | RBAC 角色 |

### ask / askStream

```
ask(agent, question, prompt="")
askStream(agent, question, prompt="", callback=NULL)
```

### run

```
run(agent, task, prompt="", timeout=60000, contextLimit=0)
```

返回 DICT: `success`, `output`, `error`, `durationMs`, `tokensUsed`

### save / resume

```
save(agent, metadata="") → sessionId STRING
resume(sessionId) → agent handle LONG
```

### info

```
info(agent) → DICT
```

返回: name, prompt, tools, skills, blockTools, workDir, messageCount, isolation

## 辅助 API

| 函数 | 用途 |
|------|------|
| `registerTool(name, desc, handler, ...)` | 注册 DDB 函数为工具 |
| `registerSkill(name, keywords, ...)` | 注册技能 |
| `blockTool(agent, tool, reason)` | 运行时禁用 |
| `unblockTool(agent, tool)` | 运行时启用 |
| `activateSkill(agent, skill)` | 运行时激活技能 |
| `deactivateSkill(agent, skill)` | 运行时去激活 |
| `sessions()` | 列出所有会话 → TABLE |

## PluginAgentOS.txt 新增条目

```
agentOSCreateAgent2,createAgent2,system,1,8,0
agentOSAsk2,ask2,system,2,3,0
agentOSAskStream2,askStream2,system,2,4,0
agentOSRun,run,system,2,5,0
agentOSSave,save,system,1,2,0
agentOSResume,resume,system,1,1,0
agentOSDestroy,destroy,system,1,1,0
agentOSInfo,info,system,1,1,0
agentOSRegisterSkill,registerSkill,system,2,4,0
agentOSBlockTool,blockTool,system,2,3,0
agentOSUnblockTool,unblockTool,system,2,2,0
agentOSActivateSkill,activateSkill,system,2,2,0
agentOSDeactivateSkill,deactivateSkill,system,2,2,0
agentOSSessions,sessions,system,0,0,0
```

## 实现策略

新函数命名为 `*2` 后缀（如 `createAgent2`），保持与现有 API 向后兼容。
现有的 `createAgent`、`ask` 等继续工作，不受影响。

内部实现复用现有 PluginRuntime 单例，新增：
- `SkillRegistry` 实例（进程级单例）
- `createAgent2` 解析原生参数 → 构造 AgentConfig + 注册 hooks/skills
- `run` 使用 HeadlessRunner 模式（timeout + 结构化返回）
- `save`/`resume` 调用 ContextManager 的 session 方法
