#pragma once
// ============================================================
// AgentOS :: News Processing Module - Relation Extractor
// 关系抽取器：从资讯中提取实体间的语义关系
// ============================================================
#include <agentos/core/types.hpp>
#include "entity_extractor.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <regex>
#include <memory>

namespace agentos::news {

// Hash for std::pair (used by RelationValidator compatibility matrix)
struct PairHash {
    template <typename T1, typename T2>
    size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

// ─────────────────────────────────────────────────────────────
// § 3.1  RelationType — 关系类型枚举
// ─────────────────────────────────────────────────────────────

enum class RelationType {
    // 公司相关
    FOUNDED_BY,        // 由...创立
    CEO_OF,           // 担任CEO
    INVESTED_IN,      // 投资于
    ACQUIRED,         // 收购
    MERGED_WITH,      // 合并
    PARTNERED_WITH,   // 合作
    
    // 人物相关
    WORKS_FOR,        // 工作于
    MEMBER_OF,        // 成员
    GRADUATED_FROM,   // 毕业于
    MARRIED_TO,       // 结婚
    FRIEND_OF,        // 朋友
    
    // 产品相关
    PRODUCED_BY,      // 由...生产
    OWNED_BY,         // 由...拥有
    SUBSIDIARY_OF,    // 子公司
    
    // 事件相关
    PARTICIPATED_IN,  // 参与
    CAUSED_BY,        // 由...引起
    RESULTED_IN,      // 导致
    
    // 地理相关
    LOCATED_IN,       // 位于
    BORN_IN,          // 出生于
    
    // 通用关系
    ASSOCIATED_WITH,  // 关联
    RELATED_TO,       // 相关
    UNKNOWN           // 未知关系
};

// 关系类型转字符串
std::string relation_type_to_string(RelationType type);
RelationType string_to_relation_type(const std::string& type_str);

// ─────────────────────────────────────────────────────────────
// § 3.2  Relation — 提取的关系信息
// ─────────────────────────────────────────────────────────────

struct Relation {
    Entity subject;                    // 主语实体
    Entity object;                     // 宾语实体
    RelationType type;                 // 关系类型
    std::string relation_text;         // 原始关系文本
    size_t start_pos;                  // 在文本中的起始位置
    size_t end_pos;                    // 在文本中的结束位置
    double confidence;                 // 置信度 (0-1)
    std::unordered_map<std::string, std::string> attributes; // 额外属性
    
    // 构造函数
    Relation(const Entity& subj, const Entity& obj, RelationType t, 
             const std::string& rel_text, size_t start, size_t end, double conf = 0.8)
        : subject(subj), object(obj), type(t), relation_text(rel_text), 
          start_pos(start), end_pos(end), confidence(conf) {}
    
    // 获取关系的唯一标识
    std::string get_id() const;
    
    // 检查是否为同一关系
    bool is_same_relation(const Relation& other) const;
};

// ─────────────────────────────────────────────────────────────
// § 3.3  RelationPattern — 关系识别模式
// ─────────────────────────────────────────────────────────────

struct RelationPattern {
    RelationType type;
    std::regex pattern;
    std::function<std::pair<size_t, size_t>(const std::smatch&)> position_extractor;
    double confidence_weight;
    
    RelationPattern(RelationType t, const std::string& pattern_str, double weight = 0.8,
                   std::function<std::pair<size_t, size_t>(const std::smatch&)> pos_ext = nullptr)
        : type(t), pattern(pattern_str), position_extractor(pos_ext), confidence_weight(weight) {}
};

// ─────────────────────────────────────────────────────────────
// § 3.4  IRelationExtractor Interface
// ─────────────────────────────────────────────────────────────

class IRelationExtractor {
public:
    virtual ~IRelationExtractor() = default;

    // 从文本和实体中提取关系
    [[nodiscard]] virtual Result<std::vector<Relation>> extract_relations(
        const std::string& text,
        const std::vector<Entity>& entities) = 0;

    // 批量提取
    [[nodiscard]] virtual Result<std::vector<Relation>> extract_relations_batch(
        const std::vector<std::pair<std::string, std::vector<Entity>>>& text_entity_pairs) = 0;

    // 获取支持的关系类型
    virtual std::vector<RelationType> get_supported_types() const noexcept = 0;

    // 更新关系模式库
    virtual void update_relation_patterns(const std::vector<RelationPattern>& patterns) noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// § 3.5  RuleBasedRelationExtractor — 基于规则的关系提取器
// ─────────────────────────────────────────────────────────────

class RuleBasedRelationExtractor : public IRelationExtractor {
public:
    RuleBasedRelationExtractor();

    [[nodiscard]] Result<std::vector<Relation>> extract_relations(
        const std::string& text,
        const std::vector<Entity>& entities) override;

    [[nodiscard]] Result<std::vector<Relation>> extract_relations_batch(
        const std::vector<std::pair<std::string, std::vector<Entity>>>& text_entity_pairs) override;

    std::vector<RelationType> get_supported_types() const noexcept override;
    void update_relation_patterns(const std::vector<RelationPattern>& patterns) noexcept override;
    
    // 添加自定义关系模式
    void add_pattern(RelationType type, const std::string& pattern, 
                     double weight = 0.8,
                     std::function<std::pair<size_t, size_t>(const std::smatch&)> pos_ext = nullptr);
    
    // 加载关系模式词典
    void load_relation_patterns(const std::string& file_path);
    
private:
    std::vector<RelationPattern> patterns_;
    
