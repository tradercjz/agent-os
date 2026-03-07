#pragma once
// ============================================================
// AgentOS :: News Processing Module - Entity Extractor
// 实体识别器：从资讯中提取命名实体（公司、人物、地点等）
// ============================================================
#include <agentos/core/types.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <memory>

namespace agentos::news {

// ─────────────────────────────────────────────────────────────
// § 2.1  EntityType — 实体类型枚举
// ─────────────────────────────────────────────────────────────

enum class EntityType {
    PERSON,        // 人物
    COMPANY,       // 公司/组织
    LOCATION,      // 地点
    PRODUCT,       // 产品
    EVENT,         // 事件
    CONCEPT,       // 概念
    DATE_TIME,     // 日期时间
    MONEY,         // 金额
    PERCENTAGE,    // 百分比
    UNKNOWN        // 未知类型
};

// 实体类型转字符串
std::string entity_type_to_string(EntityType type);
EntityType string_to_entity_type(const std::string& type_str);

// ─────────────────────────────────────────────────────────────
// § 2.2  Entity — 提取的实体信息
// ─────────────────────────────────────────────────────────────

struct Entity {
    std::string text;              // 实体原文
    std::string normalized_name;   // 标准化名称
    EntityType type;               // 实体类型
    size_t start_pos;              // 在文本中的起始位置
    size_t end_pos;                // 在文本中的结束位置
    double confidence;             // 置信度 (0-1)
    std::unordered_map<std::string, std::string> attributes; // 额外属性
    
    // 构造函数
    Entity(const std::string& txt, EntityType t, size_t start, size_t end, double conf = 0.8)
        : text(txt), type(t), start_pos(start), end_pos(end), confidence(conf) {
        normalized_name = normalize_name(txt);
    }
    
    // 标准化实体名称
    static std::string normalize_name(const std::string& name);
    
    // 检查是否为同一实体
    bool is_same_entity(const Entity& other) const;
    
private:
    static std::string normalize_name_impl(const std::string& name);
};

// ─────────────────────────────────────────────────────────────
// § 2.3  EntityPattern — 实体识别模式
// ─────────────────────────────────────────────────────────────

struct EntityPattern {
    EntityType type;
    std::regex pattern;
    std::function<std::string(const std::string&)> normalizer;
    double confidence_weight;
    
    EntityPattern(EntityType t, const std::string& pattern_str, 
                  double weight = 0.8, std::function<std::string(const std::string&)> norm = nullptr)
        : type(t), pattern(pattern_str), normalizer(norm), confidence_weight(weight) {}
};

// ─────────────────────────────────────────────────────────────
// § 2.4  IEntityExtractor Interface
// ─────────────────────────────────────────────────────────────

class IEntityExtractor {
public:
    virtual ~IEntityExtractor() = default;
    
    // 从文本中提取实体
    virtual Result<std::vector<Entity>> extract(const std::string& text) = 0;
    
    // 批量提取
    virtual Result<std::vector<Entity>> extract_batch(const std::vector<std::string>& texts) = 0;
    
    // 获取支持的实体类型
    virtual std::vector<EntityType> get_supported_types() const = 0;
    
    // 更新实体库
    virtual void update_entity_library(const std::vector<Entity>& entities) = 0;
};

// ─────────────────────────────────────────────────────────────
// § 2.5  RuleBasedEntityExtractor — 基于规则的实体提取器
// ─────────────────────────────────────────────────────────────

class RuleBasedEntityExtractor : public IEntityExtractor {
public:
    RuleBasedEntityExtractor();
    
    Result<std::vector<Entity>> extract(const std::string& text) override;
    Result<std::vector<Entity>> extract_batch(const std::vector<std::string>& texts) override;
    std::vector<EntityType> get_supported_types() const override;
    void update_entity_library(const std::vector<Entity>& entities) override;
    
    // 添加自定义模式
    void add_pattern(EntityType type, const std::string& pattern, 
                     double weight = 0.8, 
                     std::function<std::string(const std::string&)> normalizer = nullptr);
    
    // 加载实体词典
    void load_entity_dictionary(const std::string& file_path);
    
private:
    std::vector<EntityPattern> patterns_;
    std::unordered_set<std::string> company_names_;
    std::unordered_set<std::string> person_names_;
    std::unordered_set<std::string> location_names_;
    
