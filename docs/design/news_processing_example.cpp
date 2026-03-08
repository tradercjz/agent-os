// ============================================================
// AgentOS :: News Processing Example
// 实时资讯数据处理为本体论图表示的完整示例
// ============================================================
#include <agentos/memory/memory.hpp>
#include <agentos/news/news_parser.hpp>
#include <agentos/news/entity_extractor.hpp>
#include <agentos/news/relation_extractor.hpp>
#include <agentos/news/news_graph_builder.hpp>
#include <agentos/news/realtime_news_processor.hpp>
#include <iostream>
#include <thread>
#include <chrono>

using namespace agentos;
using namespace agentos::news;

// 示例：创建模拟资讯数据
NewsArticle create_sample_article(const std::string& title, const std::string& content) {
    NewsArticle article;
    article.id = "news_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    article.title = title;
    article.content = content;
    article.source = "示例财经网";
    article.category = "财经";
    article.published_at = std::chrono::system_clock::now();
    article.crawled_at = std::chrono::system_clock::now();
    article.importance_score = 0.7;
    article.sentiment_score = 0.1;
    return article;
}

// 示例1：基础的单条资讯处理流程
void example_basic_processing() {
    std::cout << "=== 示例1：基础资讯处理流程 ===" << std::endl;
    
    // 1. 初始化记忆系统
    auto memory_system = std::make_shared<memory::MemorySystem>("/tmp/agentos_news_example");
    
    // 2. 创建图管理器
    NewsGraphConfig config;
    config.create_news_nodes = true;
    config.create_entity_nodes = true;
    config.create_relation_edges = true;
    
    auto graph_manager = std::make_unique<NewsGraphManager>(
        memory_system->graph().shared_from_this(), config);
    
    // 3. 创建示例资讯
    auto article = create_sample_article(
        "苹果公司收购AI初创公司",
        "苹果公司今日宣布收购专注于自然语言处理的AI初创公司NeuralLabs。"
        "此次收购金额达5亿美元，NeuralLabs的CEO张明将加入苹果AI团队。"
        "苹果CEO蒂姆·库克表示，这次收购将加速苹果在AI领域的发展。"
    );
    
    // 4. 处理资讯
    auto result = graph_manager->process_news_article(article);
    if (result) {
        std::cout << "✓ 资讯处理成功" << std::endl;
        
        // 5. 查询结果
        auto query_result = graph_manager->get_query().query_entity_news("苹果公司", 5);
        if (query_result) {
            std::cout << "找到相关资讯: " << query_result->size() << " 条" << std::endl;
            for (const auto& news : *query_result) {
                std::cout << "  - " << news.title << std::endl;
            }
        }
        
        // 6. 查询实体关系
        auto relations = graph_manager->get_query().query_entity_relations("苹果公司", "NeuralLabs");
        if (relations) {
            std::cout << "实体关系: " << relations->size() << " 个" << std::endl;
            for (const auto& rel : *relations) {
                std::cout << "  - " << relation_type_to_string(rel.type) << std::endl;
            }
        }
    } else {
        std::cout << "✗ 资讯处理失败: " << result.error() << std::endl;
    }
    
    std::cout << std::endl;
}

