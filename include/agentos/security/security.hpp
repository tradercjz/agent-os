#pragma once
// ============================================================
// AgentOS :: Module 6 — Security Layer
// RBAC 权限控制 / ECL 执行控制层 / 污点追踪 / 注入防御
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <cstring>
#include <agentos/kernel/llm_kernel.hpp>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos::security {

// ─────────────────────────────────────────────────────────────
// § 6.1  RBAC — 基于角色的访问控制
// ─────────────────────────────────────────────────────────────

enum class Permission : uint32_t {
    // 工具权限
    ToolReadOnly    = 1 << 0,  // 只读工具（http_fetch, kv_get）
    ToolWrite       = 1 << 1,  // 写工具（kv_set, file_write）
    ToolDangerous   = 1 << 2,  // 危险工具（shell_exec, code_exec）
    ToolSystem      = 1 << 3,  // 系统工具（进程管理、网络配置）
    // Agent 权限
    AgentCreate     = 1 << 4,
    AgentKill       = 1 << 5,
    AgentObserve    = 1 << 6,
    // 记忆权限
    MemoryRead      = 1 << 7,
    MemoryWrite     = 1 << 8,
    MemoryDelete    = 1 << 9,
    // 管理权限
    Admin           = 0xFFFFFFFF,
};

inline Permission operator|(Permission a, Permission b) {
    return static_cast<Permission>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_perm(Permission granted, Permission required) {
    return (static_cast<uint32_t>(granted) &
            static_cast<uint32_t>(required)) != 0;
}

struct Role {
    std::string name;
    Permission  permissions;

    static Role make_readonly() {
        return {"readonly",
                Permission::ToolReadOnly | Permission::MemoryRead |
                Permission::AgentObserve};
    }
    static Role make_standard() {
        return {"standard",
                Permission::ToolReadOnly | Permission::ToolWrite |
                Permission::MemoryRead  | Permission::MemoryWrite |
                Permission::AgentCreate | Permission::AgentObserve};
    }
    static Role make_privileged() {
        return {"privileged",
                Permission::ToolReadOnly | Permission::ToolWrite |
                Permission::ToolDangerous |
                Permission::MemoryRead   | Permission::MemoryWrite |
                Permission::MemoryDelete |
                Permission::AgentCreate  | Permission::AgentKill |
                Permission::AgentObserve};
    }
    static Role make_admin() {
        return {"admin", Permission::Admin};
    }
};

class RBAC : private NonCopyable {
public:
    RBAC() {
        // 注册内置角色
        roles_["readonly"]   = Role::make_readonly();
        roles_["standard"]   = Role::make_standard();
        roles_["privileged"] = Role::make_privileged();
        roles_["admin"]      = Role::make_admin();
    }

    void assign_role(AgentId agent_id, std::string_view role_name) {
        std::lock_guard lk(mu_);
        agent_roles_[agent_id] = std::string(role_name);
    }

    Result<void> check(AgentId agent_id, Permission required) const {
        std::lock_guard lk(mu_);
        auto ar_it = agent_roles_.find(agent_id);
        if (ar_it == agent_roles_.end())
            return make_error(ErrorCode::PermissionDenied,
                fmt::format("Agent {} has no role assigned", agent_id));

        auto role_it = roles_.find(ar_it->second);
        if (role_it == roles_.end())
            return make_error(ErrorCode::PermissionDenied,
                fmt::format("Role '{}' not found", ar_it->second));

        if (!has_perm(role_it->second.permissions, required))
            return make_error(ErrorCode::PermissionDenied,
                fmt::format("Agent {} (role={}) lacks permission {}",
                            agent_id, ar_it->second,
                            static_cast<uint32_t>(required)));
        return {};
    }

    bool may(AgentId agent_id, Permission perm) const {
        return check(agent_id, perm).has_value();
    }

    void define_role(std::string name, Permission perms) {
        std::lock_guard lk(mu_);
        Role r{name, perms};
        roles_[std::move(name)] = std::move(r);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, Role>   roles_;
    std::unordered_map<AgentId, std::string> agent_roles_;
};

// ─────────────────────────────────────────────────────────────
// § 6.2  TaintTracker — 数据流污点追踪
// ─────────────────────────────────────────────────────────────

enum class TrustLevel {
    Trusted   = 0, // 来自系统或已认证 Agent
    UserInput = 1, // 来自用户输入（基本可信但需验证）
    External  = 2, // 来自外部 HTTP/文件（不可信）
    Untrusted = 3, // 已知恶意或注入尝试
};

struct TaintedData {
    std::string data;
    TrustLevel  trust;
    std::string source_tag; // 来源描述
};

class TaintTracker : private NonCopyable {
public:
    // 注册数据来源
    void taint(const std::string& data_id, TrustLevel level,
               std::string source = "") {
        std::lock_guard lk(mu_);
        taint_map_[data_id] = {data_id, level, std::move(source)};
    }

    TrustLevel get_trust(const std::string& data_id) const {
        std::lock_guard lk(mu_);
        auto it = taint_map_.find(data_id);
        if (it == taint_map_.end()) return TrustLevel::Trusted;
        return it->second.trust;
    }

    // 当污点数据流入敏感工具时，检查是否允许
    // sensitive_tools: 需要 Trusted 输入的工具列表
    Result<void> check_flow(const std::string& data_id,
                            const std::string& target_tool) const {
        static const std::unordered_set<std::string> sensitive_tools = {
            "shell_exec", "code_exec", "send_email",
            "file_write", "db_write", "http_post"
        };

        if (!sensitive_tools.count(target_tool)) return {};

        auto trust = get_trust(data_id);
        if (trust >= TrustLevel::External) {
            return make_error(ErrorCode::TaintedInput,
                fmt::format("Tainted data (trust={}) flowing into sensitive tool '{}'",
                            static_cast<int>(trust), target_tool));
        }
        return {};
    }

    // 从已有污点数据衍生（传播污点）
    void propagate(const std::string& source_id, const std::string& derived_id) {
        std::lock_guard lk(mu_);
        auto it = taint_map_.find(source_id);
        if (it != taint_map_.end()) {
            taint_map_[derived_id] = {derived_id, it->second.trust,
                                      "derived from " + source_id};
        }
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, TaintedData> taint_map_;
};

// ─────────────────────────────────────────────────────────────
// § 6.3  InjectionDetector — Prompt 注入检测
// ─────────────────────────────────────────────────────────────

class InjectionDetector {
public:
    InjectionDetector() {
        // 注入特征模式（可扩展）
        patterns_ = {
            "ignore previous instructions",
            "ignore all previous",
            "disregard your instructions",
            "you are now",
            "act as if",
            "forget your guidelines",
            "override system prompt",
            "new system prompt",
            "jailbreak",
            "dan mode",
            // 中文注入模式
            "忽略之前的指令",
            "忘记你的指令",
            "现在你是",
            "扮演",
        };
    }

    struct DetectionResult {
        bool        is_injection;
        std::string matched_pattern;
        float       confidence; // 0~1
    };

    DetectionResult scan(std::string_view text) const {
        std::string lower;
        lower.reserve(text.size());
        for (char c : text)
          lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::lock_guard lk(mu_);
        for (const auto& pat : patterns_) {
            if (lower.find(pat) != std::string::npos) {
                return {true, pat, 0.9f};
            }
        }

        // 启发式：异常多的指令性短语
        int instruction_count = 0;
        for (auto& kw : {"must", "shall", "should", "always", "never"}) {
            size_t pos = 0;
            while ((pos = lower.find(kw, pos)) != std::string::npos) {
                instruction_count++;
                pos += strlen(kw);
            }
        }
        if (instruction_count > 5) {
            return {true, "excessive instructions", 0.6f};
        }

        return {false, "", 0.0f};
    }

    // Thread-safe: add pattern at runtime (hot-reload)
    void add_pattern(std::string pat) {
        std::lock_guard lk(mu_);
        patterns_.push_back(std::move(pat));
    }

    // Thread-safe: remove pattern by value
    bool remove_pattern(const std::string &pat) {
        std::lock_guard lk(mu_);
        auto it = std::find(patterns_.begin(), patterns_.end(), pat);
        if (it != patterns_.end()) {
            patterns_.erase(it);
            return true;
        }
        return false;
    }

    // Thread-safe: replace all patterns at once
    void set_patterns(std::vector<std::string> pats) {
        std::lock_guard lk(mu_);
        patterns_ = std::move(pats);
    }

    size_t pattern_count() const {
        std::lock_guard lk(mu_);
        return patterns_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> patterns_;
};

// ─────────────────────────────────────────────────────────────
// § 6.4  ECL — 执行控制层（确定性拦截门面）
// ─────────────────────────────────────────────────────────────

// 高风险操作的人机回环回调（返回 true 表示批准）
using HumanApprovalFn = std::function<bool(AgentId, std::string_view action,
                                           std::string_view details)>;

class ExecutionControlLayer : private NonCopyable {
public:
    explicit ExecutionControlLayer(HumanApprovalFn approval_fn = nullptr)
        : human_approval_(std::move(approval_fn)) {}

    void set_rbac(std::shared_ptr<RBAC> rbac)               { rbac_ = rbac; }
    void set_taint_tracker(std::shared_ptr<TaintTracker> tt) { taint_ = tt; }

    // ── 工具调用前置检查 ────────────────────────────────────
    Result<void> before_tool_call(AgentId agent_id,
                                  const std::string& tool_id,
                                  const std::string& args_json,
                                  const std::string& input_data_id = "") {
        // 1. RBAC 检查
        if (rbac_) {
            Permission required = Permission::ToolReadOnly;
            if (dangerous_tools_.count(tool_id))
                required = Permission::ToolDangerous;
            else if (write_tools_.count(tool_id))
                required = Permission::ToolWrite;

            auto r = rbac_->check(agent_id, required);
            if (!r) return r;
        }

        // 2. 污点检查
        if (taint_ && !input_data_id.empty()) {
            auto r = taint_->check_flow(input_data_id, tool_id);
            if (!r) return r;
        }

        // 3. 注入检测（检查 args 是否包含注入尝试）
        auto det = injection_detector_.scan(args_json);
        if (det.is_injection) {
            {
            auto msg = fmt::format(
                "[ALERT] Injection detected in tool '{}' args: pattern='{}'",
                tool_id, det.matched_pattern);
            audit_log_.push_back(msg);
            LOG_WARN(msg);
            }
            return make_error(ErrorCode::InjectionDetected,
                fmt::format("Prompt injection detected: {}", det.matched_pattern));
        }

        // 4. 高风险操作需要人工批准
        if (human_approval_ && critical_tools_.count(tool_id)) {
            bool approved = human_approval_(agent_id, tool_id, args_json);
            if (!approved) {
                return make_error(ErrorCode::PermissionDenied,
                    fmt::format("Human rejected tool call: {}", tool_id));
            }
        }

        audit_log_.push_back(fmt::format(
            "[OK] agent={} tool={} args_len={}", agent_id, tool_id, args_json.size()));
        return {};
    }

    // ── LLM 输出扫描 ────────────────────────────────────────
    Result<void> scan_llm_output(AgentId agent_id,
                                 const kernel::LLMResponse& response) {
        // 扫描文本输出中的注入尝试
        auto det = injection_detector_.scan(response.content);
        if (det.is_injection && det.confidence > 0.8f) {
            audit_log_.push_back(fmt::format(
                "[WARN] LLM output injection suspicion: agent={} pattern='{}'",
                agent_id, det.matched_pattern));
            // 警告但不阻断（低置信度时仅记录）
        }
        return {};
    }

    // ── 配置危险工具集 ──────────────────────────────────────
    void mark_dangerous(std::string tool_id) {
        dangerous_tools_.insert(std::move(tool_id));
    }
    void mark_write(std::string tool_id) {
        write_tools_.insert(std::move(tool_id));
    }
    void mark_critical(std::string tool_id) {
        critical_tools_.insert(std::move(tool_id));
    }

    const std::vector<std::string>& audit_log() const { return audit_log_; }

    void clear_audit_log() { audit_log_.clear(); }

    // 暴露内部检测器，供 SecurityManager 统一使用（避免实例重复）
    InjectionDetector& detector() { return injection_detector_; }
    const InjectionDetector& detector() const { return injection_detector_; }

private:
    std::shared_ptr<RBAC>         rbac_;
    std::shared_ptr<TaintTracker> taint_;
    InjectionDetector             injection_detector_;
    HumanApprovalFn               human_approval_;

    std::unordered_set<std::string> dangerous_tools_{"shell_exec", "code_exec"};
    std::unordered_set<std::string> write_tools_{"kv_store", "file_write"};
    std::unordered_set<std::string> critical_tools_{"send_email", "db_delete"};

    std::vector<std::string> audit_log_;
};

// ─────────────────────────────────────────────────────────────
// § 6.5  SecurityManager — 安全层统一门面
// ─────────────────────────────────────────────────────────────

class SecurityManager : private NonCopyable {
public:
    explicit SecurityManager(HumanApprovalFn approval_fn = nullptr)
        : rbac_(std::make_shared<RBAC>()),
          taint_(std::make_shared<TaintTracker>()),
          ecl_(std::make_unique<ExecutionControlLayer>(std::move(approval_fn))) {
        ecl_->set_rbac(rbac_);
        ecl_->set_taint_tracker(taint_);

        // 标记内置危险/写工具
        ecl_->mark_dangerous("shell_exec");
        ecl_->mark_write("kv_store");
        ecl_->mark_critical("send_email");
    }

    RBAC&                   rbac()   { return *rbac_; }
    TaintTracker&           taint()  { return *taint_; }
    ExecutionControlLayer&  ecl()    { return *ecl_; }
    // 统一使用 ECL 内部的 InjectionDetector，避免两份实例不同步
    InjectionDetector&      detector() { return ecl_->detector(); }

    // 快捷方法
    void grant(AgentId id, std::string_view role) { rbac_->assign_role(id, role); }

    bool may(AgentId id, Permission p) const { return rbac_->may(id, p); }

    Result<void> authorize_tool(AgentId agent_id,
                                const std::string& tool_id,
                                const std::string& args_json) {
        return ecl_->before_tool_call(agent_id, tool_id, args_json);
    }

private:
    std::shared_ptr<RBAC>              rbac_;
    std::shared_ptr<TaintTracker>      taint_;
    std::unique_ptr<ExecutionControlLayer> ecl_;
};

} // namespace agentos::security
