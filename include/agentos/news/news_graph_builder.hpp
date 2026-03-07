#pragma once
// ============================================================
// AgentOS :: News Processing Module - News Graph Builder
// 图构建器：将资讯、实体、关系转换为本体论图表示
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/memory/graph_memory.hpp>
#include "news_parser.hpp"
#include "entity_extractor.hpp"
#include "relation_extractor.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <chrono>

namespace agentos::news {

// ─────────────────────────────────────────────────────────────
// § 4.1  NewsGraphConfig — 图构建配置
// ─────────────────────────────────────────────────────────────

struct NewsGraphConfig {
    // 实体节点配置
    bool create_news_nodes{true};              // 是否为每条资讯创建节点
    bool create_entity_nodes{true};            // 是否为实体创建节点
    bool create_concept_nodes{true};          // 是否为概念创建节点
    
    // 关系边配置
    bool create_mention_edges{true};          // 创建提及关系
    bool create_relation_edges{true};          // 创建语义关系
    bool create_temporal_edges{true};         // 创建时序关系
    bool create_similarity_edges{false};      // 创建相似性关系
    
    // 权重配置
    double default_entity_weight{1.0};        // 默认实体权重
    double default_relation_weight{1.0};      // 默认关系权重
    double mention_weight_decay{0.9};          // 提及权重衰减因子
    
    // 时间配置
    std::chrono::hours relation_ttl{720};     // 关系生存时间（30天）
    std::chrono::hours news_ttl{168};         // 资讯生存时间（7天）
    
    // 过滤配置
    double min_entity_confidence{0.6};        // 最小实体置信度
    double min_relation_confidence{0.6};      // 最小关系置信度
    size_t max_entities_per_news{50};         // 每条资讯最大实体数
    size_t max_relations_per_news{20};        // 每条资讯最大关系数
};

// ─────────────────────────────────────────────────────────────
// § 4.2  NewsGraphBuilder — 图构建器
// ─────────────────────────────────────────────────────────────

class NewsGraphBuilder {
public:
    explicit NewsGraphBuilder(std::shared_ptr<agentos::memory::IGraphMemory> graph_memory,
                             const NewsGraphConfig& config = NewsGraphConfig{});
    
    // 构建单条资讯的图
    Result<bool> build_news_graph(const NewsArticle& article);
    
    // 批量构建资讯图
    Result<size_t> build_news_graph_batch(const std::vector<NewsArticle>& articles);
    
    // 更新现有图（增量更新）
    Result<bool> update_news_graph(const NewsArticle& article);
    
    // 清理过期节点和边
    Result<size_t> cleanup_expired_nodes();
    
    // 获取构建统计信息
    struct BuildStats {
        size_t total_news_processed{0};
        size_t total_entities_created{0};
        size_t total_relations_created{0};
        size_t total_nodes{0};
        size_t total_edges{0};
        std::chrono::system_clock::time_point last_update;
    };
    
    BuildStats get_build_stats() const;
    
    // 配置更新
    void update_config(const NewsGraphConfig& config);
    const NewsGraphConfig& get_config() const;
    
private:
    std::shared_ptr<agentos::memory::IGraphMemory> graph_memory_;
    NewsGraphConfig config_;
    BuildStats stats_;
    mutable std::mutex stats_mutex_;
    
    // 节点创建
    Result<std::string> create_or_update_news_node(const NewsArticle& article);
    Result<std::string> create_or_update_entity_node(const Entity& entity);
    Result<std::string> create_or_update_concept_node(const std::string& concept, 
                                                       const NewsArticle& article);
    
    // 边创建
    Result<bool> create_mention_edges(const std::string& news_id, 
                                     const std::vector<Entity>& entities);
    Result<bool> create_relation_edges(const std::string& news_id,
                                     const std::vector<Relation>& relations);
    Result<bool> create_temporal_edges(const std::string& news_id,
                                     const NewsArticle& article);
    Result<bool> create_similarity_edges(const std::vector<Entity>& entities);
    
    // 节点ID生成
    std::string generate_news_node_id(const NewsArticle& article);
    std::string generate_entity_node_id(const Entity& entity);
    std::string generate_concept_node_id(const std::string& concept, const std::string& source);
    
    // 权重计算
    double calculate_entity_weight(const Entity& entity, const NewsArticle& article);
    double calculate_relation_weight(const Relation& relation, const NewsArticle& article);
    double calculate_mention_weight(const Entity& entity, const NewsArticle& article);
    
    // 时间处理
    uint64_t get_current_timestamp() const;
    uint64_t get_expiry_timestamp(const std::chrono::system_clock::time_point& created_at,
                                 std::chrono::hours ttl) const;
    
    // 验证和过滤
    bool should_include_entity(const Entity& entity) const;
    bool should_include_relation(const Relation& relation) const;
    
    // 图查询辅助
    Result<bool> node_exists(const std::string& node_id);
    Result<std::vector<agentos::memory::GraphEdge>> get_node_edges(const std::string& node_id);
};

// ─────────────────────────────────────────────────────────────
// § 4.3  NewsGraphQuery — 图查询接口
// ─────────────────────────────────────────────────────────────

class NewsGraphQuery {
public:
    explicit NewsGraphQuery(std::shared_ptr<agentos::memory::IGraphMemory> graph_memory);
    
