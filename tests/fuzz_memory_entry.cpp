// ============================================================
// AgentOS :: Fuzz Target — Memory Entry JSON Deserialization
// Build with: cmake -DAGENTOS_ENABLE_FUZZER=ON
// ============================================================
#include <agentos/memory/memory.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);

    // Try parsing random data as JSON memory entry metadata
    try {
        auto j = nlohmann::json::parse(input);
        // Try constructing a MemoryEntry from parsed JSON fields
        agentos::memory::MemoryEntry entry;
        if (j.contains("id")) entry.id = j.value("id", "");
        if (j.contains("content")) entry.content = j.value("content", "");
        if (j.contains("importance")) entry.importance = j.value("importance", 0.0f);
    } catch (const nlohmann::json::exception&) {
        // Expected for random input — fuzzer intentionally feeds garbage
    }

    return 0;
}
