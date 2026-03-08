#pragma once
// ============================================================
// AgentOS :: News Processing Module - Realtime News Processor
// 实时资讯处理器：持续处理资讯流，实时更新本体论图
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/memory/memory.hpp>
#include "news_parser.hpp"
#include "news_graph_builder.hpp"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <functional>
#include <memory>
#include <chrono>

namespace agentos::news {

// ─────────────────────────────────────────────────────────────
// § 5.1  ProcessingStats — 处理统计信息
// ─────────────────────────────────────────────────────────────

struct ProcessingStats {
    std::atomic<size_t> total_articles_processed{0};
    std::atomic<size_t> articles_per_second{0};
    std::atomic<size_t> entities_extracted{0};
    std::atomic<size_t> relations_extracted{0};
    std::atomic<size_t> graph_nodes_created{0};
    std::atomic<size_t> graph_edges_created{0};
    std::atomic<size_t> processing_errors{0};
    std::atomic<size_t> queue_size{0};
    
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point last_update;
    
    ProcessingStats() : start_time(std::chrono::system_clock::now()), 
                       last_update(std::chrono::system_clock::now()) {}
    
    void reset() {
        total_articles_processed = 0;
        articles_per_second = 0;
        entities_extracted = 0;
        relations_extracted = 0;
        graph_nodes_created = 0;
        graph_edges_created = 0;
        processing_errors = 0;
        queue_size = 0;
        start_time = std::chrono::system_clock::now();
        last_update = std::chrono::system_clock::now();
    }
    
    void update_articles_per_second() {
        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
        if (duration.count() > 0) {
            articles_per_second = total_articles_processed / duration.count();
        }
        last_update = now;
    }
};

// ─────────────────────────────────────────────────────────────
// § 5.2  RealtimeConfig — 实时处理配置
// ─────────────────────────────────────────────────────────────

struct RealtimeConfig {
    // 线程配置
    size_t parser_threads{2};              // 解析线程数
    size_t processing_threads{4};         // 处理线程数
    size_t graph_update_threads{2};        // 图更新线程数
    
    // 队列配置
    size_t max_queue_size{10000};         // 最大队列大小
    size_t batch_size{10};                 // 批处理大小
    std::chrono::milliseconds batch_timeout{100}; // 批处理超时
    
    // 更新间隔
    std::chrono::seconds fetch_interval{60};     // 资讯获取间隔
    std::chrono::seconds cleanup_interval{300};  // 清理间隔
    std::chrono::seconds stats_interval{30};     // 统计更新间隔
    
    // 性能配置
    bool enable_batch_processing{true};   // 启用批处理
    bool enable_async_processing{true};    // 启用异步处理
    bool enable_auto_cleanup{true};        // 启用自动清理
    bool enable_metrics{true};             // 启用指标收集
    
    // 过滤配置
    double min_article_importance{0.3};    // 最小资讯重要性
    double min_entity_confidence{0.6};     // 最小实体置信度
    double min_relation_confidence{0.6};   // 最小关系置信度
};

// ─────────────────────────────────────────────────────────────
// § 5.3  NewsEvent — 资讯事件
// ─────────────────────────────────────────────────────────────

enum class NewsEventType : int {
    ARTICLE_RECEIVED,     // 收到新资讯
    ARTICLE_PROCESSED,     // 资讯处理完成
    ENTITY_EXTRACTED,      // 实体提取完成
    RELATION_EXTRACTED,    // 关系提取完成
    GRAPH_UPDATED,         // 图更新完成
    ERROR_OCCURRED,        // 处理错误
    QUEUE_FULL,           // 队列满
    CLEANUP_COMPLETED     // 清理完成
};

} // namespace agentos::news

// Hash for NewsEventType (needed for unordered_map key)
template<>
struct std::hash<agentos::news::NewsEventType> {
    size_t operator()(agentos::news::NewsEventType t) const noexcept {
        return std::hash<int>{}(static_cast<int>(t));
    }
};

namespace agentos::news {

struct NewsEvent {
    NewsEventType type;
    std::string data;                    // 事件数据（JSON格式）
    std::chrono::system_clock::time_point timestamp;
    std::string source_id;               // 来源ID
    
