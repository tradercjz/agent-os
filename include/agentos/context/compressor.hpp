#pragma once
// ============================================================
// AgentOS :: Context Compressor
// Automatic context compression with pluggable summarizers
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <functional>
#include <string>
#include <vector>

namespace agentos::context {

struct CompressionConfig {
    float trigger_ratio{0.8f};        // compress when 80% of token budget used
    float target_ratio{0.5f};         // compress down to 50% of budget
    size_t min_messages_to_keep{4};   // always keep latest N messages uncompressed
    size_t min_messages_to_compress{3}; // need at least N messages to justify compression
};

struct CompressionResult {
    bool compressed{false};
    size_t messages_before{0};
    size_t messages_after{0};
    TokenCount tokens_before{0};
    TokenCount tokens_after{0};
};

// Summarize function type: takes messages, returns summary string or error
using CompressSummarizeFn = std::function<Result<std::string>(const std::vector<kernel::Message>&)>;

class ContextCompressor {
public:
    explicit ContextCompressor(CompressionConfig config = {});

    // Check if compression is needed based on current usage
    bool should_compress(TokenCount current_tokens, TokenCount limit) const;

    // Compress messages: summarize old ones into a single "[Summary]" message.
    // Keeps system messages and latest min_messages_to_keep messages intact.
    Result<CompressionResult> compress(
        std::vector<kernel::Message>& messages,
        TokenCount token_limit,
        const CompressSummarizeFn& summarize);

    // Built-in summarizer using LLM
    static CompressSummarizeFn make_llm_summarizer(kernel::LLMKernel& kernel);

    // Simple summarizer: concatenate and truncate (no LLM needed)
    static CompressSummarizeFn make_simple_summarizer(size_t max_chars = 500);

    const CompressionConfig& config() const { return config_; }

private:
    CompressionConfig config_;
};

} // namespace agentos::context
