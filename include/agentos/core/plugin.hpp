#pragma once
// ============================================================
// AgentOS :: Plugin System (dlopen-based dynamic loading)
// ============================================================
#include <agentos/core/types.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

class AgentOS; // forward

// ─────────────────────────────────────────────────────────────
// § Plugin Interface — all plugins must implement this
// ─────────────────────────────────────────────────────────────

class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    virtual Result<void> init(AgentOS& os) = 0;    // called after load
    virtual void shutdown() = 0;                     // called before unload

    /// Return names of plugins this plugin depends on (empty by default)
    virtual std::vector<std::string> dependencies() const { return {}; }
};

// Entry point function signature — plugins export these:
//   extern "C" IPlugin* agentos_plugin_create();
//   extern "C" void agentos_plugin_destroy(IPlugin*);
using PluginCreateFn  = IPlugin* (*)();
using PluginDestroyFn = void (*)(IPlugin*);

// ─────────────────────────────────────────────────────────────
// § PluginInfo — metadata about a loaded plugin
// ─────────────────────────────────────────────────────────────

struct PluginInfo {
    std::string name;
    std::string version;
    std::string path;
    std::vector<std::string> dependencies;
    std::chrono::system_clock::time_point loaded_at;
};

// ─────────────────────────────────────────────────────────────
// § PluginManager — manages dynamic plugin loading/unloading
// ─────────────────────────────────────────────────────────────

class PluginManager : private NonCopyable {
public:
    PluginManager() = default;
    ~PluginManager();

    /// Load a plugin from shared library path (.so/.dylib)
    Result<void> load(const std::string& path, AgentOS& os);

    /// Unload a plugin by name
    Result<void> unload(const std::string& name);

    /// Get loaded plugin by name (nullptr if not found)
    IPlugin* find(const std::string& name) const;

    /// List all loaded plugin names
    std::vector<std::string> list() const;

    /// Number of loaded plugins
    size_t count() const;

    /// Scan a directory and load all .so/.dylib files found
    Result<void> scan_directory(const std::filesystem::path& dir, AgentOS& os);

    /// Reload a plugin by name (unload then re-load from same path)
    Result<void> reload(const std::string& name, AgentOS& os);

    /// Get metadata info about a loaded plugin
    std::optional<PluginInfo> info(const std::string& name) const;

    /// Load multiple plugins with topological ordering by dependencies
    Result<void> load_ordered(const std::vector<std::string>& paths, AgentOS& os);

    /// Platform-specific shared library extension
    static std::string plugin_extension() {
#ifdef __APPLE__
        return ".dylib";
#else
        return ".so";
#endif
    }

private:
    struct LoadedPlugin {
        void* handle{nullptr};           // dlopen handle
        IPlugin* plugin{nullptr};
        PluginDestroyFn destroy{nullptr};
        std::string path;
        std::chrono::system_clock::time_point loaded_at;
    };
    std::vector<LoadedPlugin> plugins_;
    mutable std::mutex mu_;
};

} // namespace agentos
