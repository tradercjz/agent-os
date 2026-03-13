#pragma once
#include <agentos/core/types.hpp>
#include <concepts>
#include <string>

namespace agentos {

namespace kernel {
    struct LLMResponse;
    struct ToolCallRequest;
}

namespace tools {
    struct ToolResult;
}

template <typename T>
concept AgentLike = requires(T a, std::string input) {
    { a.run(input) } -> std::same_as<Result<std::string>>;
    { a.id() } -> std::convertible_to<AgentId>;
};

template <typename T>
concept ReflectiveAgentLike = AgentLike<T> && requires(T a, std::string plan) {
    { a.reflect(plan) } -> std::convertible_to<std::optional<std::string>>;
};

template <typename T>
concept ToolLike = requires(T t, const kernel::ToolCallRequest& call) {
    { t.execute(call) } -> std::same_as<Result<tools::ToolResult>>;
};

} // namespace agentos
