// ============================================================
// Tests for PluginManager (dlopen-based plugin loading)
// ============================================================
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;

TEST(PluginManagerTest, LoadAndUnloadPlugin) {
    auto os = quickstart_mock();

    // Find the test plugin library next to the test binary
    std::string plugin_path = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();

    // Skip test if plugin not built
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_path;
    }

    auto r = os->plugins().load(plugin_path, *os);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    EXPECT_EQ(os->plugins().count(), 1u);

    auto* p = os->plugins().find("sample");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "sample");
    EXPECT_EQ(p->version(), "1.0.0");

    auto names = os->plugins().list();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "sample");

    auto ur = os->plugins().unload("sample");
    EXPECT_TRUE(ur.has_value());
    EXPECT_EQ(os->plugins().count(), 0u);
}

TEST(PluginManagerTest, LoadNonexistentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->plugins().load("/nonexistent/plugin.dylib", *os);
    EXPECT_FALSE(r.has_value());
}

TEST(PluginManagerTest, DuplicateLoadReturnsError) {
    auto os = quickstart_mock();

    std::string plugin_path = std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
    if (!std::filesystem::exists(plugin_path)) {
        GTEST_SKIP() << "Test plugin not found at " << plugin_path;
    }

    auto r1 = os->plugins().load(plugin_path, *os);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = os->plugins().load(plugin_path, *os);
    EXPECT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, ErrorCode::AlreadyExists);
}

TEST(PluginManagerTest, ListPlugins) {
    PluginManager mgr;
    EXPECT_EQ(mgr.count(), 0u);
    EXPECT_TRUE(mgr.list().empty());
}

TEST(PluginManagerTest, FindNonexistentReturnsNull) {
    PluginManager mgr;
    EXPECT_EQ(mgr.find("nonexistent"), nullptr);
}

TEST(PluginManagerTest, UnloadNonexistentReturnsError) {
    PluginManager mgr;
    auto r = mgr.unload("nonexistent");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(PluginManagerTest, PluginExtension) {
#ifdef __APPLE__
    EXPECT_EQ(PluginManager::plugin_extension(), ".dylib");
#else
    EXPECT_EQ(PluginManager::plugin_extension(), ".so");
#endif
}
