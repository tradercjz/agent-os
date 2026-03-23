// ============================================================
// AgentOS :: PluginManager — dlopen-based dynamic plugin loading
// ============================================================
#include <agentos/core/plugin.hpp>
#include <agentos/core/logger.hpp>
#include <dlfcn.h>
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

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
    auto now = std::chrono::system_clock::now();
    {
        std::lock_guard lk(mu_);
        plugins_.push_back(LoadedPlugin{handle, plugin, destroy_fn, path, now});
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

Result<void> PluginManager::scan_directory(const std::filesystem::path& dir, AgentOS& os) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        return make_error(ErrorCode::NotFound,
            fmt::format("Plugin directory '{}' does not exist or is not a directory", dir.string()));
    }

    const auto ext = plugin_extension();
    std::vector<std::string> errors;
    size_t loaded_count = 0;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        if (p.extension().string() != ext) continue;

        auto r = load(p.string(), os);
        if (r) {
            ++loaded_count;
        } else {
            errors.push_back(fmt::format("{}: {}", p.filename().string(), r.error().message));
        }
    }

    if (loaded_count == 0 && !errors.empty()) {
        return make_error(ErrorCode::Unknown,
            fmt::format("Failed to load any plugins from '{}': {}", dir.string(), errors[0]));
    }

    LOG_INFO(fmt::format("scan_directory '{}': loaded {} plugin(s), {} error(s)",
                         dir.string(), loaded_count, errors.size()));
    return {};
}

Result<void> PluginManager::reload(const std::string& name, AgentOS& os) {
    // Find the path before unloading
    std::string path;
    {
        std::lock_guard lk(mu_);
        auto it = std::find_if(plugins_.begin(), plugins_.end(),
            [&name](const LoadedPlugin& lp) {
                return lp.plugin && lp.plugin->name() == name;
            });
        if (it == plugins_.end()) {
            return make_error(ErrorCode::NotFound,
                fmt::format("Plugin '{}' not loaded, cannot reload", name));
        }
        path = it->path;
    }

    // Unload, then re-load from the same path
    auto ur = unload(name);
    if (!ur) return ur;

    auto lr = load(path, os);
    if (!lr) {
        return make_error(lr.error().code,
            fmt::format("Plugin '{}' reload failed on re-load: {}", name, lr.error().message));
    }

    LOG_INFO(fmt::format("Plugin reloaded: {}", name));
    return {};
}

std::optional<PluginInfo> PluginManager::info(const std::string& name) const {
    std::lock_guard lk(mu_);
    for (const auto& lp : plugins_) {
        if (lp.plugin && lp.plugin->name() == name) {
            return PluginInfo{
                lp.plugin->name(),
                lp.plugin->version(),
                lp.path,
                lp.plugin->dependencies(),
                lp.loaded_at
            };
        }
    }
    return std::nullopt;
}

Result<void> PluginManager::load_ordered(const std::vector<std::string>& paths, AgentOS& os) {
    // Phase 1: Probe each plugin to get name + dependencies without fully loading.
    // We do a lightweight dlopen/create/query/destroy cycle to gather metadata.
    struct PluginMeta {
        std::string path;
        std::string name;
        std::vector<std::string> deps;
    };

    std::vector<PluginMeta> metas;
    metas.reserve(paths.size());

    for (const auto& path : paths) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* err = dlerror();
            return make_error(ErrorCode::NotFound,
                fmt::format("dlopen failed for '{}': {}", path, err ? err : "unknown error"));
        }

        dlerror();
        auto create_fn = reinterpret_cast<PluginCreateFn>(dlsym(handle, "agentos_plugin_create"));
        const char* sym_err = dlerror();
        if (sym_err || !create_fn) {
            dlclose(handle);
            return make_error(ErrorCode::NotFound,
                fmt::format("Plugin '{}' missing agentos_plugin_create", path));
        }

        dlerror();
        auto destroy_fn = reinterpret_cast<PluginDestroyFn>(dlsym(handle, "agentos_plugin_destroy"));
        sym_err = dlerror();
        if (sym_err || !destroy_fn) {
            dlclose(handle);
            return make_error(ErrorCode::NotFound,
                fmt::format("Plugin '{}' missing agentos_plugin_destroy", path));
        }

        IPlugin* plugin = create_fn();
        if (!plugin) {
            dlclose(handle);
            return make_error(ErrorCode::Unknown,
                fmt::format("Plugin '{}': agentos_plugin_create returned nullptr", path));
        }

        PluginMeta meta;
        meta.path = path;
        meta.name = plugin->name();
        meta.deps = plugin->dependencies();

        destroy_fn(plugin);
        dlclose(handle);

        metas.push_back(std::move(meta));
    }

    // Phase 2: Topological sort (Kahn's algorithm)
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < metas.size(); ++i) {
        name_to_idx[metas[i].name] = i;
    }

    // Build adjacency list and in-degree
    std::vector<std::vector<size_t>> adj(metas.size());
    std::vector<size_t> in_degree(metas.size(), 0);

    for (size_t i = 0; i < metas.size(); ++i) {
        for (const auto& dep : metas[i].deps) {
            auto it = name_to_idx.find(dep);
            if (it != name_to_idx.end()) {
                // dep -> i (dep must be loaded before i)
                adj[it->second].push_back(i);
                ++in_degree[i];
            }
            // Dependencies on already-loaded or external plugins are allowed
        }
    }

    // Kahn's BFS
    std::queue<size_t> q;
    for (size_t i = 0; i < metas.size(); ++i) {
        if (in_degree[i] == 0) {
            q.push(i);
        }
    }

    std::vector<size_t> order;
    order.reserve(metas.size());

    while (!q.empty()) {
        size_t u = q.front();
        q.pop();
        order.push_back(u);
        for (size_t v : adj[u]) {
            if (--in_degree[v] == 0) {
                q.push(v);
            }
        }
    }

    if (order.size() != metas.size()) {
        return make_error(ErrorCode::CircularDependency,
            "Circular dependency detected among plugins");
    }

    // Phase 3: Load in topological order
    for (size_t idx : order) {
        auto r = load(metas[idx].path, os);
        if (!r) return r;
    }

    return {};
}

} // namespace agentos
