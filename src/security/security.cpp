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
    pattern_set_.insert(patterns_.begin(), patterns_.end());
    trie_dirty_ = true;
}

void InjectionDetector::add_pattern(std::string pat) {
    std::lock_guard lk(mu_);
    if (pattern_set_.insert(pat).second) {
        patterns_.push_back(std::move(pat));
        trie_dirty_ = true;
    }
}

bool InjectionDetector::remove_pattern(const std::string &pat) {
    std::lock_guard lk(mu_);
    if (pattern_set_.erase(pat) == 0) {
        return false;
    }
    auto it = std::find(patterns_.begin(), patterns_.end(), pat);
    if (it != patterns_.end()) {
        patterns_.erase(it);
    }
    trie_dirty_ = true;
    return true;
}

void InjectionDetector::set_patterns(std::vector<std::string> pats) {
    std::lock_guard lk(mu_);
    patterns_ = std::move(pats);
    pattern_set_.clear();
    pattern_set_.insert(patterns_.begin(), patterns_.end());
    trie_dirty_ = true;
}

size_t InjectionDetector::pattern_count() const noexcept {
    std::lock_guard lk(mu_);
    return patterns_.size();
}

void InjectionDetector::build_trie() const {
    // Build Aho-Corasick automaton from patterns_
    // Caller must hold mu_
    trie_.clear();
    trie_.push_back(ACTrieNode{}); // root node at index 0

    // Phase 1: Insert all patterns into the trie (goto function)
    for (size_t pi = 0; pi < patterns_.size(); ++pi) {
        int cur = 0;
        for (char c : patterns_[pi]) {
            auto it = trie_[static_cast<size_t>(cur)].children.find(c);
            if (it == trie_[static_cast<size_t>(cur)].children.end()) {
                auto next = static_cast<int>(trie_.size());
                trie_[static_cast<size_t>(cur)].children[c] = next;
                trie_.push_back(ACTrieNode{});
                cur = next;
            } else {
                cur = it->second;
            }
        }
        trie_[static_cast<size_t>(cur)].match_indices.push_back(pi);
    }

    // Phase 2: Build failure links via BFS
    std::queue<int> bfs;
    // Initialize depth-1 nodes: their fail links point to root
    for (auto& [ch, child_idx] : trie_[0].children) {
        trie_[static_cast<size_t>(child_idx)].fail = 0;
        bfs.push(child_idx);
    }

    while (!bfs.empty()) {
        int u = bfs.front();
        bfs.pop();
        for (auto& [ch, v] : trie_[static_cast<size_t>(u)].children) {
            // Walk up failure links to find longest proper suffix that is a prefix
            int f = trie_[static_cast<size_t>(u)].fail;
            while (f != 0 &&
                   trie_[static_cast<size_t>(f)].children.find(ch) ==
                       trie_[static_cast<size_t>(f)].children.end()) {
                f = trie_[static_cast<size_t>(f)].fail;
            }
            auto fit = trie_[static_cast<size_t>(f)].children.find(ch);
            if (fit != trie_[static_cast<size_t>(f)].children.end() && fit->second != v) {
                trie_[static_cast<size_t>(v)].fail = fit->second;
            } else {
                trie_[static_cast<size_t>(v)].fail = 0;
            }
            // Merge output from the fail node (dictionary suffix links)
            auto& fail_matches = trie_[static_cast<size_t>(trie_[static_cast<size_t>(v)].fail)].match_indices;
            for (auto idx : fail_matches) {
                trie_[static_cast<size_t>(v)].match_indices.push_back(idx);
            }
            bfs.push(v);
        }
    }

    trie_dirty_ = false;
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

    if (patterns_.size() > 50) {
        // Use Aho-Corasick for large pattern sets: O(n + m + z) total
        if (trie_dirty_) {
            build_trie();
        }

        int cur = 0;
        for (size_t i = 0; i < lower.size(); ++i) {
            char c = lower[i];
            // Follow failure links until we find a transition or reach root
            while (cur != 0 &&
                   trie_[static_cast<size_t>(cur)].children.find(c) ==
                       trie_[static_cast<size_t>(cur)].children.end()) {
                cur = trie_[static_cast<size_t>(cur)].fail;
            }
            auto it = trie_[static_cast<size_t>(cur)].children.find(c);
            if (it != trie_[static_cast<size_t>(cur)].children.end()) {
                cur = it->second;
            }
            // Check all patterns that end at this position
            for (size_t pi : trie_[static_cast<size_t>(cur)].match_indices) {
                const auto& pat = patterns_[pi];
                // Verify word boundary: match ends at position i, starts at i+1-pat.size()
                size_t match_start = i + 1 - pat.size();
                if (at_word_boundary(lower, match_start, pat.size())) {
                    return {true, pat, 0.9f};
                }
            }
        }
    } else {
        // O(n*m) linear scan — acceptable for small pattern sets
        for (const auto& pat : patterns_) {
            if (contains_keyword(lower, pat)) {
                return {true, pat, 0.9f};
            }
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
