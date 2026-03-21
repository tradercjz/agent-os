// ============================================================
// AgentOS :: HotConfig Implementation
// ============================================================
#include <agentos/core/hot_config.hpp>
#include <agentos/core/logger.hpp>

#ifdef __APPLE__
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#elif __linux__
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#endif

#include <set>

namespace agentos {

HotConfig::HotConfig(const std::string& config_path)
    : config_path_(config_path) {
    if (!config_path_.empty()) {
        (void)reload();
    }
}

Result<void> HotConfig::set(const std::string& key, const nlohmann::json& value) {
    auto v = validate(key, value);
    if (!v) return v;

    nlohmann::json new_config;
    {
        std::shared_lock lk(mu_);
        new_config = config_;
    }
    new_config[key] = value;
    apply_and_notify(new_config);
    return {};
}

Result<void> HotConfig::reload() {
    if (config_path_.empty()) {
        return make_error(ErrorCode::InvalidArgument, "HotConfig: no config path set");
    }

    std::ifstream ifs(config_path_);
    if (!ifs) {
        return make_error(ErrorCode::NotFound,
            fmt::format("HotConfig: cannot open '{}'", config_path_));
    }

    nlohmann::json new_config;
    try {
        ifs >> new_config;
    } catch (const nlohmann::json::exception& e) {
        return make_error(ErrorCode::InvalidArgument,
            fmt::format("HotConfig: parse error in '{}': {}", config_path_, e.what()));
    }

    // Validate all known keys
    for (auto it = new_config.begin(); it != new_config.end(); ++it) {
        auto v = validate(it.key(), it.value());
        if (!v) return v;
    }

    apply_and_notify(new_config);
    return {};
}

void HotConfig::on_change(const std::string& key, ConfigChangeCallback cb) {
    std::lock_guard lk(observers_mu_);
    observers_[key].push_back(std::move(cb));
}

Result<void> HotConfig::validate(const std::string& key, const nlohmann::json& value) const {
    static const std::set<std::string> valid_log_levels = {
        "debug", "info", "warn", "error", "off"
    };

    if (key == "tpm_limit") {
        if (!value.is_number_integer() || value.get<int64_t>() <= 0) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: tpm_limit must be a positive integer");
        }
    } else if (key == "log_level") {
        if (!value.is_string() || valid_log_levels.find(value.get<std::string>()) == valid_log_levels.end()) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: log_level must be one of {debug, info, warn, error, off}");
        }
    } else if (key == "injection_threshold") {
        if (!value.is_number()) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: injection_threshold must be a number");
        }
        double v = value.get<double>();
        if (v < 0.0 || v > 1.0) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: injection_threshold must be in [0.0, 1.0]");
        }
    } else if (key == "injection_detection_enabled") {
        if (!value.is_boolean()) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: injection_detection_enabled must be a boolean");
        }
    } else if (key == "audit_rotation_max_entries" || key == "context_default_token_limit") {
        if (!value.is_number_integer() || value.get<int64_t>() <= 0) {
            return make_error(ErrorCode::InvalidArgument,
                fmt::format("HotConfig: {} must be a positive integer", key));
        }
    } else if (key == "audit_rotation_max_age_hours") {
        if (!value.is_number_integer() || value.get<int64_t>() < 0) {
            return make_error(ErrorCode::InvalidArgument,
                "HotConfig: audit_rotation_max_age_hours must be a non-negative integer");
        }
    }
    // Unknown keys: allowed (forward-compatible)

    return {};
}

void HotConfig::apply_and_notify(const nlohmann::json& new_config) {
    // 1. Diff and swap under exclusive lock
    std::vector<std::string> changed_keys;
    {
        std::unique_lock lk(mu_);
        for (auto it = new_config.begin(); it != new_config.end(); ++it) {
            if (!config_.contains(it.key()) || config_[it.key()] != it.value()) {
                changed_keys.push_back(it.key());
            }
        }
        config_ = new_config;
    }
    // mu_ is released here

    // 2. Invoke callbacks outside mu_ to prevent deadlock
    std::lock_guard obs_lk(observers_mu_);
    for (const auto& key : changed_keys) {
        auto obs_it = observers_.find(key);
        if (obs_it != observers_.end()) {
            for (auto& cb : obs_it->second) {
                cb(key, new_config[key]);
            }
        }
    }
}

