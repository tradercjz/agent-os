#pragma once
// ============================================================
// AgentOS × Corax — 数据源工具
//
// 将股票数据拉取注册为 AgentOS 工具，
// 使 Agent 能自动获取行情数据并喂入 Corax pipeline。
//
// 调用 tools/fetch_stock.py，通过 fork/execvp 安全执行。
//
// Usage:
//   corax::register_corax_data_tools(registry, "/path/to/tools/fetch_stock.py");
// ============================================================

#include <agentos/tools/tool_manager.hpp>
#include <agentos/tools/corax_tools.hpp>  // for exec_corax (reuse exec infrastructure)
#include <agentos/core/logger.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace agentos::corax {

/// 注册数据源相关工具
inline void register_corax_data_tools(tools::ToolRegistry& registry,
                                       const std::string& fetch_script_path) {

    // Validate script exists
    if (!std::filesystem::exists(fetch_script_path)) {
        LOG_WARN(fmt::format("fetch_stock.py not found: {}", fetch_script_path));
    }

    // ═══════════════════════════════════════════════════════════
    // Tool 1: fetch_stock_data — 获取股票行情数据
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "fetch_stock_data",
            .description =
                "Fetch stock market data from tushare. Returns OHLCV data in Corax-compatible "
                "JSON format (with schema and timestamps).\n"
                "Supports: A-share stocks, daily and minute-level data.\n"
                "The returned 'data' field can be directly fed into Corax pipeline tools.\n"
                "Example codes: 600519 (茅台), 000858 (五粮液), 300750 (宁德时代)",
            .params = {
                {.name = "code",
                 .type = tools::ParamType::String,
                 .description = "Stock code(s), comma-separated. e.g. '600519' or '600519,000858'",
                 .required = false},
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Stock name (Chinese, fuzzy match). e.g. '茅台'. Alternative to code.",
                 .required = false},
                {.name = "period",
                 .type = tools::ParamType::String,
                 .description = "Data period: 'daily' (default), '1min', '5min', '15min', '30min', '60min'",
                 .required = false,
                 .default_value = "daily"},
                {.name = "limit",
                 .type = tools::ParamType::Integer,
                 .description = "Max rows per stock (default: 30)",
                 .required = false,
                 .default_value = "30"},
            },
            .timeout_ms = 30000,
        },
        [fetch_script_path](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto code = args.get("code");
            auto name = args.get("name");
            if (code.empty() && name.empty())
                return tools::ToolResult::fail("Provide 'code' (e.g. '600519') or 'name' (e.g. '茅台')");

            // Build argv for: python3 fetch_stock.py --code/--name X --period Y --limit Z
            std::vector<std::string> py_args = {fetch_script_path};

            if (!code.empty()) {
                py_args.push_back("--code");
                py_args.push_back(code);
            } else {
                py_args.push_back("--name");
                py_args.push_back(name);
            }

            auto period = args.get("period", "daily");
            py_args.push_back("--period");
            py_args.push_back(period);

            auto limit = args.get("limit", "30");
            py_args.push_back("--limit");
            py_args.push_back(limit);

            auto result = exec_corax("python3", py_args,
                "", std::chrono::seconds{25}, st);

            if (!result.success) {
                // Try parsing error from stdout (script returns JSON errors)
                if (!result.stdout_output.empty())
                    return tools::ToolResult::fail(result.stdout_output);
                return tools::ToolResult::fail(
                    result.stderr_output.empty()
                        ? "Failed to fetch stock data" : result.stderr_output);
            }
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 2: search_stocks — 搜索股票
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "search_stocks",
            .description =
                "Search A-share stocks by name, code, or industry keyword.\n"
                "Returns matching stock codes and names. Use this when the user "
                "mentions a stock name and you need to find the exact code.\n"
                "Example: search '新能源' returns all new-energy stocks.",
            .params = {
                {.name = "keyword",
                 .type = tools::ParamType::String,
                 .description = "Search keyword (stock name, code, or industry in Chinese)",
                 .required = true},
                {.name = "limit",
                 .type = tools::ParamType::Integer,
                 .description = "Max results (default: 10)",
                 .required = false,
                 .default_value = "10"},
            },
            .timeout_ms = 15000,
        },
        [fetch_script_path](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto keyword = args.get("keyword");
            if (keyword.empty())
                return tools::ToolResult::fail("'keyword' is required");

            auto limit = args.get("limit", "10");

            auto result = exec_corax("python3",
                {fetch_script_path, "--search", keyword, "--limit", limit},
                 "", std::chrono::seconds{15}, st);

            if (!result.success) {
                if (!result.stdout_output.empty())
                    return tools::ToolResult::fail(result.stdout_output);
                return tools::ToolResult::fail(
                    result.stderr_output.empty()
                        ? "Stock search failed" : result.stderr_output);
            }
            return tools::ToolResult::ok(result.stdout_output);
        });

    LOG_INFO(fmt::format("Corax data tools registered (2 tools, script: {})", fetch_script_path));
}

/// 数据源工具 ID 列表
inline std::vector<std::string> corax_data_tool_ids() {
    return {"fetch_stock_data", "search_stocks"};
}

} // namespace agentos::corax