// 示例2：批量处理和实时更新
void example_batch_processing() {
    std::cout << "=== 示例2：批量处理和实时更新 ===" << std::endl;
    
    // 1. 初始化系统
    auto memory_system = std::make_shared<memory::MemorySystem>("/tmp/agentos_news_batch");
    
    // 2. 创建实时处理器
    RealtimeConfig config;
    config.parser_threads = 2;
    config.processing_threads = 4;
    config.enable_batch_processing = true;
    config.batch_size = 5;
    
    auto processor = RealtimeProcessorFactory::create_processor(memory_system, config);
    
    // 3. 设置事件回调
    processor->set_event_callback([](const NewsEvent& event) {
        switch (event.type) {
            case NewsEventType::ARTICLE_PROCESSED:
                std::cout << "📰 资讯处理完成" << std::endl;
                break;
            case NewsEventType::GRAPH_UPDATED:
                std::cout << "🔗 图更新完成" << std::endl;
                break;
            case NewsEventType::ERROR_OCCURRED:
                std::cout << "❌ 处理错误: " << event.data << std::endl;
                break;
            default:
                break;
        }
    });
    
    // 4. 启动处理器
    auto start_result = processor->start();
    if (start_result) {
        std::cout << "✓ 实时处理器启动成功" << std::endl;
        
        // 5. 添加资讯源
        NewsSourceConfig rss_config;
        rss_config.name = "财经RSS";
        rss_config.type = "rss";
        rss_config.url = "https://finance.example.com/rss";
        rss_config.update_interval_seconds = 60;
        
        auto source_result = processor->add_news_source(rss_config);
        if (source_result) {
            std::cout << "✓ 资讯源添加成功" << std::endl;
        }
        
        // 6. 批量添加资讯
        std::vector<NewsArticle> articles;
        articles.push_back(create_sample_article(
            "腾讯发布最新财报",
            "腾讯发布2024年Q3财报，营收1546亿元，同比增长8%。"
            "游戏业务收入758亿元，金融科技及企业服务收入520亿元。"
        ));
        
        articles.push_back(create_sample_article(
            "阿里巴巴云计算业务增长",
            "阿里巴巴云计算业务收入增长21%，达到298亿元。"
            "阿里云CEO张勇表示，AI需求推动云业务快速增长。"
        ));
        
        articles.push_back(create_sample_article(
            "字节跳动推出新AI产品",
            "字节跳动推出全新AI助手产品，集成抖音和今日头条的数据。"
            "该产品由字节跳动AI实验室负责人李航带队开发。"
        ));
        
        auto batch_result = processor->add_articles(articles);
        if (batch_result) {
            std::cout << "✓ 批量资讯添加成功: " << articles.size() << " 条" << std::endl;
        }
        
        // 7. 等待处理完成
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // 8. 查看统计信息
        auto stats = processor->get_stats();
        std::cout << "处理统计:" << std::endl;
        std::cout << "  总处理文章: " << stats.total_articles_processed.load() << std::endl;
        std::cout << "  处理速度: " << stats.articles_per_second.load() << " 篇/秒" << std::endl;
        std::cout << "  提取实体: " << stats.entities_extracted.load() << std::endl;
        std::cout << "  提取关系: " << stats.relations_extracted.load() << std::endl;
        
        // 9. 停止处理器
        processor->stop();
        std::cout << "✓ 实时处理器已停止" << std::endl;
    }
    
    std::cout << std::endl;
}

// 示例3：图分析和查询
void example_graph_analysis() {
    std::cout << "=== 示例3：图分析和查询 ===" << std::endl;
    
    // 1. 初始化系统
    auto memory_system = std::make_shared<memory::MemorySystem>("/tmp/agentos_news_analysis");
    
    NewsGraphConfig config;
    auto graph_manager = std::make_unique<NewsGraphManager>(
        memory_system->graph().shared_from_this(), config);
    
    // 2. 添加更多测试数据
    std::vector<NewsArticle> test_articles;
    
    test_articles.push_back(create_sample_article(
        "微软收购动视暴雪",
        "微软以687亿美元收购游戏巨头动视暴雪。"
        "动视暴雪CEO Bobby Kotick将加入微软游戏部门。"
        "微软CEO萨提亚·纳德拉表示这是游戏业务的重要里程碑。"
    ));
    
    test_articles.push_back(create_sample_article(
        "索尼与微软合作",
        "索尼和微软宣布在云游戏领域达成战略合作。"
        "索尼PlayStation将集成微软Azure云服务。"
        "索尼CEO吉田宪一郎对合作表示期待。"
    ));
    
    test_articles.push_back(create_sample_article(
        "任天堂发布新主机",
        "任天堂发布下一代游戏主机Switch 2。"
        "新主机支持4K分辨率和更好的性能。"
        "任天堂社长古川俊太郎表示新主机将延续创新理念。"
    ));
    
    // 3. 批量处理
    auto batch_result = graph_manager->process_news_batch(test_articles);
    if (batch_result) {
        std::cout << "✓ 批量处理成功: " << *batch_result << " 条资讯" << std::endl;
    }
    
    // 4. 图分析
    auto& analyzer = graph_manager->get_analyzer();
    
    // 4.1 图统计信息
    auto graph_stats = analyzer.analyze_graph_stats();
    if (graph_stats) {
        std::cout << "图统计信息:" << std::endl;
        std::cout << "  总节点数: " << graph_stats->total_nodes << std::endl;
        std::cout << "  总边数: " << graph_stats->total_edges << std::endl;
        std::cout << "  平均度: " << graph_stats->average_degree << std::endl;
        std::cout << "  连通分量: " << graph_stats->connected_components << std::endl;
        
        std::cout << "节点类型分布:" << std::endl;
        for (const auto& [type, count] : graph_stats->node_type_distribution) {
            std::cout << "  " << type << ": " << count << std::endl;
        }
    }
    
    // 4.2 实体重要性分析
    auto importance = analyzer.analyze_entity_importance();
    if (importance) {
        std::cout << "重要实体 (Top 10):" << std::endl;
        for (size_t i = 0; i < std::min(size_t(10), importance->size()); ++i) {
            std::cout << "  " << (i+1) << ". " << (*importance)[i].first 
                     << " (分数: " << (*importance)[i].second << ")" << std::endl;
        }
    }
    
    // 4.3 热点实体查询
    auto hot_entities = graph_manager->get_query().query_hot_entities(5);
    if (hot_entities) {
        std::cout << "热点实体 (按连接度):" << std::endl;
        for (const auto& [entity, connections] : *hot_entities) {
            std::cout << "  " << entity << ": " << connections << " 个连接" << std::endl;
        }
    }
    
    // 4.4 K-hop子图查询
    auto subgraph = graph_manager->get_query().query_subgraph("微软", 2);
    if (subgraph) {
        std::cout << "微软的2-hop子图:" << std::endl;
        std::cout << "  包含节点: " << subgraph->nodes.size() << std::endl;
        std::cout << "  包含边: " << subgraph->edges.size() << std::endl;
        
        // 显示相关实体
        std::cout << "  相关实体:" << std::endl;
        for (const auto& node : subgraph->nodes) {
            if (node.id != "微软" && node.type == "Entity") {
                std::cout << "    - " << node.id << std::endl;
            }
        }
    }
    
    std::cout << std::endl;
}

