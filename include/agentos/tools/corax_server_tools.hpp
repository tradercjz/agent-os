#pragma once
// ============================================================
// AgentOS × Corax — Server Mode Tool Integration
//
// 连接持久运行的 corax-server，支持：
//   - 创建/管理长期运行的 pipeline
//   - 持续喂数据（实时监控场景）
//   - 实时查询结果和告警
//
// Usage:
//   corax::register_corax_server_tools(tool_mgr.registry(), "http://localhost:8080");
// ============================================================

#include <agentos/tools/tool_manager.hpp>
#include <agentos/kernel/http_client.hpp>
#include <agentos/core/logger.hpp>

#include <memory>
#include <string>

namespace agentos::corax {

// ─────────────────────────────────────────────────────────────
// § 内部：轻量 HTTP helper，复用 AgentOS 的 HttpClient
// ─────────────────────────────────────────────────────────────

namespace detail {

/// 对 corax-server 发 POST 请求
inline tools::ToolResult http_post(const std::string& base_url,
                                    const std::string& path,
                                    const std::string& body,
                                    const std::string& api_key = "",
                                    int timeout_sec = 30) {
    static kernel::HttpClient client;

    std::string url = base_url + path;
    std::vector<std::string> headers = {"Content-Type: application/json"};
    if (!api_key.empty()) {
        headers.push_back("X-API-Key: " + api_key);
    }

    auto resp = client.post(url, body, headers, timeout_sec);
    if (!resp) {
        return tools::ToolResult::fail("HTTP request failed: " + resp.error().message);
    }

    if (resp->status_code >= 400) {
        return tools::ToolResult::fail(
            "Server returned " + std::to_string(resp->status_code) + ": " + resp->body);
    }

    return tools::ToolResult::ok(resp->body);
}

/// 对 corax-server 发 GET 请求（POST with empty body，server 会按路由处理）
/// 注意：corax-server 对 GET 路由也接受 POST，所以统一用 POST + empty body
inline tools::ToolResult http_get(const std::string& base_url,
                                   const std::string& path,
                                   const std::string& api_key = "",
                                   int timeout_sec = 15) {
    return http_post(base_url, path, "", api_key, timeout_sec);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────
// § Server 模式工具注册
// ─────────────────────────────────────────────────────────────

/// 注册 Corax Server 模式工具（连接持久运行的 corax-server）
/// @param registry  AgentOS ToolRegistry
/// @param base_url  corax-server 地址，如 "http://localhost:8080"
/// @param api_key   可选的 API key
inline void register_corax_server_tools(tools::ToolRegistry& registry,
                                         const std::string& base_url,
                                         const std::string& api_key = "") {

    // ═══════════════════════════════════════════════════════════
    // Tool 1: corax_create_pipeline — 创建持久 pipeline
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_create_pipeline",
            .description =
                "Create a persistent streaming pipeline on the Corax server. "
                "The pipeline stays alive and accumulates data across multiple feed calls. "
                "Use this for real-time monitoring scenarios.\n"
                "After creating, call corax_start_pipeline to begin processing, "
                "then corax_feed_data to push data, and corax_get_results to check output.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Unique pipeline name, e.g. 'maotai_monitor'",
                 .required = true},
                {.name = "config",
                 .type = tools::ParamType::String,
                 .description = "Pipeline config as JSON string with schema and stages",
                 .required = true},
            },
            .timeout_ms = 15000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            auto config = args.get("config");
            if (name.empty() || config.empty())
                return tools::ToolResult::fail("'name' and 'config' are required");

            // Server expects: {"name": "...", "config": "<json-string>"}
            Json body;
            body["name"] = name;
            body["config"] = config;

