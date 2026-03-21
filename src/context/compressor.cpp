#include <agentos/context/compressor.hpp>
#include <algorithm>

namespace agentos::context {

ContextCompressor::ContextCompressor(CompressionConfig config)
    : config_(std::move(config)) {}

bool ContextCompressor::should_compress(TokenCount current, TokenCount limit) const {
    if (limit == 0) return false;
    return static_cast<float>(current) / static_cast<float>(limit) >= config_.trigger_ratio;
}

Result<CompressionResult> ContextCompressor::compress(
    std::vector<kernel::Message>& messages,
    TokenCount token_limit,
    const CompressSummarizeFn& summarize) {

    CompressionResult result;
    result.messages_before = messages.size();
    result.tokens_before = 0;
    for (const auto& m : messages)
        result.tokens_before += kernel::ILLMBackend::estimate_tokens(m.content);

    // Not enough messages to compress
    if (messages.size() < config_.min_messages_to_keep + config_.min_messages_to_compress) {
        return result;
    }

    // Separate system messages (always keep) from non-system
    std::vector<kernel::Message> system_msgs;
    std::vector<kernel::Message> non_system;
    for (auto& m : messages) {
        if (m.role == kernel::Role::System) {
            system_msgs.push_back(std::move(m));
        } else {
            non_system.push_back(std::move(m));
        }
    }

    if (non_system.size() <= config_.min_messages_to_keep) {
        // Nothing to compress — restore and return
        messages.clear();
        for (auto& m : system_msgs) messages.push_back(std::move(m));
        for (auto& m : non_system) messages.push_back(std::move(m));
        return result;
    }

    // Split: old messages to compress, recent messages to keep
    size_t keep_count = config_.min_messages_to_keep;
    size_t compress_count = non_system.size() - keep_count;

    std::vector<kernel::Message> to_compress(
        std::make_move_iterator(non_system.begin()),
        std::make_move_iterator(non_system.begin() + static_cast<ptrdiff_t>(compress_count)));
    std::vector<kernel::Message> to_keep(
        std::make_move_iterator(non_system.begin() + static_cast<ptrdiff_t>(compress_count)),
        std::make_move_iterator(non_system.end()));

    // Summarize
    auto summary_result = summarize(to_compress);
    if (!summary_result.has_value()) {
        // Restore original messages on failure (use to_compress + to_keep since non_system was moved)
        messages.clear();
        for (auto& m : system_msgs) messages.push_back(std::move(m));
        for (auto& m : to_compress) messages.push_back(std::move(m));
        for (auto& m : to_keep) messages.push_back(std::move(m));
        return make_error(summary_result.error().code, summary_result.error().message);
    }

    // Rebuild message list: system + summary + kept recent
    messages.clear();
    for (auto& m : system_msgs) messages.push_back(std::move(m));
    messages.push_back(kernel::Message::system(
        "[Conversation Summary]\n" + *summary_result));
    for (auto& m : to_keep) messages.push_back(std::move(m));

    result.compressed = true;
    result.messages_after = messages.size();
    result.tokens_after = 0;
    for (const auto& m : messages)
        result.tokens_after += kernel::ILLMBackend::estimate_tokens(m.content);

    (void)token_limit; // reserved for future target_ratio-based compression
    return result;
}

CompressSummarizeFn ContextCompressor::make_llm_summarizer(kernel::LLMKernel& kernel) {
    return [&kernel](const std::vector<kernel::Message>& msgs) -> Result<std::string> {
        std::string conversation;
        for (const auto& m : msgs) {
            std::string role_str;
            switch (m.role) {
                case kernel::Role::User: role_str = "User"; break;
                case kernel::Role::Assistant: role_str = "Assistant"; break;
                case kernel::Role::Tool: role_str = "Tool"; break;
                default: role_str = "System"; break;
            }
            conversation += role_str + ": " + m.content + "\n";
        }

        kernel::LLMRequest req;
        req.messages.push_back(kernel::Message::system(
            "Summarize the following conversation concisely, preserving key facts and decisions:"));
        req.messages.push_back(kernel::Message::user(conversation));
        req.max_tokens = 500;

        auto result = kernel.infer(req);
        if (!result.has_value()) return make_error(result.error().code, result.error().message);
        return result->content;
    };
}

CompressSummarizeFn ContextCompressor::make_simple_summarizer(size_t max_chars) {
    return [max_chars](const std::vector<kernel::Message>& msgs) -> Result<std::string> {
        std::string summary;
        for (const auto& m : msgs) {
            std::string role_str;
            switch (m.role) {
                case kernel::Role::User: role_str = "User"; break;
                case kernel::Role::Assistant: role_str = "Assistant"; break;
                default: role_str = "Other"; break;
            }
            size_t snippet_len = std::min(m.content.size(), static_cast<size_t>(100));
            summary += role_str + ": " + m.content.substr(0, snippet_len) + "...\n";
        }
        if (summary.size() > max_chars) {
            summary = summary.substr(0, max_chars) + "\n[truncated]";
        }
        return summary;
    };
}

} // namespace agentos::context