    NewsEvent(NewsEventType t, const std::string& d, const std::string& src = "")
        : type(t), data(d), timestamp(std::chrono::system_clock::now()), source_id(src) {}
};

// ─────────────────────────────────────────────────────────────
// § 5.4  IRealtimeProcessor Interface
// ─────────────────────────────────────────────────────────────

class IRealtimeProcessor {
public:
    virtual ~IRealtimeProcessor() = default;

    // 启动/停止处理器
    [[nodiscard]] virtual Result<bool> start() = 0;
    [[nodiscard]] virtual Result<bool> stop() = 0;
    virtual bool is_running() const noexcept = 0;

    // 添加资讯源
    [[nodiscard]] virtual Result<bool> add_news_source(const NewsSourceConfig& config) = 0;
    [[nodiscard]] virtual Result<bool> remove_news_source(const std::string& source_name) = 0;

    // 手动添加资讯
    [[nodiscard]] virtual Result<bool> add_article(const NewsArticle& article) = 0;
    [[nodiscard]] virtual Result<bool> add_articles(const std::vector<NewsArticle>& articles) = 0;

    // 获取统计信息
    virtual ProcessingStats get_stats() const noexcept = 0;

    // 事件回调
    virtual void set_event_callback(std::function<void(const NewsEvent&)> callback) noexcept = 0;

    // 配置管理
    virtual void update_config(const RealtimeConfig& config) noexcept = 0;
    virtual const RealtimeConfig& get_config() const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// § 5.5  RealtimeNewsProcessor — 实时资讯处理器实现
// ─────────────────────────────────────────────────────────────

class RealtimeNewsProcessor : public IRealtimeProcessor {
public:
    explicit RealtimeNewsProcessor(
        std::shared_ptr<agentos::memory::MemorySystem> memory_system,
        const RealtimeConfig& config = RealtimeConfig{});

    ~RealtimeNewsProcessor() override;

    // IRealtimeProcessor 接口实现
    [[nodiscard]] Result<bool> start() override;
    [[nodiscard]] Result<bool> stop() override;
    bool is_running() const noexcept override { return running_; }

    [[nodiscard]] Result<bool> add_news_source(const NewsSourceConfig& config) override;
    [[nodiscard]] Result<bool> remove_news_source(const std::string& source_name) override;

    [[nodiscard]] Result<bool> add_article(const NewsArticle& article) override;
    [[nodiscard]] Result<bool> add_articles(const std::vector<NewsArticle>& articles) override;

    ProcessingStats get_stats() const noexcept override;

    void set_event_callback(std::function<void(const NewsEvent&)> callback) noexcept override;

    void update_config(const RealtimeConfig& config) noexcept override;
    const RealtimeConfig& get_config() const noexcept override { return config_; }
    
    // 额外方法
    void force_cleanup() noexcept;                    // 强制清理
    void rebuild_graph() noexcept;                   // 重建图
    std::vector<std::string> get_active_sources() const noexcept;
    
private:
    // 核心组件
    std::shared_ptr<agentos::memory::MemorySystem> memory_system_;
    std::unique_ptr<NewsGraphManager> graph_manager_;
    std::unique_ptr<NewsParserManager> parser_manager_;
    
    // 配置和状态
    RealtimeConfig config_;
    std::atomic<bool> running_{false};
    ProcessingStats stats_;
    
    // 线程管理
    std::vector<std::thread> worker_threads_;
    std::thread fetch_thread_;
    std::thread cleanup_thread_;
    std::thread stats_thread_;
    
    // 队列管理
    std::queue<NewsArticle> article_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> should_stop_{false};
    
    // 资讯源管理
    std::unordered_map<std::string, NewsSourceConfig> news_sources_;
    std::mutex sources_mutex_;
    
    // 事件回调
    std::function<void(const NewsEvent&)> event_callback_;
    std::mutex callback_mutex_;
    
    // 私有方法
    void worker_loop();                     // 工作线程循环
    void fetch_loop();                      // 资讯获取循环
    void cleanup_loop();                    // 清理循环
    void stats_loop();                      // 统计更新循环
    
    // 资讯处理
    [[nodiscard]] Result<bool> process_article(const NewsArticle& article);
    [[nodiscard]] Result<bool> process_article_batch(const std::vector<NewsArticle>& articles);
    
    // 队列操作
    bool enqueue_article(const NewsArticle& article);
    std::optional<NewsArticle> dequeue_article();
    std::vector<NewsArticle> dequeue_batch();
    