// 示例4：自定义实体和关系提取
void example_custom_extraction() {
    std::cout << "=== 示例4：自定义实体和关系提取 ===" << std::endl;
    
    // 1. 创建自定义实体提取器
    auto entity_extractor = EntityExtractorFactory::create_extractor(
        EntityExtractorFactory::ExtractorType::RULE_BASED);
    
    // 添加自定义模式
    auto rule_extractor = dynamic_cast<RuleBasedEntityExtractor*>(entity_extractor.get());
    if (rule_extractor) {
        // 添加公司名称模式
        rule_extractor->add_pattern(EntityType::COMPANY, R"((\w+(?:科技|公司|集团|网络|数字|智能)))", 0.9);
        
        // 添加人名模式
        rule_extractor->add_pattern(EntityType::PERSON, R"(([\u4e00-\u9fff]{2,4}(?:先生|女士|CEO|总裁|总监)))", 0.8);
        
        // 加载实体词典
        // rule_extractor->load_entity_dictionary("entities.dict");
    }
    
    // 2. 创建自定义关系提取器
    auto relation_extractor = RelationExtractorFactory::create_extractor(
        RelationExtractorFactory::ExtractorType::RULE_BASED);
    
    // 添加自定义关系模式
    auto rule_relation_extractor = dynamic_cast<RuleBasedRelationExtractor*>(relation_extractor.get());
    if (rule_relation_extractor) {
        // 收购关系模式
        rule_relation_extractor->add_pattern(RelationType::ACQUIRED, 
            R"((\w+(?:公司|集团)?)收购(\w+(?:公司|集团)?))", 0.9);
        
        // 投资关系模式
        rule_relation_extractor->add_pattern(RelationType::INVESTED_IN,
            R"((\w+(?:公司|集团)?)投资(\w+(?:公司|集团)?))", 0.9);
    }
    
    // 3. 测试提取
    std::string test_text = "百度公司收购了小红书公司，腾讯投资了京东集团。"
                           "百度CEO李彦宏表示这次收购意义重大。";
    
    // 实体提取
    auto entities = entity_extractor->extract(test_text);
    if (entities) {
        std::cout << "提取的实体:" << std::endl;
        for (const auto& entity : *entities) {
            std::cout << "  - " << entity.text << " (" 
                     << entity_type_to_string(entity.type) << ")" << std::endl;
        }
    }
    
    // 关系提取
    auto relations = relation_extractor->extract_relations(test_text, *entities);
    if (relations) {
        std::cout << "提取的关系:" << std::endl;
        for (const auto& relation : *relations) {
            std::cout << "  - " << relation.subject.text << " " 
                     << relation_type_to_string(relation.type) << " "
                     << relation.object.text << std::endl;
        }
    }
    
    std::cout << std::endl;
}

int main() {
    std::cout << "AgentOS 资讯处理系统示例" << std::endl;
    std::cout << "========================" << std::endl;
    std::cout << std::endl;
    
    try {
        // 运行所有示例
        example_basic_processing();
        example_batch_processing();
        example_graph_analysis();
        example_custom_extraction();
        
        std::cout << "✓ 所有示例运行完成!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ 运行错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
