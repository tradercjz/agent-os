# 单机高性能本体图计算存储框架设计文档

## 1. 项目目标

**核心目标：**
- 单机嵌入式图数据库内核
- 高性能、大规模图检索（支持 10^9 节点，100亿边）
- 支持时间图（Temporal Graph）
- 支持规则引擎（Rule Engine）
- 可扩展向量检索（GraphRAG 场景）
- C++ 可落地实现

**设计原则：**
- mmap 优先、顺序写
- append-only 写入，禁止频繁随机写
- WAL + Snapshot 保证一致性
- CSR 图存储 + 压缩
- ID 数值化，禁止在线字符串查找
- 多读单写并发模型
- 分层存储：离线构建 + 在线只读

## 2. 总体架构
```text
┌────────────────────────────┐
│       Raw Internet Data    │
└─────────┬──────────────────┘
          │
  [Offline Build Pipeline]
          │
  ┌────────────────────────┐
  │ 1. Ontology Normalize  │
  │ 2. ID Remap (string→ID)│
  │ 3. Edge Sort & Partition│
  │ 4. CSR/Compressed Graph│
  │ 5. Inverted Index Build│
  │ 6. Serialize to Disk   │
  └────────────────────────┘
          │
      Immutable Files
          │
   ┌─────────────────┐
   │  Online Engine  │
   │ (mmap + zero-copy) │
   └─────────────────┘
```

## 3. 数据规模与性能目标
| 项目 | 数量级 | 内存占用估算 |
| --- | --- | --- |
| 节点 (N) | 10亿 | offsets 8GB |
| 边 (M) | 100亿 | edge_data 40~60GB |
| 本体类型 | 1000万 | inverted index ~10GB |
| 实体 | 2000万 | ~1GB |

**查询性能目标：**
- 邻接查询 < 1ms
- 本体检索 < 5ms
- 小规模子图遍历 < 50ms

*前提：只读，数据已排序，mmap 加载。*

## 4. 存储设计
### 4.1 文件结构
```text
/data/
  graph.offset       # CSR offsets
  graph.edges        # edge_data (delta+varint压缩)
  ontology.index     # 本体倒排索引
  string.dict        # 全局字符串字典
  metadata.json      # schema, counts, version
  wal.log            # 仅在构建阶段使用
  snapshot.meta
```

### 4.2 CSR 图
- `offsets[N+1]`: 指向 edges 起始位置
- `edges[M]`: 压缩邻接列表 (delta + varint)
- 支持 O(1) 邻接访问，顺序扫描度列表

### 4.3 Temporal Graph
- `edge_data` 按 start_ts 排序
- 查询支持时间窗口：
  ```cpp
  if (edge.start_ts <= query_ts && edge.end_ts >= query_ts)
  ```

### 4.4 本体倒排索引
- `type_id` → posting list
- `property_id` → posting list
- mmap 顺序扫描
- 查询复杂度 O(logN + result_size)

### 4.5 字符串映射
- 所有字符串离线映射为 uint64 ID
- 在线阶段只操作 ID
- 字符串查找一次性 hash 表映射

## 5. 内存管理
- mmap 所有数据文件
- page aligned，支持 madvise(HUGE_PAGE)
- 扩容通过 segment 文件 + ftruncate + remap
- 禁止 realloc 复制

## 6. 并发模型
- 多线程查询，无锁
- 只读 CSR + Inverted Index
- 写操作仅在构建阶段，完成后 immutable

## 7. 数据构建 Pipeline
- 分块读取原始数据
- 实体抽取 & 关系抽取
- ID 映射 (string → uint64)
- 分块排序 & 外部归并
- 构建 CSR + 倒排索引
- 序列化到磁盘，生成 snapshot

**构建特点：**
- 外部排序
- 并行 ID 分配
- 批量写入 CSR
- 压缩邻接列表 (delta+varint)

## 8. 查询与子图抽取
### 8.1 邻接查询
```cpp
std::vector<EntityID> neighbors(EntityID id, uint64_t ts);
```
- O(1) 找邻接起点
- O(degree) 扫描边

### 8.2 时间过滤遍历
```cpp
std::vector<EntityID> temporal_k_hop(EntityID start, int k, uint64_t ts);
```

### 8.3 子图提取
```cpp
Subgraph extract_subgraph(EntityID start, int k, uint64_t ts);
```
- BFS 或 DFS
- 遍历顺序内存扫描
- 局部子图 < 1M 节点毫秒级

## 9. 写入策略
- append-only
- WAL 顺序写入，fsync
- CSR 在构建阶段批量构建
- 在线阶段禁止写操作

## 10. 压缩与优化
- 邻接列表 delta + varint
- edge_data 顺序扫描
- offsets 连续内存
- 页面预读 + HugePage
- NUMA-aware 分区

## 11. 扩展接口
### 向量索引
```cpp
class VectorIndex {
public:
    virtual void add(EntityID id, float* vec) = 0;
    virtual std::vector<EntityID> search(float* q) = 0;
};
```
- GraphCore 不依赖向量实现
- 支持 GraphRAG 业务

### 规则引擎
```text
RULE risk_propagation:
    IF guarantee(A, B) AND default(B)
    THEN risk(A) += 1.0
```
- 只读图，不修改结构
- DSL → 函数对象 → 执行

## 12. 单机可扩展性
- CSR + mmap + 压缩 → 可支撑 10^9 节点 100亿边
- 超过 500亿边 → 分区/分片策略
- 高并发查询 → 线程池 + immutable data

## 13. 阶段开发路线
| 阶段 | 内容 |
| --- | --- |
| Phase 1 | 单机 Immutable Graph Engine，CSR + mmap |
| Phase 2 | Temporal graph, snapshot, rule engine |
| Phase 3 | 向量检索接口，子图导出 |
| Phase 4 | 分区/分布式扩展 |

## 14. 工程目录建议
```text
graph_engine/
    core/           # CSR, temporal graph
    storage/        # mmap, WAL, snapshot
    compute/        # BFS, DFS, K-hop
    schema/         # EntityType, RelationType, RuleEngine
    engine/         # 高层接口
    utils/          # 内存、压缩、hash
    tests/
    examples/
```

## 15. 工程参考对标
- Neo4j
- TigerGraph
- RocksDB

**定位：**
- 单机嵌入式高性能，专注金融/互联网大数据场景

## 16. 注意事项
- 只读数据保证 CSR 高效
- 高度节点（super-node）可单独二级索引
- 动态频繁写入需用 LSM 或分区策略
- 实体 ID 连续、避免随机写
- 构建阶段必须排序、压缩、批量
