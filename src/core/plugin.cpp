// ============================================================
// AgentOS :: PluginManager — dlopen-based dynamic plugin loading
// ============================================================
#include <agentos/core/plugin.hpp>
#include <agentos/core/logger.hpp>
#include <dlfcn.h>
#include <algorithm>

namespace agentos {

PluginManager::~PluginManager() {
    // Unload all plugins in reverse order (LIFO)
    std::lock_guard lk(mu_);
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (it->plugin && it->destroy) {
            it->plugin->shutdown();
            it->destroy(it->plugin);
        }
        if (it->handle) {
            dlclose(it->handle);
        }
    }
    plugins_.clear();
}

Result<void> PluginManager::load(const std::string& path, AgentOS& os) {
    // dlopen the shared library
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        return make_error(ErrorCode::NotFound,
            fmt::format("dlopen failed for '{}': {}", path, err ? err : "unknown error"));
    }

    // Resolve create function
    dlerror(); // clear any prior error
    auto create_fn = reinterpret_cast<PluginCreateFn>(dlsym(handle, "agentos_plugin_create"));
    const char* sym_err = dlerror();
    if (sym_err || !create_fn) {
        dlclose(handle);
        return make_error(ErrorCode::NotFound,
            fmt::format("Plugin '{}' missing agentos_plugin_create: {}", path,
                        sym_err ? sym_err : "symbol is null"));
    }

    // Resolve destroy function
    dlerror();
    auto destroy_fn = reinterpret_cast<PluginDestroyFn>(dlsym(handle, "agentos_plugin_destroy"));
    sym_err = dlerror();
    if (sym_err || !destroy_fn) {
        dlclose(handle);
        return make_error(ErrorCode::NotFound,
            fmt::format("Plugin '{}' missing agentos_plugin_destroy: {}", path,
                        sym_err ? sym_err : "symbol is null"));
    }

    // Create the plugin instance
    IPlugin* plugin = create_fn();
    if (!plugin) {
        dlclose(handle);
        return make_error(ErrorCode::Unknown,
            fmt::format("Plugin '{}': agentos_plugin_create returned nullptr", path));
    }

    // Check for duplicate name
    std::string plugin_name = plugin->name();
    {
        std::lock_guard lk(mu_);
        for (const auto& lp : plugins_) {
            if (lp.plugin && lp.plugin->name() == plugin_name) {
                destroy_fn(plugin);
                dlclose(handle);
                return make_error(ErrorCode::AlreadyExists,
                    fmt::format("Plugin '{}' already loaded", plugin_name));
            }
        }
    }

    // Initialize the plugin
    auto init_result = plugin->init(os);
    if (!init_result) {
        destroy_fn(plugin);
        dlclose(handle);
        return make_error(init_result.error().code,
            fmt::format("Plugin '{}' init failed: {}", plugin_name, init_result.error().message));
    }

    // Store the loaded plugin
    {
        std::lock_guard lk(mu_);
        plugins_.push_back(LoadedPlugin{handle, plugin, destroy_fn, path});
    }

    LOG_INFO(fmt::format("Plugin loaded: {} v{} from {}", plugin_name, plugin->version(), path));
    return {};
}

Result<void> PluginManager::unload(const std::string& name) {
    std::lock_guard lk(mu_);

    auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [&name](const LoadedPlugin& lp) {
            return lp.plugin && lp.plugin->name() == name;
        });

    if (it == plugins_.end()) {
        return make_error(ErrorCode::NotFound,
            fmt::format("Plugin '{}' not loaded", name));
    }

    // Shutdown and destroy
    it->plugin->shutdown();
    it->destroy(it->plugin);
    dlclose(it->handle);
    plugins_.erase(it);

    LOG_INFO(fmt::format("Plugin unloaded: {}", name));
    return {};
}

IPlugin* PluginManager::find(const std::string& name) const {
    std::lock_guard lk(mu_);
    for (const auto& lp : plugins_) {
        if (lp.plugin && lp.plugin->name() == name) {
            return lp.plugin;
        }
    }
    return nullptr;
}

std::vector<std::string> PluginManager::list() const {
    std::lock_guard lk(mu_);
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& lp : plugins_) {
        if (lp.plugin) {
            names.push_back(lp.plugin->name());
        }
    }
    return names;
}

size_t PluginManager::count() const {
    std::lock_guard lk(mu_);
    return plugins_.size();
}

} // namespace agentos
