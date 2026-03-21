#pragma once
// ============================================================
// AgentOS :: Core — API Key Secure Loader
// Fallback chain: explicit value -> env var -> key file
// ============================================================
#include <agentos/core/types.hpp>
#include <string>

namespace agentos {

class KeyLoader {
public:
    // Load API key with fallback chain:
    // 1. Explicit value (if non-empty)
    // 2. Environment variable
    // 3. Key file (~/.agentos/keys.json, field = key_name)
    // Returns the key or error
    static Result<std::string> load(const std::string& explicit_value,
                                    const std::string& env_var,
                                    const std::string& key_name);

    // Load from key file only
    static Result<std::string> from_file(const std::string& key_name,
                                         const std::string& file_path = "");

    // Default key file path: ~/.agentos/keys.json
    static std::string default_key_file();
};

} // namespace agentos
