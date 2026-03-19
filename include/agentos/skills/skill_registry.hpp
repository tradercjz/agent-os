#pragma once
// ============================================================
// AgentOS :: Skill Registry
// 基于关键词匹配的领域知识自动激活系统
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos::skills {

// ── Skill definition ──

using ToolFn = std::function<tools::ToolResult(const tools::ParsedArgs&, std::stop_token)>;

struct SkillDef {
    std::string name;
    std::string description;
    std::vector<std::string> keywords;
    std::vector<tools::ToolSchema> tools;
    std::vector<ToolFn> tool_fns;          // corresponding implementations
    std::string prompt_injection;          // extra system prompt when active
};

// ── Match result ──

struct SkillMatch {
    const SkillDef* skill;
    size_t hit_count;
};

// ── Skill Registry ──

class SkillRegistry {
public:
    void register_skill(SkillDef skill) {
        std::lock_guard lk(mu_);
        skills_[skill.name] = std::move(skill);
    }

    void remove_skill(const std::string& name) {
        std::lock_guard lk(mu_);
        skills_.erase(name);
    }

    /// Match skills against a task description (case-insensitive keyword scan)
    std::vector<SkillMatch> match(const std::string& task_description) const {
        std::lock_guard lk(mu_);
        std::string lower_desc = to_lower(task_description);

        std::vector<SkillMatch> matches;
        for (const auto& [_, skill] : skills_) {
            size_t hits = 0;
            for (const auto& kw : skill.keywords) {
                if (lower_desc.find(to_lower(kw)) != std::string::npos) {
                    ++hits;
                }
            }
            if (hits > 0) {
                matches.push_back({&skill, hits});
            }
        }

        // Sort by hit count descending
        std::sort(matches.begin(), matches.end(),
                  [](const SkillMatch& a, const SkillMatch& b) {
                      return a.hit_count > b.hit_count;
                  });
        return matches;
    }

    /// Activate a skill: register its tools with the tool registry
    Result<void> activate(const std::string& skill_name,
                          tools::ToolRegistry& registry,
                          AgentId agent_id) {
        std::lock_guard lk(mu_);
        auto it = skills_.find(skill_name);
        if (it == skills_.end()) {
            return make_error(ErrorCode::NotFound,
                              fmt::format("Skill '{}' not found", skill_name));
        }

        const auto& skill = it->second;

        // Register each tool
        for (size_t i = 0; i < skill.tools.size() && i < skill.tool_fns.size(); ++i) {
            registry.register_fn(skill.tools[i], skill.tool_fns[i]);
        }

        active_[agent_id].insert(skill_name);
        return {};
    }

    /// Deactivate a skill: unregister its tools
    Result<void> deactivate(const std::string& skill_name,
                            tools::ToolRegistry& registry,
                            AgentId agent_id) {
        std::lock_guard lk(mu_);
        auto it = skills_.find(skill_name);
        if (it == skills_.end()) {
            return make_error(ErrorCode::NotFound,
                              fmt::format("Skill '{}' not found", skill_name));
        }

        const auto& skill = it->second;
        for (const auto& tool : skill.tools) {
            registry.unregister(tool.id);
        }

        auto ait = active_.find(agent_id);
        if (ait != active_.end()) {
            ait->second.erase(skill_name);
        }
        return {};
    }

    /// Get active skills for an agent
    std::vector<std::string> active_skills(AgentId agent_id) const {
        std::lock_guard lk(mu_);
        auto it = active_.find(agent_id);
        if (it == active_.end()) return {};
        return {it->second.begin(), it->second.end()};
    }

    /// Get the prompt injection for a skill
    std::string get_prompt(const std::string& skill_name) const {
        std::lock_guard lk(mu_);
        auto it = skills_.find(skill_name);
        if (it == skills_.end()) return "";
        return it->second.prompt_injection;
    }

    size_t skill_count() const {
        std::lock_guard lk(mu_);
        return skills_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, SkillDef> skills_;
    std::unordered_map<AgentId, std::unordered_set<std::string>> active_;

    static std::string to_lower(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }
};

} // namespace agentos::skills
