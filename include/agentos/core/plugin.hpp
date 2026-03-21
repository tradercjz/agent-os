#pragma once
// ============================================================
// AgentOS :: Plugin System (dlopen-based dynamic loading)
// ============================================================
#include <agentos/core/types.hpp>
#include <memory>
#include <mutex>
#include <string>
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
};

// Entry point function signature — plugins export these:
//   extern "C" IPlugin* agentos_plugin_create();
//   extern "C" void agentos_plugin_destroy(IPlugin*);
using PluginCreateFn  = IPlugin* (*)();
using PluginDestroyFn = void (*)(IPlugin*);

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
    };
    std::vector<LoadedPlugin> plugins_;
    mutable std::mutex mu_;
};

} // namespace agentos
