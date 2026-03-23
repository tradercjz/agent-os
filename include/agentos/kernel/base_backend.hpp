#pragma once
// ============================================================
// AgentOS :: Kernel — BackendUtils
// Shared HTTP infrastructure for LLM backends that communicate
// over REST (OpenAI, Anthropic, Ollama, etc.)
// ============================================================
#include <agentos/core/logger.hpp>
#include <agentos/core/types.hpp>
#include <agentos/kernel/http_client.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace agentos::kernel {

/// Shared utilities for HTTP-based LLM backends.
///
/// Provides:
///   - redact_auth(): strip API keys / Bearer tokens from error messages
///   - http_post_json(): POST JSON to an endpoint with standard HTTP error handling
///   - apply_fallback_token_estimates(): heuristic token counts when the API
///     does not return usage data (common in streaming responses)
struct BackendUtils {

    /// Redact sensitive headers and API keys from error messages before logging.
    /// Handles "Bearer xxx" and "sk-xxx" patterns.
    static std::string redact_auth(const std::string& msg) {
        std::string result = msg;
        // Redact "Bearer xxx..." patterns
        static const std::string bearer = "Bearer ";
        auto pos = result.find(bearer);
        while (pos != std::string::npos) {
            auto end = result.find_first_of(" \r\n\"'}", pos + bearer.size());
            if (end == std::string::npos) end = result.size();
            result.replace(pos + bearer.size(), end - pos - bearer.size(), "***REDACTED***");
            pos = result.find(bearer, pos + bearer.size() + 14);
        }
        // Redact "sk-..." API key patterns
        auto sk_pos = result.find("sk-");
        while (sk_pos != std::string::npos) {
            auto end = result.find_first_of(" \r\n\"'}", sk_pos);
            if (end == std::string::npos) end = result.size();
            result.replace(sk_pos, end - sk_pos, "sk-***REDACTED***");
            sk_pos = result.find("sk-", sk_pos + 17);
        }
        return result;
    }

    /// POST JSON to `url` with the given `headers`.
    /// Returns the response body on success (HTTP 2xx).
    /// Maps common HTTP error status codes to appropriate ErrorCode values
    /// and redacts sensitive data from error messages.
    ///
    /// @param provider_name  Short label for log messages (e.g. "OpenAI", "Ollama")
    /// @param http           The HttpClient to use
    /// @param url            Full URL to POST to
    /// @param body           JSON request body
    /// @param headers        HTTP headers (Content-Type, Auth, etc.)
    /// @param timeout_sec    HTTP timeout in seconds
    [[nodiscard]]
    static Result<std::string> http_post_json(
            std::string_view provider_name,
            const HttpClient& http,
            const std::string& url,
            const std::string& body,
            const std::vector<std::string>& headers,
            int timeout_sec = 60) {
        auto result = http.post(url, body, headers, timeout_sec);
        if (!result) {
            return make_error(result.error().code,
                              redact_auth(result.error().message));
        }

        long http_code = result->status_code;

        if (http_code == 401) {
            return make_error(ErrorCode::LLMBackendError,
                fmt::format("{} API: authentication failed (check API key)",
                            provider_name));
        }
        if (http_code == 429) {
            return make_error(ErrorCode::RateLimitExceeded,
                fmt::format("{} API: rate limited (HTTP 429): {}",
                            provider_name,
                            redact_auth(result->body.substr(0, 500))));
        }
        if (http_code >= 500 && http_code < 600) {
            return make_error(ErrorCode::LLMBackendError,
                fmt::format("{} API: server error (HTTP {}): {}",
                            provider_name, http_code,
                            redact_auth(result->body.substr(0, 500))));
        }
        if (http_code >= 400) {
            // Try to extract a structured error message from the JSON body
            std::string error_msg;
            try {
                Json err_json = Json::parse(result->body);
                if (err_json.contains("error")) {
                    auto& err_obj = err_json["error"];
                    if (err_obj.is_object() && err_obj.contains("message") &&
                        err_obj["message"].is_string()) {
                        error_msg = err_obj["message"].get<std::string>();
                    } else if (err_obj.is_string()) {
                        error_msg = err_obj.get<std::string>();
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // not JSON, use raw body
            }

            if (error_msg.empty())
                error_msg = result->body.substr(0, 500);

            return make_error(ErrorCode::LLMBackendError,
                fmt::format("{} API: unexpected status (HTTP {}): {}",
                            provider_name, http_code,
                            redact_auth(error_msg)));
        }

        return result->body;
    }

    /// Fill in heuristic token estimates when the API response did not
    /// include usage data (common with streaming endpoints).
    static void apply_fallback_token_estimates(
            LLMResponse& resp,
            const LLMRequest& req) {
        if (resp.prompt_tokens == 0) {
            for (const auto& m : req.messages)
                resp.prompt_tokens += ILLMBackend::estimate_tokens(m.content);
        }
        if (resp.completion_tokens == 0) {
            resp.completion_tokens = ILLMBackend::estimate_tokens(resp.content);
        }
    }
};

} // namespace agentos::kernel
