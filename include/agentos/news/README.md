# AgentOS News Processing Module

实时资讯数据处理为本体论图表示的完整解决方案。

## 概述

本模块将实时资讯流转换为本体论图表示，与 AgentOS 的三层记忆体系和图存储模块完美集成，支持：

- **多格式资讯解析**：RSS、JSON API、WebSocket等
- **智能实体识别**：公司、人物、地点、事件等
- **语义关系抽取**：投资、收购、合作等关系
- **实时图构建**：增量更新本体论图
- **高级图分析**：影响力分析、社区发现、趋势预测

## 架构设计

```
资讯流 → 解析器 → 实体识别 → 关系抽取 → 图构建 → 三层记忆
  ↓         ↓         ↓         ↓         ↓         ↓
RSS/API   多格式     规则/ML    规则/ML    本体图    L0/L1/L2
```

## 核心组件

### 1. News Parser (`news_parser.hpp`)
- **NewsArticle**：标准化资讯条目
- **INewsParser**：多格式解析器接口
- **RSSParser**：RSS格式解析
- **JSONAPIParser**：JSON API解析
- **NewsFetcher**：资讯获取器
- **NewsParserManager**：统一管理

### 2. Entity Extractor (`entity_extractor.hpp`)
- **Entity**：实体信息结构
- **IEntityExtractor**：实体识别接口
- **RuleBasedEntityExtractor**：基于规则的识别
- **MLBasedEntityExtractor**：基于ML的识别
- **HybridEntityExtractor**：混合识别器
- **EntityLibrary**：实体库管理

### 3. Relation Extractor (`relation_extractor.hpp`)
- **Relation**：关系信息结构
- **IRelationExtractor**：关系抽取接口
- **RuleBasedRelationExtractor**：基于规则的抽取
- **MLBasedRelationExtractor**：基于ML的抽取
- **RelationLibrary**：关系库管理

### 4. News Graph Builder (`news_graph_builder.hpp`)
- **NewsGraphBuilder**：图构建器
- **NewsGraphQuery**：图查询接口
- **NewsGraphAnalyzer**：图分析器
- **NewsGraphManager**：统一管理器

### 5. Realtime Processor (`realtime_news_processor.hpp`)
- **RealtimeNewsProcessor**：实时处理器
- **NewsEventBus**：事件总线
- **NewsMetricsCollector**：指标收集
- **RealtimeProcessorFactory**：工厂类

## 快速开始

### 基础使用

```cpp
#include <agentos/memory/memory.hpp>
#include <agentos/news/realtime_news_processor.hpp>

// 1. 初始化记忆系统
auto memory_system = std::make_shared<memory::MemorySystem>("/tmp/agentos_news");

// 2. 创建实时处理器
auto processor = RealtimeProcessorFactory::create_processor(memory_system);

// 3. 启动处理器
processor->start();

// 4. 添加资讯源
NewsSourceConfig config;
config.name = "财经RSS";
config.type = "rss";
config.url = "https://finance.example.com/rss";
processor->add_news_source(config);

// 5. 手动添加资讯
NewsArticle article;
article.title = "苹果公司收购AI初创公司";
article.content = "苹果公司今日宣布收购...";
processor->add_article(article);
```

### 图查询

```cpp
// 查询实体相关资讯
auto news = graph_manager->get_query().query_entity_news("苹果公司", 10);

// 查询实体关系
auto relations = graph_manager->get_query().query_entity_relations("苹果公司", "NeuralLabs");

// 查询热点实体
auto hot_entities = graph_manager->get_query().query_hot_entities(20);

// K-hop子图查询
auto subgraph = graph_manager->get_query().query_subgraph("微软", 2);
```

### 图分析

```cpp
// 图统计信息
auto stats = graph_manager->get_analyzer().analyze_graph_stats();

// 实体重要性分析
auto importance = graph_manager->get_analyzer().analyze_entity_importance();

// 社区发现
auto communities = graph_manager->get_analyzer().detect_communities();

// 趋势分析
auto trends = graph_manager->get_analyzer().analyze_trends(start_time, end_time);
```