void HotConfig::start_watching() {
    if (config_path_.empty()) return;
    watcher_thread_ = std::jthread([this](std::stop_token st) {
        file_watch_loop(std::move(st));
    });
}

void HotConfig::stop_watching() {
    if (watcher_thread_.joinable()) {
        watcher_thread_.request_stop();
        watcher_thread_.join();
    }
}

void HotConfig::file_watch_loop(std::stop_token st) {
#ifdef __APPLE__
    int fd = ::open(config_path_.c_str(), O_RDONLY | O_EVTONLY);
    if (fd < 0) {
        LOG_WARN(fmt::format("HotConfig: cannot open '{}' for watching", config_path_));
        return;
    }

    int kq = ::kqueue();
    if (kq < 0) {
        ::close(fd);
        LOG_WARN("HotConfig: kqueue() failed");
        return;
    }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
    if (::kevent(kq, &change, 1, nullptr, 0, nullptr) < 0) {
        ::close(kq);
        ::close(fd);
        LOG_WARN("HotConfig: kevent registration failed");
        return;
    }

    TimePoint last_reload = Clock::now();

    while (!st.stop_requested()) {
        struct kevent event;
        struct timespec timeout = {1, 0}; // 1s poll timeout
        int nev = ::kevent(kq, nullptr, 0, &event, 1, &timeout);

        if (st.stop_requested()) break;

        if (nev > 0) {
            // Debounce: skip if last reload was < 200ms ago
            auto now_tp = Clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_reload);
            if (elapsed.count() >= 200) {
                auto result = reload();
                if (result) {
                    LOG_INFO("HotConfig: reloaded config from file");
                } else {
                    LOG_WARN(fmt::format("HotConfig: reload failed: {}", result.error().message));
                }
                last_reload = Clock::now();
            }

            // If file was deleted/renamed, re-open
            if (event.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                ::close(fd);
                fd = ::open(config_path_.c_str(), O_RDONLY | O_EVTONLY);
                if (fd < 0) break;
                EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
                if (::kevent(kq, &change, 1, nullptr, 0, nullptr) < 0) break;
            }
        }
    }

    ::close(kq);
    ::close(fd);

#elif __linux__
    int ifd = ::inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        LOG_WARN("HotConfig: inotify_init1 failed");
        return;
    }

    int wd = ::inotify_add_watch(ifd, config_path_.c_str(), IN_MODIFY | IN_CLOSE_WRITE);
    if (wd < 0) {
        ::close(ifd);
        LOG_WARN(fmt::format("HotConfig: inotify_add_watch failed for '{}'", config_path_));
        return;
    }

    TimePoint last_reload = Clock::now();

    while (!st.stop_requested()) {
        struct pollfd pfd = {ifd, POLLIN, 0};
        int ret = ::poll(&pfd, 1, 1000); // 1s timeout

        if (st.stop_requested()) break;

        if (ret > 0 && (pfd.revents & POLLIN)) {
            // Drain inotify events
            char buf[4096];
            (void)::read(ifd, buf, sizeof(buf));

            auto now_tp = Clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_reload);
            if (elapsed.count() >= 200) {
                auto result = reload();
                if (result) {
                    LOG_INFO("HotConfig: reloaded config from file");
                } else {
                    LOG_WARN(fmt::format("HotConfig: reload failed: {}", result.error().message));
                }
                last_reload = Clock::now();
            }
        }
    }

    ::close(ifd);

#else
    // Fallback: poll-based file watching
    TimePoint last_reload = Clock::now();
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (st.stop_requested()) break;

        auto now_tp = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_reload);
        if (elapsed.count() >= 2000) {
            auto result = reload();
            if (result) {
                LOG_INFO("HotConfig: reloaded config from file");
            }
            last_reload = Clock::now();
        }
    }
#endif
}

} // namespace agentos
