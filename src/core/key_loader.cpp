#include <agentos/core/key_loader.hpp>
#include <agentos/core/logger.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace agentos {

std::string KeyLoader::default_key_file() {
    const char* home = std::getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.agentos/keys.json";
}

Result<std::string> KeyLoader::load(const std::string& explicit_value,
                                    const std::string& env_var,
                                    const std::string& key_name) {
    // 1. Explicit value takes priority
    if (!explicit_value.empty()) {
        return explicit_value;
    }

    // 2. Environment variable
    if (!env_var.empty()) {
        const char* val = std::getenv(env_var.c_str());
        if (val && val[0] != '\0') {
            return std::string(val);
        }
    }

    // 3. Key file fallback
    auto file_result = from_file(key_name);
    if (file_result.has_value()) {
        return file_result;
    }

    return make_error(ErrorCode::NotFound,
                      fmt::format("API key '{}' not found in explicit value, "
                                  "env var '{}', or key file",
                                  key_name, env_var));
}

Result<std::string> KeyLoader::from_file(const std::string& key_name,
                                         const std::string& file_path) {
    std::string path = file_path.empty() ? default_key_file() : file_path;
    if (path.empty()) {
        return make_error(ErrorCode::NotFound,
                          "Cannot determine key file path (HOME not set)");
    }

    if (!std::filesystem::exists(path)) {
        return make_error(ErrorCode::NotFound,
                          fmt::format("Key file '{}' does not exist", path));
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return make_error(ErrorCode::MemoryReadFailed,
                          fmt::format("Cannot open key file '{}'", path));
    }

    try {
        auto j = Json::parse(ifs);
        if (!j.is_object() || !j.contains(key_name)) {
            return make_error(ErrorCode::NotFound,
                              fmt::format("Key '{}' not found in '{}'",
                                          key_name, path));
        }
        auto val = j[key_name].get<std::string>();
        if (val.empty()) {
            return make_error(ErrorCode::NotFound,
                              fmt::format("Key '{}' is empty in '{}'",
                                          key_name, path));
        }
        return val;
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(ErrorCode::InvalidArgument,
                          fmt::format("Failed to parse key file '{}': {}",
                                      path, e.what()));
    } catch (const nlohmann::json::type_error& e) {
        return make_error(ErrorCode::InvalidArgument,
                          fmt::format("Type error in key file '{}': {}",
                                      path, e.what()));
    }
}

} // namespace agentos
