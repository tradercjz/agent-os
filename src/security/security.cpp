#include <agentos/security/security.hpp>

#include <algorithm>
#include <cstring>

namespace agentos::security {

// ─────────────────────────────────────────────────────────────
// TaintTracker
// ─────────────────────────────────────────────────────────────

void TaintTracker::taint(std::string_view data_id, TrustLevel level,
                         std::string source) {
    std::lock_guard lk(mu_);
    std::string key(data_id);
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Update existing entry, move to front (most recently used)
        it->second->trust = level;
        it->second->source_tag = std::move(source);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    // Evict LRU (back of list) when full
    if (map_.size() >= kMaxTaintEntries) {
        auto& victim = lru_.back();
        LOG_WARN(fmt::format("[Security:TaintTracker] Evicting LRU taint entry '{}' "
                             "to make room for '{}' (map full at {} entries)",
                             victim.data, key, kMaxTaintEntries));
        map_.erase(victim.data);
        lru_.pop_back();
    }
    lru_.push_front({key, level, std::move(source)});
    map_[key] = lru_.begin();
}

TrustLevel TaintTracker::get_trust(std::string_view data_id) const noexcept {
    std::lock_guard lk(mu_);
    auto it = map_.find(std::string(data_id));
    if (it == map_.end()) return TrustLevel::Trusted;
    // Move to front (mark as recently accessed)
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->trust;
}

Result<void> TaintTracker::check_flow(const std::string& data_id,
                                      const std::string& target_tool) const {
    static const std::unordered_set<std::string> sensitive_tools = {
        "shell_exec", "code_exec", "send_email",
        "file_write", "db_write", "http_post"
    };

    if (!sensitive_tools.contains(target_tool)) return {};

    auto trust = get_trust(data_id);
    if (trust >= TrustLevel::External) {
        return make_error(ErrorCode::TaintedInput,
            fmt::format("Tainted data (trust={}) flowing into sensitive tool '{}'",
                        static_cast<int>(trust), target_tool));
    }
    return {};
}

void TaintTracker::propagate(const std::string& source_id, const std::string& derived_id) {
    std::lock_guard lk(mu_);
    auto it = map_.find(source_id);
    if (it != map_.end()) {
        // Move source to front
        lru_.splice(lru_.begin(), lru_, it->second);
        TrustLevel trust = it->second->trust;
        // Insert or update derived entry
        std::string derived_source = "derived from " + source_id;
        auto dit = map_.find(derived_id);
        if (dit != map_.end()) {
            dit->second->trust = trust;
            dit->second->source_tag = std::move(derived_source);
            lru_.splice(lru_.begin(), lru_, dit->second);
        } else {
            if (map_.size() >= kMaxTaintEntries) {
                auto& victim = lru_.back();
                map_.erase(victim.data);
                lru_.pop_back();
            }
            lru_.push_front({derived_id, trust, std::move(derived_source)});
            map_[derived_id] = lru_.begin();
        }
    }
}

// ─────────────────────────────────────────────────────────────
// InjectionDetector
// ─────────────────────────────────────────────────────────────

InjectionDetector::InjectionDetector() {
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
    trie_dirty_ = true;
}

void InjectionDetector::add_pattern(std::string pat) {
    std::lock_guard lk(mu_);
    patterns_.push_back(std::move(pat));
    trie_dirty_ = true;
}

bool InjectionDetector::remove_pattern(const std::string &pat) {
    std::lock_guard lk(mu_);
    auto it = std::find(patterns_.begin(), patterns_.end(), pat);
    if (it != patterns_.end()) {
        patterns_.erase(it);
        trie_dirty_ = true;
        return true;
    }
    return false;
}

void InjectionDetector::set_patterns(std::vector<std::string> pats) {
    std::lock_guard lk(mu_);
    patterns_ = std::move(pats);
    trie_dirty_ = true;
}

size_t InjectionDetector::pattern_count() const noexcept {
    std::lock_guard lk(mu_);
    return patterns_.size();
}

void InjectionDetector::build_trie() const {
    // Stub or implementation of AC trie
    // For now we use the O(n*m) scan below which doesn't use the trie.
}

InjectionDetector::DetectionResult InjectionDetector::scan(std::string_view text) const {
    // Input must not exceed kMaxScanLength to prevent truncation bypass attacks
    // Attackers could embed payloads past the truncation point
    if (text.size() > kMaxScanLength) {
        LOG_WARN(fmt::format("[Security] Input too large to scan: {} bytes (limit: {})",
                             text.size(), kMaxScanLength));
        // Treat oversized input as suspicious but not certain injection
        return {true, "input_too_large_for_scan", 0.5f};
    }

    // Strip null bytes, carriage returns, and control characters to prevent bypasses
    std::string clean;
    clean.reserve(text.size());
    for (char c : text) {
        // Strip null bytes, CR, and control characters (except space, tab, newline)
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 0x20 || c == ' ' || c == '\t' || c == '\n') {
            clean += c;
        }
    }