    // 事件处理
    void emit_event(const NewsEvent& event);
    void handle_error(const std::string& error, const std::string& context = "");
    
    // 统计更新
    void update_stats();
    void reset_stats();
    
    // 资讯源管理
    void fetch_from_all_sources();
    Result<std::vector<NewsArticle>> fetch_from_source(const NewsSourceConfig& config);
    
    // 清理操作
    void perform_cleanup();
    void cleanup_expired_articles();
    void cleanup_graph_data();
};

// ─────────────────────────────────────────────────────────────
// § 5.6  NewsEventBus — 事件总线
// ─────────────────────────────────────────────────────────────

class NewsEventBus {
public:
    static NewsEventBus& instance();

    // 订阅事件（返回订阅 ID，用于取消订阅）
    size_t subscribe(NewsEventType type, std::function<void(const NewsEvent&)> callback);
    void unsubscribe(NewsEventType type, size_t subscription_id);

    // 发布事件
    // THREAD-SAFETY: Dispatches to subscribers under subscribers_mutex_, then updates
    // event_history_ under history_mutex_ (separate lock scope to avoid ordering races).
    void publish(const NewsEvent& event);

    // 获取事件历史
    std::vector<NewsEvent> get_event_history(NewsEventType type,
                                            std::chrono::seconds duration = std::chrono::seconds{300}) const;

private:
    NewsEventBus() = default;

    struct Subscription {
        size_t id;
        std::function<void(const NewsEvent&)> callback;
    };

    std::unordered_map<NewsEventType, std::vector<Subscription>> subscribers_;
    std::mutex subscribers_mutex_;
    std::atomic<size_t> next_subscription_id_{1};

    // 事件历史（可选）
    // NOTE: Must be protected separately from subscribers_ to avoid deadlock and ordering races.
    // publish() releases subscribers_mutex_ before acquiring history_mutex_.
    std::vector<NewsEvent> event_history_;
    std::mutex history_mutex_;
    static constexpr size_t max_history_size = 10000;
};

// ─────────────────────────────────────────────────────────────
// § 5.7  NewsMetricsCollector — 指标收集器
// ─────────────────────────────────────────────────────────────

class NewsMetricsCollector {
public:
    explicit NewsMetricsCollector(const std::string& metrics_prefix = "agentos_news");
    
    // 记录指标
    void record_counter(const std::string& name, double value = 1.0);
    void record_gauge(const std::string& name, double value);
    void record_histogram(const std::string& name, double value);
    void record_timer(const std::string& name, std::chrono::milliseconds duration);
    
    // 获取指标
    std::unordered_map<std::string, double> get_metrics() const;
    std::string get_metrics_prometheus() const;
    
    // 重置指标
    void reset_metrics();
    
private:
    std::string metrics_prefix_;
    std::unordered_map<std::string, double> counters_;
    std::unordered_map<std::string, double> gauges_;
    std::unordered_map<std::string, std::vector<double>> histograms_;
    std::unordered_map<std::string, std::vector<double>> timers_;
    mutable std::mutex metrics_mutex_;
};

// ─────────────────────────────────────────────────────────────
// § 5.8  RealtimeProcessorFactory — 工厂类
// ─────────────────────────────────────────────────────────────

class RealtimeProcessorFactory {
public:
    // 创建标准实时处理器
    static std::unique_ptr<IRealtimeProcessor> create_processor(
        std::shared_ptr<agentos::memory::MemorySystem> memory_system,
        const RealtimeConfig& config = RealtimeConfig{});
    
    // 创建高性能处理器（针对高吞吐量场景）
    static std::unique_ptr<IRealtimeProcessor> create_high_performance_processor(
        std::shared_ptr<agentos::memory::MemorySystem> memory_system);
    
    // 创建低延迟处理器（针对实时性要求高的场景）
    static std::unique_ptr<IRealtimeProcessor> create_low_latency_processor(
        std::shared_ptr<agentos::memory::MemorySystem> memory_system);
    
    // 创建自定义处理器
    static std::unique_ptr<IRealtimeProcessor> create_custom_processor(
        std::shared_ptr<agentos::memory::MemorySystem> memory_system,
        const RealtimeConfig& config,
        std::unique_ptr<NewsGraphManager> custom_graph_manager = nullptr);
};

} // namespace agentos::news