    void initialize_default_patterns();
    std::vector<Entity> extract_with_patterns(const std::string& text);
    std::vector<Entity> extract_with_dictionary(const std::string& text);
    std::vector<Entity> merge_and_deduplicate(const std::vector<Entity>& pattern_entities,
                                             const std::vector<Entity>& dict_entities);
};

// ─────────────────────────────────────────────────────────────
// § 2.6  MLBasedEntityExtractor — 基于机器学习的实体提取器
// ─────────────────────────────────────────────────────────────

class MLBasedEntityExtractor : public IEntityExtractor {
public:
    explicit MLBasedEntityExtractor(const std::string& model_path = "");
    
    Result<std::vector<Entity>> extract(const std::string& text) override;
    Result<std::vector<Entity>> extract_batch(const std::vector<std::string>& texts) override;
    std::vector<EntityType> get_supported_types() const override;
    void update_entity_library(const std::vector<Entity>& entities) override;
    
    // 加载预训练模型
    Result<bool> load_model(const std::string& model_path);
    
    // 在线学习
    void train_online(const std::string& text, const std::vector<Entity>& gold_entities);
    
private:
    std::string model_path_;
    bool model_loaded_{false};
    
    // 模拟的ML预测（实际实现中会调用真实的ML模型）
    std::vector<Entity> predict_entities(const std::string& text);
};

// ─────────────────────────────────────────────────────────────
// § 2.7  EntityExtractorFactory
// ─────────────────────────────────────────────────────────────

class EntityExtractorFactory {
public:
    enum class ExtractorType {
        RULE_BASED,
        ML_BASED,
        HYBRID
    };
    
    static std::unique_ptr<IEntityExtractor> create_extractor(ExtractorType type);
    static std::unique_ptr<IEntityExtractor> create_extractor(const std::string& type_str);
    
    // 创建混合提取器（规则+ML）
    static std::unique_ptr<IEntityExtractor> create_hybrid_extractor(
        const std::string& model_path = "");
};

// ─────────────────────────────────────────────────────────────
// § 2.8  HybridEntityExtractor — 混合实体提取器
// ─────────────────────────────────────────────────────────────

class HybridEntityExtractor : public IEntityExtractor {
public:
    explicit HybridEntityExtractor(const std::string& model_path = "");
    
    Result<std::vector<Entity>> extract(const std::string& text) override;
    Result<std::vector<Entity>> extract_batch(const std::vector<std::string>& texts) override;
    std::vector<EntityType> get_supported_types() const override;
    void update_entity_library(const std::vector<Entity>& entities) override;
    
private:
    std::unique_ptr<RuleBasedEntityExtractor> rule_extractor_;
    std::unique_ptr<MLBasedEntityExtractor> ml_extractor_;
    
    std::vector<Entity> merge_results(const std::vector<Entity>& rule_results,
                                      const std::vector<Entity>& ml_results);
};

// ─────────────────────────────────────────────────────────────
// § 2.9  EntityLibrary — 实体库管理
// ─────────────────────────────────────────────────────────────

class EntityLibrary {
public:
    static EntityLibrary& instance();
    
    // 添加实体
    void add_entity(const Entity& entity);
    void add_entities(const std::vector<Entity>& entities);
    
    // 查找实体
    std::vector<Entity> find_entities(const std::string& name) const;
    std::vector<Entity> find_entities_by_type(EntityType type) const;
    
    // 检查实体是否存在
    bool contains_entity(const std::string& name) const;
    
    // 标准化实体名称
    std::string normalize_entity_name(const std::string& name) const;
    
    // 保存/加载实体库
    void save_to_file(const std::string& file_path) const;
    void load_from_file(const std::string& file_path);
    
    // 获取统计信息
    size_t get_entity_count() const;
    std::unordered_map<EntityType, size_t> get_type_distribution() const;
    
private:
    EntityLibrary() = default;
    std::unordered_map<std::string, std::vector<Entity>> entities_by_name_;
    std::unordered_map<EntityType, std::vector<Entity>> entities_by_type_;
    mutable std::mutex mutex_;
};

} // namespace agentos::news