    // 查询实体相关资讯
    Result<std::vector<NewsArticle>> query_entity_news(const std::string& entity_name, 
                                                       int limit = 10);
    
    // 查询实体间关系
    Result<std::vector<Relation>> query_entity_relations(const std::string& entity1,
                                                        const std::string& entity2);
    
    // 查询时间范围内的资讯
    Result<std::vector<NewsArticle>> query_news_by_time_range(
        const std::chrono::system_clock::time_point& start_time,
        const std::chrono::system_clock::time_point& end_time);
    
    // 查询相似实体
    Result<std::vector<std::string>> query_similar_entities(const std::string& entity_name,
                                                           int k_hop = 2);
    
    // 查询热点实体（按连接度排序）
    Result<std::vector<std::pair<std::string, size_t>>> query_hot_entities(int top_k = 20);
    
    // 查询实体演化路径
    Result<std::vector<std::pair<std::string, std::chrono::system_clock::time_point>>> 
    query_entity_evolution(const std::string& entity_name);
    
    // K-hop子图查询
    Result<agentos::memory::Subgraph> query_subgraph(const std::string& start_entity,
                                                     int k_hop = 2);
    
private:
    std::shared_ptr<agentos::memory::IGraphMemory> graph_memory_;
    
    // 辅助方法
    NewsArticle reconstruct_news_from_node(const agentos::memory::GraphNode& node);
    Relation reconstruct_relation_from_edge(const agentos::memory::GraphEdge& edge);
    std::chrono::system_clock::time_point timestamp_to_time(uint64_t timestamp);
};

// ─────────────────────────────────────────────────────────────
// § 4.4  NewsGraphAnalyzer — 图分析器
// ─────────────────────────────────────────────────────────────

class NewsGraphAnalyzer {
public:
    explicit NewsGraphAnalyzer(std::shared_ptr<agentos::memory::IGraphMemory> graph_memory);
    
    // 图统计信息
    struct GraphStats {
        size_t total_nodes{0};
        size_t total_edges{0};
        std::unordered_map<std::string, size_t> node_type_distribution;
        std::unordered_map<std::string, size_t> edge_type_distribution;
        double average_degree{0.0};
        size_t connected_components{0};
    };
    
    Result<GraphStats> analyze_graph_stats();
    
    // 实体重要性分析
    Result<std::vector<std::pair<std::string, double>>> analyze_entity_importance();
    
    // 社区发现
    Result<std::vector<std::vector<std::string>>> detect_communities();
    
    // 影响力传播分析
    Result<std::unordered_map<std::string, double>> analyze_influence_propagation(
        const std::string& source_entity, int max_steps = 3);
    
    // 趋势分析
    struct TrendAnalysis {
        std::vector<std::pair<std::string, double>> rising_entities;
        std::vector<std::pair<std::string, double>> declining_entities;
        std::vector<std::pair<std::string, double>> stable_entities;
    };
    
    Result<TrendAnalysis> analyze_trends(
        const std::chrono::system_clock::time_point& start_time,
        const std::chrono::system_clock::time_point& end_time);
    
private:
    std::shared_ptr<agentos::memory::IGraphMemory> graph_memory_;
    
    // 算法实现
    double calculate_pagerank(const std::string& entity_id, int iterations = 100);
    std::vector<std::string> get_neighbors(const std::string& entity_id);
    double calculate_betweenness_centrality(const std::string& entity_id);
};

// ─────────────────────────────────────────────────────────────
// § 4.5  NewsGraphManager — 统一管理器
// ─────────────────────────────────────────────────────────────

class NewsGraphManager {
public:
    explicit NewsGraphManager(std::shared_ptr<agentos::memory::IGraphMemory> graph_memory,
                             const NewsGraphConfig& config = NewsGraphConfig{});
    
    // 完整的资讯处理流水线
    Result<bool> process_news_article(const NewsArticle& article);
    Result<size_t> process_news_batch(const std::vector<NewsArticle>& articles);
    
    // 获取各个组件
    NewsGraphBuilder& get_builder() { return *builder_; }
    NewsGraphQuery& get_query() { return *query_; }
    NewsGraphAnalyzer& get_analyzer() { return *analyzer_; }
    
    // 配置管理
    void update_config(const NewsGraphConfig& config);
    const NewsGraphConfig& get_config() const;
    
    // 批量操作
    Result<bool> rebuild_graph_from_news(const std::vector<NewsArticle>& articles);
    Result<size_t> cleanup_expired_data();
    
    // 导出/导入
    Result<bool> export_graph(const std::string& file_path);
    Result<bool> import_graph(const std::string& file_path);
    
private:
    std::unique_ptr<NewsGraphBuilder> builder_;
    std::unique_ptr<NewsGraphQuery> query_;
    std::unique_ptr<NewsGraphAnalyzer> analyzer_;
    
    std::unique_ptr<IEntityExtractor> entity_extractor_;
    std::unique_ptr<IRelationExtractor> relation_extractor_;
    
    // 处理流水线
    Result<std::vector<Entity>> extract_entities(const NewsArticle& article);
    Result<std::vector<Relation>> extract_relations(const NewsArticle& article,
                                                    const std::vector<Entity>& entities);
};

} // namespace agentos::news
