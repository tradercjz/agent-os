#pragma once
// ============================================================
// AgentOS :: HotConfig
// Live-reloadable configuration with file watching + validation
// ============================================================
#include <agentos/core/types.hpp>
#include <fstream>
#include <functional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agentos {

using ConfigChangeCallback = std::function<void(const std::string& key, const nlohmann::json& value)>;

class HotConfig : private NonCopyable {
public:
    explicit HotConfig(const std::string& config_path = "");

    template<typename T>
    T get(const std::string& key, const T& default_val) const {
        std::shared_lock lk(mu_);
        if (config_.contains(key)) {
            try { return config_[key].get<T>(); }
            catch (const nlohmann::json::exception&) { return default_val; }
        }
        return default_val;
    }

    Result<void> set(const std::string& key, const nlohmann::json& value);
    Result<void> reload();

    void on_change(const std::string& key, ConfigChangeCallback cb);

    void start_watching();
    void stop_watching();

private:
    nlohmann::json config_;
    mutable std::shared_mutex mu_;
    std::string config_path_;
    std::unordered_map<std::string, std::vector<ConfigChangeCallback>> observers_;
    std::mutex observers_mu_;
    std::jthread watcher_thread_;

    Result<void> validate(const std::string& key, const nlohmann::json& value) const;
    void apply_and_notify(const nlohmann::json& new_config);
    void file_watch_loop(std::stop_token st);
};

} // namespace agentos