    void initialize_default_patterns();
    std::vector<Relation> extract_with_patterns(const std::string& text, 
                                               const std::vector<Entity>& entities);
    std::vector<Relation> extract_dependency_relations(const std::string& text,
                                                      const std::vector<Entity>& entities);
    std::vector<Entity> find_entities_in_range(const std::vector<Entity>& entities,
                                               size_t start, size_t end);
    RelationType infer_relation_type(const std::string& context, 
                                    const Entity& subj, const Entity& obj);
};

// ─────────────────────────────────────────────────────────────
// § 3.6  MLBasedRelationExtractor — 基于机器学习的关系提取器
// ─────────────────────────────────────────────────────────────

class MLBasedRelationExtractor : public IRelationExtractor {
public:
    explicit MLBasedRelationExtractor(const std::string& model_path = "");

    [[nodiscard]] Result<std::vector<Relation>> extract_relations(
        const std::string& text,
        const std::vector<Entity>& entities) override;

    [[nodiscard]] Result<std::vector<Relation>> extract_relations_batch(
        const std::vector<std::pair<std::string, std::vector<Entity>>>& text_entity_pairs) override;

    std::vector<RelationType> get_supported_types() const noexcept override;
    void update_relation_patterns(const std::vector<RelationPattern>& patterns) noexcept override;

    // 加载预训练模型
    [[nodiscard]] Result<bool> load_model(const std::string& model_path);
    
    // 在线学习
    void train_online(const std::string& text, const std::vector<Entity>& entities,
                     const std::vector<Relation>& gold_relations);
    
private:
    std::string model_path_;
    bool model_loaded_{false};
    
    // 模拟的ML预测（实际实现中会调用真实的ML模型）
    std::vector<Relation> predict_relations(const std::string& text, 
                                           const std::vector<Entity>& entities);
    std::vector<std::string> extract_features(const std::string& text,
                                             const Entity& subj, const Entity& obj);
};

// ─────────────────────────────────────────────────────────────
// § 3.7  RelationExtractorFactory
// ─────────────────────────────────────────────────────────────

class RelationExtractorFactory {
public:
    enum class ExtractorType {
        RULE_BASED,
        ML_BASED,
        HYBRID
    };
    
    static std::unique_ptr<IRelationExtractor> create_extractor(ExtractorType type);
    static std::unique_ptr<IRelationExtractor> create_extractor(const std::string& type_str);
    
    // 创建混合提取器（规则+ML）
    static std::unique_ptr<IRelationExtractor> create_hybrid_extractor(
        const std::string& model_path = "");
};

// ─────────────────────────────────────────────────────────────
// § 3.8  HybridRelationExtractor — 混合关系提取器
// ─────────────────────────────────────────────────────────────

class HybridRelationExtractor : public IRelationExtractor {
public:
    explicit HybridRelationExtractor(const std::string& model_path = "");

    [[nodiscard]] Result<std::vector<Relation>> extract_relations(
        const std::string& text,
        const std::vector<Entity>& entities) override;

    [[nodiscard]] Result<std::vector<Relation>> extract_relations_batch(
        const std::vector<std::pair<std::string, std::vector<Entity>>>& text_entity_pairs) override;

    std::vector<RelationType> get_supported_types() const noexcept override;
    void update_relation_patterns(const std::vector<RelationPattern>& patterns) noexcept override;
    
private:
    std::unique_ptr<RuleBasedRelationExtractor> rule_extractor_;
    std::unique_ptr<MLBasedRelationExtractor> ml_extractor_;
    
    std::vector<Relation> merge_results(const std::vector<Relation>& rule_results,
                                        const std::vector<Relation>& ml_results);
    std::vector<Relation> resolve_conflicts(const std::vector<Relation>& relations);
};

// ─────────────────────────────────────────────────────────────
// § 3.9  RelationLibrary — 关系库管理
// ─────────────────────────────────────────────────────────────

class RelationLibrary {
public:
    static RelationLibrary& instance();
    
    // 添加关系
    void add_relation(const Relation& relation);
    void add_relations(const std::vector<Relation>& relations);
    
    // 查找关系
    std::vector<Relation> find_relations(const std::string& subject_id, 
                                        const std::string& object_id) const;
    std::vector<Relation> find_relations_by_type(RelationType type) const;
    std::vector<Relation> find_relations_involving_entity(const std::string& entity_id) const;
    
    // 获取关系统计
    size_t get_relation_count() const;
    std::unordered_map<RelationType, size_t> get_type_distribution() const;
    
    // 保存/加载关系库
    void save_to_file(const std::string& file_path) const;
    void load_from_file(const std::string& file_path);
    
    // 关系验证
    bool validate_relation(const Relation& relation) const;
    
private:
    RelationLibrary() = default;
    std::unordered_map<std::string, std::vector<Relation>> relations_by_subject_;
    std::unordered_map<std::string, std::vector<Relation>> relations_by_object_;
    std::unordered_map<RelationType, std::vector<Relation>> relations_by_type_;
    mutable std::mutex mutex_;
};

// ─────────────────────────────────────────────────────────────
// § 3.10  RelationValidator — 关系验证器
// ─────────────────────────────────────────────────────────────

class RelationValidator {
public:
    // 验证关系的合理性
    static bool validate_relation(const Relation& relation);
    
    // 检查实体类型与关系类型的兼容性
    static bool is_compatible(EntityType subject_type, EntityType object_type, RelationType relation_type);
    
    // 计算关系置信度
    static double calculate_confidence(const Relation& relation, const std::string& context);
    
private:
    static std::unordered_map<std::pair<EntityType, EntityType>, std::vector<RelationType>, 
                             PairHash> compatible_relations_;
    static void initialize_compatibility_matrix();
};

} // namespace agentos::news
