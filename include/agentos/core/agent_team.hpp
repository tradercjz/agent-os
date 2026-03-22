#pragma once
// ============================================================
// AgentOS :: Core :: Agent Teams / Groups
// Organize agents into named teams with optional policies
// ============================================================
#include <agentos/core/types.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentos {

struct TeamPolicy {
    uint32_t max_members{0};  // 0 = unlimited
    std::string description;
};

class AgentTeam : private NonCopyable {
public:
    explicit AgentTeam(std::string name, TeamPolicy policy = {})
        : name_(std::move(name)), policy_(std::move(policy)) {}

    bool add_member(AgentId id) {
        std::lock_guard lk(mu_);
        if (policy_.max_members > 0 && members_.size() >= policy_.max_members) return false;
        return members_.insert(id).second;
    }

    bool remove_member(AgentId id) {
        std::lock_guard lk(mu_);
        return members_.erase(id) > 0;
    }

    bool has_member(AgentId id) const {
        std::lock_guard lk(mu_);
        return members_.contains(id);
    }

    std::vector<AgentId> members() const {
        std::lock_guard lk(mu_);
        return {members_.begin(), members_.end()};
    }

    size_t size() const {
        std::lock_guard lk(mu_);
        return members_.size();
    }

    const std::string& name() const { return name_; }
    const TeamPolicy& policy() const { return policy_; }

private:
    std::string name_;
    TeamPolicy policy_;
    mutable std::mutex mu_;
    std::unordered_set<AgentId> members_;
};

class TeamManager : private NonCopyable {
public:
    Result<void> create_team(std::string name, TeamPolicy policy = {}) {
        std::lock_guard lk(mu_);
        if (teams_.contains(name)) {
            return make_error(ErrorCode::AlreadyExists, "Team already exists: " + name);
        }
        teams_[name] = std::make_unique<AgentTeam>(name, std::move(policy));
        return {};
    }

    Result<void> remove_team(const std::string& name) {
        std::lock_guard lk(mu_);
        if (teams_.erase(name) == 0) {
            return make_error(ErrorCode::NotFound, "Team not found: " + name);
        }
        return {};
    }

    AgentTeam* find_team(const std::string& name) {
        std::lock_guard lk(mu_);
        auto it = teams_.find(name);
        return it != teams_.end() ? it->second.get() : nullptr;
    }

    std::vector<std::string> list_teams() const {
        std::lock_guard lk(mu_);
        std::vector<std::string> names;
        for (auto& [n, _] : teams_) names.push_back(n);
        return names;
    }

    Result<void> assign(AgentId id, const std::string& team) {
        std::lock_guard lk(mu_);
        auto it = teams_.find(team);
        if (it == teams_.end()) return make_error(ErrorCode::NotFound, "Team not found");
        if (!it->second->add_member(id)) {
            return make_error(ErrorCode::ResourceExhausted, "Team is full");
        }
        return {};
    }

    Result<void> unassign(AgentId id, const std::string& team) {
        std::lock_guard lk(mu_);
        auto it = teams_.find(team);
        if (it == teams_.end()) return make_error(ErrorCode::NotFound, "Team not found");
        it->second->remove_member(id);
        return {};
    }

    std::vector<std::string> teams_of(AgentId id) const {
        std::lock_guard lk(mu_);
        std::vector<std::string> result;
        for (auto& [name, team] : teams_) {
            if (team->has_member(id)) result.push_back(name);
        }
        return result;
    }

    size_t team_count() const {
        std::lock_guard lk(mu_);
        return teams_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<AgentTeam>> teams_;
};

} // namespace agentos