## 配置选项

### 图构建配置

```cpp
NewsGraphConfig config;
config.create_news_nodes = true;      // 创建资讯节点
config.create_entity_nodes = true;    // 创建实体节点
config.create_relation_edges = true;   // 创建关系边
config.min_entity_confidence = 0.6;   // 最小实体置信度
config.min_relation_confidence = 0.6; // 最小关系置信度
```

### 实时处理配置

```cpp
RealtimeConfig config;
config.parser_threads = 2;            // 解析线程数
config.processing_threads = 4;       // 处理线程数
config.enable_batch_processing = true; // 启用批处理
config.batch_size = 10;               // 批处理大小
```

## 实体类型

- **PERSON**：人物
- **COMPANY**：公司/组织
- **LOCATION**：地点
- **PRODUCT**：产品
- **EVENT**：事件
- **CONCEPT**：概念
- **DATE_TIME**：日期时间
- **MONEY**：金额
- **PERCENTAGE**：百分比

## 关系类型

### 公司相关
- **FOUNDED_BY**：由...创立
- **CEO_OF**：担任CEO
- **INVESTED_IN**：投资于
- **ACQUIRED**：收购
- **MERGED_WITH**：合并
- **PARTNERED_WITH**：合作

### 人物相关
- **WORKS_FOR**：工作于
- **MEMBER_OF**：成员
- **GRADUATED_FROM**：毕业于
- **MARRIED_TO**：结婚
- **FRIEND_OF**：朋友

### 产品相关
- **PRODUCED_BY**：由...生产
- **OWNED_BY**：由...拥有
- **SUBSIDIARY_OF**：子公司

### 事件相关
- **PARTICIPATED_IN**：参与
- **CAUSED_BY**：由...引起
- **RESULTED_IN**：导致

### 地理相关
- **LOCATED_IN**：位于
- **BORN_IN**：出生于

## 性能特性

- **高吞吐量**：支持每秒处理数百条资讯
- **低延迟**：实时处理，秒级响应
- **可扩展**：多线程架构，水平扩展
- **容错性**：错误隔离，自动恢复
- **持久化**：数据持久化存储

## 集成示例

完整的使用示例请参考 `examples/news_processing_example.cpp`，包含：

1. **基础处理流程**：单条资讯处理
2. **批量处理**：多线程批量处理
3. **图分析**：统计信息、重要性分析
4. **自定义提取**：自定义实体和关系模式

## 扩展性

### 自定义解析器

```cpp
class CustomParser : public INewsParser {
public:
    Result<std::vector<NewsArticle>> parse(const std::string& raw_data) override {
        // 自定义解析逻辑
    }
};

// 注册自定义解析器
NewsParserFactory::register_parser("custom", []() {
    return std::make_unique<CustomParser>();
});
```

### 自定义实体提取

```cpp
auto extractor = std::make_unique<RuleBasedEntityExtractor>();

// 添加自定义模式
extractor->add_pattern(EntityType::COMPANY, 
    R"((\w+(?:科技|公司|集团)))", 0.9);
```

### 自定义关系抽取

```cpp
auto relation_extractor = std::make_unique<RuleBasedRelationExtractor>();

// 添加自定义关系模式
relation_extractor->add_pattern(RelationType::ACQUIRED,
    R"((\w+公司)收购(\w+公司))", 0.9);
```

## 最佳实践

1. **合理配置线程数**：根据硬件资源调整
2. **设置合适的置信度阈值**：平衡准确率和召回率
3. **定期清理过期数据**：避免图膨胀
4. **使用批处理**：提高处理效率
5. **监控处理指标**：及时发现性能问题

## 依赖项

- AgentOS Core System
- AgentOS Memory System
- AgentOS Graph Memory
- C++20 或更高版本
- 线程库支持

## 许可证

本模块遵循 AgentOS 项目的许可证条款。
