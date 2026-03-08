#pragma once
// ============================================================
// AgentOS :: News Processing Module - News Parser
// 多格式资讯解析器：支持RSS、JSON、API等格式
// ============================================================
#include <agentos/core/types.hpp>
#include <chrono>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace agentos::news {

// ─────────────────────────────────────────────────────────────
// § 1.1  NewsArticle — 标准化资讯条目
// ─────────────────────────────────────────────────────────────

struct NewsArticle {
    std::string id;                    // 唯一标识
    std::string title;                 // 标题
    std::string content;               // 内容正文
    std::string summary;               // 摘要
    std::string source;                // 来源（媒体名称）
    std::string author;                // 作者
    std::string url;                   // 原文链接
    std::string category;              // 分类（财经、科技、政治等）
    std::vector<std::string> tags;     // 标签
    std::chrono::system_clock::time_point published_at; // 发布时间
    std::chrono::system_clock::time_point crawled_at;   // 抓取时间
    double sentiment_score{0.0};       // 情感分数 (-1 到 1)
    double importance_score{0.5};      // 重要性分数 (0 到 1)
    std::unordered_map<std::string, std::string> metadata; // 扩展字段
};

// ─────────────────────────────────────────────────────────────
// § 1.2  NewsSourceConfig — 资讯源配置
// ─────────────────────────────────────────────────────────────

struct NewsSourceConfig {
    std::string name;                  // 资讯源名称
    std::string type;                  // "rss", "json_api", "websocket"
    std::string url;                   // 资讯源URL
    std::string api_key;               // API密钥（如需要）
    std::vector<std::string> categories; // 关注的分类
    int update_interval_seconds{300};  // 更新间隔
    std::unordered_map<std::string, std::string> headers; // HTTP头
    std::unordered_map<std::string, std::string> params;  // 请求参数
};

// ─────────────────────────────────────────────────────────────
// § 1.3  INewsParser Interface
// ─────────────────────────────────────────────────────────────

class INewsParser {
public:
    virtual ~INewsParser() = default;

    // 解析原始数据为标准化资讯条目
    [[nodiscard]] virtual Result<std::vector<NewsArticle>> parse(const std::string& raw_data) = 0;

    // 获取解析器类型
    virtual std::string get_type() const noexcept = 0;

    // 验证数据格式
    virtual bool validate_format(const std::string& raw_data) const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// § 1.4  RSS Parser
// ─────────────────────────────────────────────────────────────

class RSSParser : public INewsParser {
public:
    [[nodiscard]] Result<std::vector<NewsArticle>> parse(const std::string& raw_data) override;
    std::string get_type() const noexcept override { return "rss"; }
    bool validate_format(const std::string& raw_data) const noexcept override;
    
private:
    std::string extract_xml_content(const std::string& xml, const std::string& tag);
    std::chrono::system_clock::time_point parse_rss_time(const std::string& time_str);
};

// ─────────────────────────────────────────────────────────────
// § 1.5  JSON API Parser
// ─────────────────────────────────────────────────────────────

class JSONAPIParser : public INewsParser {
public:
    explicit JSONAPIParser(const std::string& field_mapping = "");

    [[nodiscard]] Result<std::vector<NewsArticle>> parse(const std::string& raw_data) override;
    std::string get_type() const noexcept override { return "json_api"; }
    bool validate_format(const std::string& raw_data) const noexcept override;
    
private:
    std::string field_mapping_; // JSON字段映射配置
    
    NewsArticle parse_single_article(const std::string& json_str);
    std::chrono::system_clock::time_point parse_iso_time(const std::string& time_str);
};

// ─────────────────────────────────────────────────────────────
// § 1.6  NewsParserFactory
// ─────────────────────────────────────────────────────────────

class NewsParserFactory {
public:
    static std::unique_ptr<INewsParser> create_parser(const std::string& type);
    static std::unique_ptr<INewsParser> create_parser(const NewsSourceConfig& config);
    
    // 注册自定义解析器
    static void register_parser(const std::string& type, 
                               std::function<std::unique_ptr<INewsParser>()> creator);
    
private:
    static std::unordered_map<std::string, std::function<std::unique_ptr<INewsParser>()>> parsers_;
};

// ─────────────────────────────────────────────────────────────
// § 1.7  NewsFetcher — 资讯获取器
// ─────────────────────────────────────────────────────────────

class NewsFetcher {
public:
    explicit NewsFetcher(const NewsSourceConfig& config);

    // 获取资讯数据
    [[nodiscard]] Result<std::string> fetch();

    // 获取配置
    const NewsSourceConfig& get_config() const noexcept { return config_; }

    // 检查是否需要更新
    bool should_update() const noexcept;

    // 更新最后获取时间
    void mark_fetched() noexcept;
    
private:
    NewsSourceConfig config_;
    std::chrono::system_clock::time_point last_fetch_;
    
    Result<std::string> fetch_rss();
    Result<std::string> fetch_json_api();
    Result<std::string> fetch_websocket();
};

// ─────────────────────────────────────────────────────────────
// § 1.8  NewsParserManager — 统一管理器
// ─────────────────────────────────────────────────────────────

class NewsParserManager {
public:
    explicit NewsParserManager() = default;

    // 添加资讯源
    [[nodiscard]] Result<bool> add_source(const NewsSourceConfig& config);

    // 移除资讯源
    [[nodiscard]] Result<bool> remove_source(const std::string& source_name);

    // 获取所有资讯源
    std::vector<NewsSourceConfig> get_sources() const noexcept;

    // 获取指定源的资讯
    [[nodiscard]] Result<std::vector<NewsArticle>> fetch_news(const std::string& source_name);

    // 获取所有源的资讯
    [[nodiscard]] Result<std::vector<NewsArticle>> fetch_all_news();

    // 获取需要更新的源
    std::vector<std::string> get_sources_to_update() const noexcept;
    
private:
    // THREAD-SAFETY: Use shared_ptr to prevent use-after-move if remove_source() is called
    // while async operations hold references to the fetcher.
    std::unordered_map<std::string, std::shared_ptr<NewsFetcher>> fetchers_;
    std::unordered_map<std::string, std::unique_ptr<INewsParser>> parsers_;

    // IMPORTANT: Any code that starts async operations on a fetcher MUST capture the
    // shared_ptr by value, not by reference, to extend the fetcher's lifetime.
    Result<std::vector<NewsArticle>> process_source(const std::string& source_name);
};

} // namespace agentos::news