    // Strip zero-width and control characters that can bypass keyword detection
    auto stripped = clean;
    stripped.erase(std::remove_if(stripped.begin(), stripped.end(),
        [](unsigned char c) { return c < 0x20 && c != ' ' && c != '\n' && c != '\t'; }),
        stripped.end());

    // Normalize: lowercase + collapse whitespace (defeat spacing bypass)
    std::string lower;
    lower.reserve(stripped.size());
    bool last_space = false;
    for (char c : stripped) {
      char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!last_space) { lower += ' '; last_space = true; }
      } else {
        lower += lc;
        last_space = false;
      }
    }

    // Helper lambda: check if keyword match at position pos is at word boundary
    auto at_word_boundary = [&](const std::string &haystack, size_t pos, size_t kw_len) -> bool {
        bool left_ok  = (pos == 0 || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1])));
        bool right_ok = (pos + kw_len >= haystack.size() ||
                         !std::isalnum(static_cast<unsigned char>(haystack[pos + kw_len])));
        return left_ok && right_ok;
    };

    // Helper lambda for word-boundary aware search
    auto contains_keyword = [&](const std::string& haystack, const std::string& kw) -> bool {
        size_t pos = 0;
        while ((pos = haystack.find(kw, pos)) != std::string::npos) {
            // Check word boundaries
            if (at_word_boundary(haystack, pos, kw.size())) return true;
            pos += kw.size();
        }
        return false;
    };

    std::lock_guard lk(mu_);
    // O(n*m) scan — acceptable for kMaxScanLength=100KB and ~14 patterns
    // TODO: Switch to Aho-Corasick if pattern count exceeds 50
    for (const auto& pat : patterns_) {
        if (contains_keyword(lower, pat)) {
            return {true, pat, 0.9f};  // early exit on first match
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

// ─────────────────────────────────────────────────────────────
// ExecutionControlLayer
// ─────────────────────────────────────────────────────────────

Result<void> ExecutionControlLayer::before_tool_call(AgentId agent_id,
                                      const std::string& tool_id,
                                      const std::string& args_json,
                                      const std::string& input_data_id) {
    // 1. RBAC 检查
    if (rbac_) {
        Permission required = Permission::ToolReadOnly;
        if (dangerous_tools_.contains(tool_id))
            required = Permission::ToolDangerous;
        else if (write_tools_.contains(tool_id))
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
        audit(msg);
        LOG_WARN(msg);
        }
        return make_error(ErrorCode::InjectionDetected,
            fmt::format("Prompt injection detected: {}", det.matched_pattern));
    }

    // 4. 高风险操作需要人工批准
    if (human_approval_ && critical_tools_.contains(tool_id)) {
        bool approved = human_approval_(agent_id, tool_id, args_json);
        if (!approved) {
            return make_error(ErrorCode::PermissionDenied,
                fmt::format("Human rejected tool call: {}", tool_id));
        }
    }

    audit(fmt::format(
        "[OK] agent={} tool={} args_len={}", agent_id, tool_id, args_json.size()));
    return {};
}

Result<void> ExecutionControlLayer::scan_llm_output(AgentId agent_id,
                                     const kernel::LLMResponse& response) {
    // 扫描文本输出中的注入尝试
    auto det = injection_detector_.scan(response.content);
    if (det.is_injection && det.confidence > 0.8f) {
        audit(fmt::format(
            "[WARN] LLM output injection suspicion: agent={} pattern='{}'",
            agent_id, det.matched_pattern));
        // 警告但不阻断（低置信度时仅记录）
    }
    return {};
}

void ExecutionControlLayer::audit(std::string_view event) {
    std::lock_guard<std::mutex> lock(audit_mu_);
    // Format: [ISO8601_timestamp] event
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    std::string entry = fmt::format("[{}] {}", ts, event);

    if (audit_log_.size() >= kAuditLogCapacity) {
        // Drop oldest half to make room
        audit_log_.erase(audit_log_.begin(),
                         audit_log_.begin() + kAuditLogCapacity / 2);
    }
    audit_log_.push_back(std::move(entry));
}

} // namespace agentos::security
