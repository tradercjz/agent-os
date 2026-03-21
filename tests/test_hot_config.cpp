#include <agentos/core/hot_config.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace agentos;

namespace {

// Helper to create a temp config file
class TempConfigFile {
public:
    TempConfigFile() {
        path_ = std::filesystem::temp_directory_path() /
                fmt::format("hot_config_test_{}.json",
                            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    ~TempConfigFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    void write(const nlohmann::json& j) {
        std::ofstream ofs(path_);
        ofs << j.dump(2);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

// 1. GetReturnsDefaultForMissingKey
TEST(HotConfigTest, GetReturnsDefaultForMissingKey) {
    HotConfig cfg;
    EXPECT_EQ(cfg.get<int>("nonexistent", 42), 42);
    EXPECT_EQ(cfg.get<std::string>("missing", "default"), "default");
    EXPECT_DOUBLE_EQ(cfg.get<double>("nope", 3.14), 3.14);
}

// 2. SetAndGet
TEST(HotConfigTest, SetAndGet) {
    HotConfig cfg;
    auto r = cfg.set("tpm_limit", 5000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(cfg.get<int>("tpm_limit", 0), 5000);

    r = cfg.set("log_level", "debug");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(cfg.get<std::string>("log_level", ""), "debug");
}

// 3. ReloadFromFile
TEST(HotConfigTest, ReloadFromFile) {
    TempConfigFile tmp;
    nlohmann::json j;
    j["tpm_limit"] = 9999;
    j["log_level"] = "info";
    tmp.write(j);

    HotConfig cfg(tmp.path().string());
    EXPECT_EQ(cfg.get<int>("tpm_limit", 0), 9999);
    EXPECT_EQ(cfg.get<std::string>("log_level", ""), "info");

    // Modify file and reload
    j["tpm_limit"] = 1234;
    tmp.write(j);
    auto r = cfg.reload();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(cfg.get<int>("tpm_limit", 0), 1234);
}

// 4. ReloadRejectsInvalidFile
TEST(HotConfigTest, ReloadRejectsInvalidFile) {
    TempConfigFile tmp;
    {
        std::ofstream ofs(tmp.path());
        ofs << "not valid json {{{";
    }

    HotConfig cfg(tmp.path().string());
    auto r = cfg.reload();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

// 5. ObserverNotifiedOnChange
TEST(HotConfigTest, ObserverNotifiedOnChange) {
    HotConfig cfg;
    int notify_count = 0;
    nlohmann::json received_value;

    cfg.on_change("tpm_limit", [&](const std::string& /*key*/, const nlohmann::json& v) {
        ++notify_count;
        received_value = v;
    });

    (void)cfg.set("tpm_limit", 2000);
    EXPECT_EQ(notify_count, 1);
    EXPECT_EQ(received_value, 2000);

    // Set same value again - should still notify (value may differ from prior state)
    (void)cfg.set("tpm_limit", 3000);
    EXPECT_EQ(notify_count, 2);
    EXPECT_EQ(received_value, 3000);
}

// 6. ObserverCanCallGetWithoutDeadlock
TEST(HotConfigTest, ObserverCanCallGetWithoutDeadlock) {
    HotConfig cfg;
    int observed_tpm = 0;

    cfg.on_change("tpm_limit", [&](const std::string&, const nlohmann::json&) {
        // This calls get() from inside the callback — must not deadlock
        observed_tpm = cfg.get<int>("tpm_limit", 0);
    });

    // Use a timeout to detect deadlock
    std::atomic<bool> done{false};
    std::thread t([&] {
        (void)cfg.set("tpm_limit", 7777);
        done.store(true, std::memory_order_release);
    });

    // Wait up to 2 seconds for the set to complete
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(done.load()) << "Deadlock detected: callback calling get() blocked";
    EXPECT_EQ(observed_tpm, 7777);
    t.join();
}

// 7. ValidationRejectsNegativeTPM
TEST(HotConfigTest, ValidationRejectsNegativeTPM) {
    HotConfig cfg;
    auto r = cfg.set("tpm_limit", -5);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);

    // Zero is also invalid
    r = cfg.set("tpm_limit", 0);
    EXPECT_FALSE(r.has_value());

    // String is invalid for tpm_limit
    r = cfg.set("tpm_limit", "not_a_number");
    EXPECT_FALSE(r.has_value());
}

// 8. ConcurrentGetSetNoRace
TEST(HotConfigTest, ConcurrentGetSetNoRace) {
    HotConfig cfg;
    (void)cfg.set("tpm_limit", 1000);

    std::atomic<bool> stop{false};
    constexpr int kIterations = 5000;

    // Writer thread
    std::thread writer([&] {
        for (int i = 1; i <= kIterations && !stop.load(); ++i) {
            (void)cfg.set("tpm_limit", i);
        }
        stop.store(true);
    });

    // Reader thread
    std::thread reader([&] {
        while (!stop.load()) {
            int val = cfg.get<int>("tpm_limit", -1);
            EXPECT_GE(val, 0);
        }
    });

    writer.join();
    reader.join();
    // If we reach here without crash/TSAN error, the test passes
}

// Validation edge cases
TEST(HotConfigTest, ValidationLogLevel) {
    HotConfig cfg;
    EXPECT_TRUE(cfg.set("log_level", "debug").has_value());
    EXPECT_TRUE(cfg.set("log_level", "info").has_value());
    EXPECT_TRUE(cfg.set("log_level", "warn").has_value());
    EXPECT_TRUE(cfg.set("log_level", "error").has_value());
    EXPECT_TRUE(cfg.set("log_level", "off").has_value());
    EXPECT_FALSE(cfg.set("log_level", "verbose").has_value());
    EXPECT_FALSE(cfg.set("log_level", 42).has_value());
}

TEST(HotConfigTest, ValidationInjectionThreshold) {
    HotConfig cfg;
    EXPECT_TRUE(cfg.set("injection_threshold", 0.5).has_value());
    EXPECT_TRUE(cfg.set("injection_threshold", 0.0).has_value());
    EXPECT_TRUE(cfg.set("injection_threshold", 1.0).has_value());
    EXPECT_FALSE(cfg.set("injection_threshold", -0.1).has_value());
    EXPECT_FALSE(cfg.set("injection_threshold", 1.1).has_value());
    EXPECT_FALSE(cfg.set("injection_threshold", "high").has_value());
}

TEST(HotConfigTest, ValidationBooleanField) {
    HotConfig cfg;
    EXPECT_TRUE(cfg.set("injection_detection_enabled", true).has_value());
    EXPECT_TRUE(cfg.set("injection_detection_enabled", false).has_value());
    EXPECT_FALSE(cfg.set("injection_detection_enabled", 1).has_value());
    EXPECT_FALSE(cfg.set("injection_detection_enabled", "true").has_value());
}

TEST(HotConfigTest, UnknownKeysAllowed) {
    HotConfig cfg;
    EXPECT_TRUE(cfg.set("custom_feature_flag", true).has_value());
    EXPECT_TRUE(cfg.set("my_app_setting", "hello").has_value());
    EXPECT_EQ(cfg.get<std::string>("my_app_setting", ""), "hello");
}

TEST(HotConfigTest, ReloadNoPathReturnsError) {
    HotConfig cfg;
    auto r = cfg.reload();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(HotConfigTest, ReloadValidatesAllKeys) {
    TempConfigFile tmp;
    nlohmann::json j;
    j["tpm_limit"] = -100;
    j["log_level"] = "info";
    tmp.write(j);

    HotConfig cfg;
    // Manually set path and reload — constructor would have tried reload already
    // So create fresh with bad config
    HotConfig cfg2(tmp.path().string());
    // The invalid tpm_limit should have caused reload to fail in constructor
    // but constructor does best-effort, so let's explicitly reload
    auto r = cfg2.reload();
    EXPECT_FALSE(r.has_value());
}
