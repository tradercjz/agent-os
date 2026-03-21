#pragma once
// ============================================================
// AgentOS :: Memory :: SharedMemory
// Thread-safe shared key-value store for multi-agent collaboration
// ============================================================
#include <agentos/core/types.hpp>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos::memory {

class SharedMemory : private NonCopyable {
public:
    struct Entry {
        std::string value;
        AgentId writer{0};
        TimePoint written_at;
    };

    // Write a value (any agent can write)
    void put(const std::string& key, const std::string& value, AgentId writer = 0) {
        std::unique_lock lk(mu_);
        store_[key] = Entry{value, writer, now()};
    }

    // Read a value
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
        std::shared_lock lk(mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        return it->second.value;
    }

    // Delete a key
    bool remove(const std::string& key) {
        std::unique_lock lk(mu_);
        return store_.erase(key) > 0;
    }

    // List all keys
    [[nodiscard]] std::vector<std::string> keys() const {
        std::shared_lock lk(mu_);
        std::vector<std::string> result;
        result.reserve(store_.size());
        for (const auto& [k, _] : store_) {
            result.push_back(k);
        }
        return result;
    }

    // Check existence
    [[nodiscard]] bool contains(const std::string& key) const {
        std::shared_lock lk(mu_);
        return store_.contains(key);
    }

    // Size
    [[nodiscard]] size_t size() const {
        std::shared_lock lk(mu_);
        return store_.size();
    }

    // Clear all
    void clear() {
        std::unique_lock lk(mu_);
        store_.clear();
    }

    // Get with metadata (who wrote it, when)
    [[nodiscard]] std::optional<Entry> get_entry(const std::string& key) const {
        std::shared_lock lk(mu_);
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, Entry> store_;
};

} // namespace agentos::memory
