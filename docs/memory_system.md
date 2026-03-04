# AgentOS 产品级记忆系统 (Memory System)

AgentOS 的记忆系统采用了类似 Zep / Mem0 的先进存储架构设计，提供了多层缓存架构、外部标准化大模型向量嵌入（Embedding）支持以及针对多租户场景的精细化读写隔离（Scoping）能力。

## 架构概览

系统采用 L0 到 L2 的三级存储架构，利用 `MemorySystem` 统筹：

1. **L0: WorkingMemory (工作记忆)**
   - **特点**：基于内存的最快访问层，容量小（默认 32 条），以 LRU 策略进行淘汰。
   - **用途**：记录 Agent 当前正在高频交互的最核心上下文。
2. **L1: ShortTermMemory (短期记忆)**
   - **特点**：基于内存，容量中等（默认 512 条），采用基于重要性与访问频率的综合评分公式来淘汰记忆。
   - **用途**：保存当前 Session 内的连贯对话，或者近期的相关信息。
3. **L2: LongTermMemory (长期记忆)**
   - **特点**：基于磁盘持久化，可以无限扩张。
   - **用途**：用于存储长期重要或者具有高泛化价值的语义知识点。
   - **机制**：通过文本 `.mem` 与二进制向量池 `.vec` 平行持久化。

当记忆的**重要性评分 (Importance) > 0.7** 且符合巩固（Consolidation）条件时，底层会自动落盘为长期记忆。

## 核心特性

### 1. 外部大模型向量嵌入 (External Embeddings)
此版本的记忆系统去除了本地低效的 TF-IDF （词频向量）计算，通过 `LLMKernel::embed()` 直接挂载接入远端模型（例如 OpenAI `text-embedding-3-small`）。
- **`EmbeddingRequest` 和 `EmbeddingResponse`** 提供了规范化的请求与解析接口。
- 返回的 1536 维等高维 `std::vector<float>` 将成为精确的向量点乘（Cosine Similarity）依据。
- 检索时通过相似度矩阵与记忆原生的重要性分数进行加权排行（Top-K）。

### 2. 作用域隔离与多租户管理 (Scoping)
系统通过 `MemoryFilter` 抽象原生支持多场景隔离，可轻松过滤特定租户的数据。
支持的作用域维度：
- `user_id`: 绑定该条记忆关联的具体用户
- `agent_id`: 生成该条记忆的智能体
- `session_id`: 对话关联的回话标识符
- `type`: 记忆分类，主要分为 `episodic` (情景记忆) 与 `semantic` (语义记忆)

### 3. 多模态操作接口 (Multi-Modal Memory API)
上层提供了高阶语义方法包装写入：
- **`add_episodic`**: 添加情景对话记录，将信息固化在特定语境中（包含 Session ID、User ID 等），方便回放。
- **`add_semantic`**: 提取的泛化语义或事实（仅属于 User 层级），能够跨越多个对话窗口对同一个用户提供增强上下文。

## 示例用法

```cpp
#include "agentos/agentos.hpp"

using namespace agentos;

// 1. 初始化带有 LTM 持久化落盘的系统
memory::MemorySystem mem("/tmp/agent_knowledge_base");

// 2. 提供模拟或真实的 Embeddings 向量
std::vector<float> fake_emb(1536, 0.1f); 

// 3. 写入不同作用域（Scope）下的记忆
mem.add_episodic("用户想通过 Python 构建爬虫系统", fake_emb, "user_001", "session_abc", 0.8f);
mem.add_semantic("用户的母语是中文", fake_emb, "user_001", 0.95f);

// 4. 用户重新登录开启新会话 session_xyz，跨 Session 获取用户的语言习惯
memory::MemoryFilter filter;
filter.user_id = "user_001";
auto results = mem.recall(fake_emb, filter, 3); // 会过滤出符合条件的记忆
```

## 未来路线图（Phase 3）
后续规划中将进一步集成 SQLite 或原生 GraphDB 以支持：
- 后台异构知识图谱的建立与异步记忆融合（Consolidation）
- 知识图谱节点及边缘（Nodes / Edges）关联关系提取
