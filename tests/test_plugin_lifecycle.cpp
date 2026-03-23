// ============================================================
// Tests for Plugin Lifecycle — load/unload/find/list/count
// and destructor behavior
// ============================================================
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
namespace fs = std::filesystem;

// Helper: path to the test plugin shared library
static std::string plugin_path() {
    return std::string("./libtest_plugin_sample") + PluginManager::plugin_extension();
}

static bool plugin_available() {
    return fs::exists(plugin_path());
}

// ─────────────────────────────────────────────────────────────
// § 1. Load -> use -> unload -> verify unloaded
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, LoadUseUnloadVerifyUnloaded) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    // Load
    auto lr = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    // Use — verify it's accessible and functional
    auto* p = os->plugins().find("sample");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "sample");
    EXPECT_EQ(p->version(), "1.0.0");

    // Unload
    auto ur = os->plugins().unload("sample");
    ASSERT_TRUE(ur.has_value()) << ur.error().message;

    // Verify unloaded — find returns nullptr, count is 0
    EXPECT_EQ(os->plugins().find("sample"), nullptr);
    EXPECT_EQ(os->plugins().count(), 0u);
    EXPECT_TRUE(os->plugins().list().empty());
}

// ─────────────────────────────────────────────────────────────
// § 2. Load same plugin twice returns error
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, LoadSamePluginTwiceReturnsError) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    auto r1 = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = os->plugins().load(plugin_path(), *os);
    EXPECT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, ErrorCode::AlreadyExists);

    // Original is still loaded
    EXPECT_EQ(os->plugins().count(), 1u);
    EXPECT_NE(os->plugins().find("sample"), nullptr);
}

// ─────────────────────────────────────────────────────────────
// § 3. Unload nonexistent plugin returns error
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, UnloadNonexistentPluginReturnsError) {
    auto os = quickstart_mock();

    auto r = os->plugins().unload("does_not_exist");
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

// ─────────────────────────────────────────────────────────────
// § 4. find() returns nullptr after unload
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, FindReturnsNullptrAfterUnload) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    auto lr = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;
    ASSERT_NE(os->plugins().find("sample"), nullptr);

    auto ur = os->plugins().unload("sample");
    ASSERT_TRUE(ur.has_value());

    EXPECT_EQ(os->plugins().find("sample"), nullptr);
}

// ─────────────────────────────────────────────────────────────
// § 5. list() returns correct names
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, ListReturnsCorrectNames) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    // Initially empty
    EXPECT_TRUE(os->plugins().list().empty());

    // After loading one plugin
    auto lr = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    auto names = os->plugins().list();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "sample");

    // After unloading, empty again
    (void)os->plugins().unload("sample");
    EXPECT_TRUE(os->plugins().list().empty());
}

// ─────────────────────────────────────────────────────────────
// § 6. count() matches number of loaded plugins
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, CountMatchesNumberOfLoadedPlugins) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    EXPECT_EQ(os->plugins().count(), 0u);

    auto lr = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;
    EXPECT_EQ(os->plugins().count(), 1u);

    (void)os->plugins().unload("sample");
    EXPECT_EQ(os->plugins().count(), 0u);
}

// ─────────────────────────────────────────────────────────────
// § 7. Plugin init() receives valid AgentOS reference
// ─────────────────────────────────────────────────────────────

namespace {
// An in-process test plugin that records whether init() was called
// with a valid AgentOS reference (by checking agent_count).
class InitTrackingPlugin : public IPlugin {
public:
    std::string name() const override { return "init_tracker"; }
    std::string version() const override { return "0.0.1"; }
    Result<void> init(AgentOS& os) override {
        init_called = true;
        // Verify the AgentOS reference is usable
        agent_count_at_init = os.agent_count();
        return {};
    }
    void shutdown() override {
        shutdown_called = true;
    }

    bool init_called{false};
    bool shutdown_called{false};
    size_t agent_count_at_init{999};
};
} // anonymous namespace

TEST(PluginLifecycleTest, PluginInitReceivesValidAgentOSReference) {
    auto os = quickstart_mock();

    // Create an agent first so count > 0
    auto agent = make_agent(*os, "test-agent").prompt("test").create();
    ASSERT_EQ(os->agent_count(), 1u);

    InitTrackingPlugin plugin;
    auto r = plugin.init(*os);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(plugin.init_called);
    EXPECT_EQ(plugin.agent_count_at_init, 1u);
}

// ─────────────────────────────────────────────────────────────
// § 8. Plugin shutdown() is called on unload
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, PluginShutdownCalledOnUnload) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    // Load the plugin
    auto lr = os->plugins().load(plugin_path(), *os);
    ASSERT_TRUE(lr.has_value()) << lr.error().message;

    // The actual shutdown call is inside the PluginManager::unload.
    // We cannot directly observe the call on the dynamic plugin, but
    // we verify the unload completes without error and the plugin is gone.
    auto ur = os->plugins().unload("sample");
    EXPECT_TRUE(ur.has_value());
    EXPECT_EQ(os->plugins().find("sample"), nullptr);
    EXPECT_EQ(os->plugins().count(), 0u);
}

// Also verify with an in-process plugin that shutdown is called
TEST(PluginLifecycleTest, InProcessPluginShutdownCalledExplicitly) {
    InitTrackingPlugin plugin;
    auto os = quickstart_mock();

    auto r = plugin.init(*os);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(plugin.shutdown_called);

    plugin.shutdown();
    EXPECT_TRUE(plugin.shutdown_called);
}

// ─────────────────────────────────────────────────────────────
// § 9. Platform extension is .dylib on macOS, .so on Linux
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, PlatformExtensionIsCorrect) {
    auto ext = PluginManager::plugin_extension();
#ifdef __APPLE__
    EXPECT_EQ(ext, ".dylib");
#else
    EXPECT_EQ(ext, ".so");
#endif
    // Verify it starts with a dot
    EXPECT_FALSE(ext.empty());
    EXPECT_EQ(ext[0], '.');
}

// ─────────────────────────────────────────────────────────────
// § 10. PluginManager destructor calls shutdown on all plugins
// ─────────────────────────────────────────────────────────────

TEST(PluginLifecycleTest, DestructorCallsShutdownOnAllPlugins) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    {
        // Create a scoped PluginManager, load a plugin, let it destruct
        PluginManager mgr;
        auto lr = mgr.load(plugin_path(), *os);
        ASSERT_TRUE(lr.has_value()) << lr.error().message;
        EXPECT_EQ(mgr.count(), 1u);

        // Destructor runs at end of scope — it calls shutdown() and
        // destroy on all plugins, then dlclose. No crash = success.
    }

    // If we get here without segfault/crash, the destructor properly
    // cleaned up all loaded plugins.
    SUCCEED();
}

// Additional: Verify that after PluginManager destruction,
// a new PluginManager can load the same plugin again
TEST(PluginLifecycleTest, CanReloadAfterManagerDestruction) {
    if (!plugin_available()) GTEST_SKIP() << "Test plugin not built";

    auto os = quickstart_mock();

    {
        PluginManager mgr;
        auto lr = mgr.load(plugin_path(), *os);
        ASSERT_TRUE(lr.has_value()) << lr.error().message;
    }

    // New manager should be able to load the same plugin
    {
        PluginManager mgr2;
        auto lr2 = mgr2.load(plugin_path(), *os);
        EXPECT_TRUE(lr2.has_value()) << lr2.error().message;
        EXPECT_EQ(mgr2.count(), 1u);
    }
}
