// ============================================================
// AgentOS :: Fuzz Target — LLM Response JSON Parsing
// Build with: cmake -DAGENTOS_ENABLE_FUZZER=ON
// ============================================================
#include <agentos/kernel/ollama_backend.hpp>
#include <agentos/kernel/anthropic_backend.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);

    // Test OllamaBackend response parsing
    agentos::kernel::OllamaBackend ollama;
    (void)ollama.parse_chat_response(input);

    // Test AnthropicBackend response parsing
    agentos::kernel::AnthropicBackend anthropic("dummy-key");
    (void)anthropic.parse_response(input);

    return 0;
}
