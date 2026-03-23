// ============================================================
// Tests for PluginManager enhancements:
//   scan_directory, reload, dependency ordering, PluginInfo
// ============================================================
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace agentos;
namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// § scan_directory tests
// ─────────────────────────────────────────────────────────────

TEST(PluginEnhancedTest, ScanDirectoryNonexistentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->plugins().scan_directory("/nonexistent_dir_abc123", *os);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(PluginEnhancedTest, ScanDirectoryEmptyDirSucceeds) {
    auto os = quickstart_mock();

    // Create a temporary empty directory
    auto tmp = fs::temp_directory_path() / "agentos_plugin_test_empty";
    fs::create_directories(tmp);

    auto r = os->plugins().scan_directory(tmp, *os);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(os->plugins().count(), 0u);

    fs::remove_all(tmp);
}

TEST(PluginEnhancedTest, ScanDirectoryIgnoresNonPluginFiles) {
    auto os = quickstart_mock();

    auto tmp = fs::temp_directory_path() / "agentos_plugin_test_mixed";
    fs::create_directories(tmp);

    // Create a non-plugin file (e.g., .txt)
    {
        std::ofstream ofs(tmp / "readme.txt");
        ofs << "not a plugin";
    }

    auto r = os->plugins().scan_directory(tmp, *os);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(os->plugins().count(), 0u);

    fs::remove_all(tmp);
}

TEST(PluginEnhancedTest, ScanDirectoryLoadsRealPlugins) {
    auto os = quickstart_mock();

    // Build a temp dir with a symlink to our test plugin
    std::string plugin_file = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
    if (!fs::exists(plugin_file)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_file;
    }

    auto tmp = fs::temp_directory_path() / "agentos_plugin_test_scan";
    fs::create_directories(tmp);

    // Copy (or symlink) the real plugin into the temp dir
    auto dest = tmp / fs::path(plugin_file).filename();
    fs::copy_file(plugin_file, dest, fs::copy_options::overwrite_existing);

    auto r = os->plugins().scan_directory(tmp, *os);
    EXPECT_TRUE(r.has_value());
    EXPECT_GE(os->plugins().count(), 1u);

    fs::remove_all(tmp);
}

// ─────────────────────────────────────────────────────────────
// § reload tests
// ─────────────────────────────────────────────────────────────

TEST(PluginEnhancedTest, ReloadNonexistentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->plugins().reload("nonexistent_plugin", *os);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(PluginEnhancedTest, ReloadPlugin) {
    auto os = quickstart_mock();

    std::string plugin_path = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
    if (!fs::exists(plugin_path)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_path;
    }

    auto lr = os->plugins().load(plugin_path, *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    EXPECT_EQ(os->plugins().count(), 1u);
    auto* p = os->plugins().find("sample");
    ASSERT_NE(p, nullptr);

    // Reload — should unload then re-load from same path
    auto rr = os->plugins().reload("sample", *os);
    ASSERT_TRUE(rr.has_value()) << rr.error().message;

    EXPECT_EQ(os->plugins().count(), 1u);
    auto* p2 = os->plugins().find("sample");
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p2->name(), "sample");
    EXPECT_EQ(p2->version(), "1.0.0");
}

// ─────────────────────────────────────────────────────────────
// § PluginInfo tests
// ─────────────────────────────────────────────────────────────

TEST(PluginEnhancedTest, InfoNonexistentReturnsNullopt) {
    PluginManager mgr;
    auto pi = mgr.info("nonexistent");
    EXPECT_FALSE(pi.has_value());
}

TEST(PluginEnhancedTest, InfoReturnsCorrectMetadata) {
    auto os = quickstart_mock();

    std::string plugin_path = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
    if (!fs::exists(plugin_path)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_path;
    }

    auto lr = os->plugins().load(plugin_path, *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    auto pi = os->plugins().info("sample");
    ASSERT_TRUE(pi.has_value());
    EXPECT_EQ(pi->name, "sample");
    EXPECT_EQ(pi->version, "1.0.0");
    EXPECT_FALSE(pi->path.empty());

    // loaded_at should be recent (within last 60 seconds)
    auto elapsed = std::chrono::system_clock::now() - pi->loaded_at;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 60);
}

// ─────────────────────────────────────────────────────────────
// § Dependency ordering tests
// ─────────────────────────────────────────────────────────────

TEST(PluginEnhancedTest, LoadOrderedEmptyListSucceeds) {
    auto os = quickstart_mock();
    auto r = os->plugins().load_ordered({}, *os);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(os->plugins().count(), 0u);
}

TEST(PluginEnhancedTest, LoadOrderedNonexistentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->plugins().load_ordered({"/nonexistent/plugin.dylib"}, *os);
    EXPECT_FALSE(r.has_value());
}

TEST(PluginEnhancedTest, LoadOrderedSinglePlugin) {
    auto os = quickstart_mock();

    std::string plugin_path = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
    if (!fs::exists(plugin_path)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_path;
    }

    auto r = os->plugins().load_ordered({plugin_path}, *os);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(os->plugins().count(), 1u);
}

// ─────────────────────────────────────────────────────────────
// § IPlugin::dependencies() default
// ─────────────────────────────────────────────────────────────

namespace {
class TestPluginWithDeps : public IPlugin {
public:
    std::string name() const override { return "test_with_deps"; }
    std::string version() const override { return "0.1.0"; }
    Result<void> init(AgentOS&) override { return {}; }
    void shutdown() override {}
    std::vector<std::string> dependencies() const override {
        return {"dep_a", "dep_b"};
    }
};

class TestPluginNoDeps : public IPlugin {
public:
    std::string name() const override { return "test_no_deps"; }
    std::string version() const override { return "0.2.0"; }
    Result<void> init(AgentOS&) override { return {}; }
    void shutdown() override {}
    // uses default dependencies() — empty
};
} // anon namespace

TEST(PluginEnhancedTest, DefaultDependenciesEmpty) {
    TestPluginNoDeps p;
    EXPECT_TRUE(p.dependencies().empty());
}

TEST(PluginEnhancedTest, CustomDependenciesReturned) {
    TestPluginWithDeps p;
    auto deps = p.dependencies();
    ASSERT_EQ(deps.size(), 2u);
    EXPECT_EQ(deps[0], "dep_a");
    EXPECT_EQ(deps[1], "dep_b");
}

// ─────────────────────────────────────────────────────────────
// § PluginInfo struct fields
// ─────────────────────────────────────────────────────────────

TEST(PluginEnhancedTest, PluginInfoStructFields) {
    auto now = std::chrono::system_clock::now();
    PluginInfo pi{"myplugin", "2.0.0", "/path/to/lib.so", {"dep1"}, now};
    EXPECT_EQ(pi.name, "myplugin");
    EXPECT_EQ(pi.version, "2.0.0");
    EXPECT_EQ(pi.path, "/path/to/lib.so");
    ASSERT_EQ(pi.dependencies.size(), 1u);
    EXPECT_EQ(pi.dependencies[0], "dep1");
    EXPECT_EQ(pi.loaded_at, now);
}

TEST(PluginEnhancedTest, PluginExtensionIsPlatformCorrect) {
    auto ext = PluginManager::plugin_extension();
#ifdef __APPLE__
    EXPECT_EQ(ext, ".dylib");
#else
    EXPECT_EQ(ext, ".so");
#endif
}