            return detail::http_post(base_url, "/create", body.dump(), api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 2: corax_start_pipeline — 启动 pipeline
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_start_pipeline",
            .description =
                "Start a previously created pipeline. Must be called after corax_create_pipeline.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name to start",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            if (name.empty()) return tools::ToolResult::fail("'name' is required");

            return detail::http_post(base_url, "/start/" + name, "", api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 3: corax_feed_data — 喂数据到运行中的 pipeline
    // 这是持续监控的关键：定时调用，推送新数据
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_feed_data",
            .description =
                "Feed data into a running pipeline. Data accumulates across calls. "
                "Use this to push new market data, sensor readings, log entries, etc.\n"
                "Can be called repeatedly to simulate real-time streaming.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name to feed data into",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 30000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            auto data = args.get("data");
            if (name.empty() || data.empty())
                return tools::ToolResult::fail("'name' and 'data' are required");

            return detail::http_post(base_url, "/feed/" + name, data, api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 4: corax_get_results — 获取 pipeline 计算结果
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_get_results",
            .description =
                "Get accumulated results from a running pipeline. "
                "Returns all computed output rows (filtered data, aggregations, anomalies, alerts).\n"
                "For monitoring: call this periodically to check for new alerts.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name to get results from",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            if (name.empty()) return tools::ToolResult::fail("'name' is required");

            return detail::http_get(base_url, "/results/" + name, api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 5: corax_get_metrics — 获取 pipeline 运行指标
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_get_metrics",
            .description =
                "Get runtime metrics of a pipeline (rows processed, latency, throughput, etc.).",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            if (name.empty()) return tools::ToolResult::fail("'name' is required");

            return detail::http_get(base_url, "/metrics/" + name, api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 6: corax_list_pipelines — 列出所有 pipeline
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_list_pipelines",
            .description =
                "List all pipelines on the Corax server with their status (running/stopped).",
            .params = {},
            .timeout_ms = 5000,
        },
        [base_url, api_key](const tools::ParsedArgs&, std::stop_token) -> tools::ToolResult {
            return detail::http_get(base_url, "/list", api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 7: corax_stop_pipeline — 停止 pipeline
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_stop_pipeline",
            .description =
                "Stop a running pipeline. Results are preserved and can still be queried.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name to stop",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            if (name.empty()) return tools::ToolResult::fail("'name' is required");

            return detail::http_post(base_url, "/stop/" + name, "", api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 8: corax_remove_pipeline — 删除 pipeline
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_remove_pipeline",
            .description =
                "Remove a pipeline from the server. Stops it first if running. "
                "All data and results are lost.",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name to remove",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            if (name.empty()) return tools::ToolResult::fail("'name' is required");

            return detail::http_post(base_url, "/remove/" + name, "", api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 9: corax_update_stage — 热更新 pipeline stage
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_update_stage",
            .description =
                "Hot-update a stage in a running pipeline without restart. "
                "Can change filter expressions, window sizes, anomaly thresholds, etc.\n"
                "Example: change a filter from 'price > 100' to 'price > 200'",
            .params = {
                {.name = "name",
                 .type = tools::ParamType::String,
                 .description = "Pipeline name",
                 .required = true},
                {.name = "stage",
                 .type = tools::ParamType::String,
                 .description = "Stage update as JSON, e.g. {\"type\":\"filter\",\"engine_name\":\"filter_0\",\"expr\":\"price > 200\"}",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto name = args.get("name");
            auto stage = args.get("stage");
            if (name.empty() || stage.empty())
                return tools::ToolResult::fail("'name' and 'stage' are required");

            return detail::http_post(base_url, "/update/" + name, stage, api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 10: corax_server_health — 健康检查
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_server_health",
            .description =
                "Check if the Corax server is running and healthy.",
            .params = {},
            .timeout_ms = 5000,
        },
        [base_url, api_key](const tools::ParsedArgs&, std::stop_token) -> tools::ToolResult {
            return detail::http_get(base_url, "/health", api_key);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 11: corax_server_sql — 无状态 SQL 查询
    // 不需要创建 pipeline，直接查
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_server_sql",
            .description =
                "Run a stateless SQL query on the Corax server. "
                "No pipeline creation needed — sends data and SQL, gets results immediately.",
            .params = {
                {.name = "sql",
                 .type = tools::ParamType::String,
                 .description = "SQL query. Table name is 't'.",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 30000,
        },
        [base_url, api_key](const tools::ParsedArgs& args, std::stop_token) -> tools::ToolResult {
            auto sql = args.get("sql");
            auto data = args.get("data");
            if (sql.empty() || data.empty())
                return tools::ToolResult::fail("'sql' and 'data' are required");

            Json body;
            body["sql"] = sql;
            body["data"] = Json::parse(data);

            return detail::http_post(base_url, "/sql", body.dump(), api_key);
        });

    LOG_INFO(fmt::format("Corax Server tools registered (11 tools, server: {})", base_url));
}

/// Server 模式的 tool ID 列表
inline std::vector<std::string> corax_server_tool_ids() {
    return {
        "corax_create_pipeline", "corax_start_pipeline", "corax_feed_data",
        "corax_get_results", "corax_get_metrics", "corax_list_pipelines",
        "corax_stop_pipeline", "corax_remove_pipeline", "corax_update_stage",
        "corax_server_health", "corax_server_sql"
    };
}

/// 全部 Corax 工具 ID（CLI + Server 模式合并）
inline std::vector<std::string> all_corax_tool_ids() {
    auto cli = corax_tool_ids();
    auto srv = corax_server_tool_ids();
    cli.insert(cli.end(), srv.begin(), srv.end());
    return cli;
}

} // namespace agentos::corax
