#pragma once
// ============================================================
// AgentOS × Corax — Streaming Analytics Tool Integration
//
// 将 Corax CLI (bin/corax) 注册为 AgentOS 工具，
// 使 ReAct Agent 能通过自然语言驱动实时流计算。
//
// Usage:
//   corax::register_corax_tools(tool_mgr.registry(), "/path/to/bin/corax");
// ============================================================

#include <agentos/tools/tool_manager.hpp>
#include <agentos/core/logger.hpp>

#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace agentos::corax {

// ─────────────────────────────────────────────────────────────
// § 内部：安全执行 Corax CLI 命令（fork/execvp，不经过 shell）
// ─────────────────────────────────────────────────────────────

struct ExecResult {
    bool success;
    std::string stdout_output;
    std::string stderr_output;
    int exit_code;
};

/// 直接 fork/execvp 执行 corax 命令，不经过 shell。
/// 安全：无 shell 注入风险，支持含特殊字符的参数。
inline ExecResult exec_corax(const std::string& binary_path,
                              const std::vector<std::string>& args,
                              const std::string& stdin_data = "",
                              std::chrono::seconds timeout = std::chrono::seconds{30},
                              std::stop_token st = {}) {
    // Build argv: [binary, args..., nullptr]
    std::vector<const char*> argv;
    argv.push_back(binary_path.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // stdout pipe
    int out_pipe[2];
    if (pipe(out_pipe) == -1)
        return {false, "", "Failed to create stdout pipe", -1};

    // stderr pipe
    int err_pipe[2];
    if (pipe(err_pipe) == -1) {
        close(out_pipe[0]); close(out_pipe[1]);
        return {false, "", "Failed to create stderr pipe", -1};
    }

    // stdin pipe (for feeding data)
    int in_pipe[2] = {-1, -1};
    if (!stdin_data.empty()) {
        if (pipe(in_pipe) == -1) {
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
            return {false, "", "Failed to create stdin pipe", -1};
        }
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (in_pipe[0] != -1) { close(in_pipe[0]); close(in_pipe[1]); }
        return {false, "", "Failed to fork", -1};
    }

    if (pid == 0) {
        // ── Child process ──
        // Redirect stdin if we have data
        if (in_pipe[0] != -1) {
            close(in_pipe[1]);
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
        }
        // Redirect stdout
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[1]);
        // Redirect stderr
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);

        // Execute: no shell, just execvp
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // ── Parent process ──
    close(out_pipe[1]);
    close(err_pipe[1]);

    // Write stdin data to child
    if (in_pipe[0] != -1) {
        close(in_pipe[0]);
        if (!stdin_data.empty()) {
            // Write in chunks to avoid SIGPIPE on large data
            const char* data = stdin_data.c_str();
            size_t remaining = stdin_data.size();
            while (remaining > 0) {
                ssize_t written = write(in_pipe[1], data, remaining);
                if (written <= 0) break;
                data += written;
                remaining -= static_cast<size_t>(written);
            }
        }
        close(in_pipe[1]);
    }

    // Cooperative cancellation
    std::stop_callback cancel_cb(st, [pid]() {
        kill(pid, SIGTERM);
    });

    // Read stdout + stderr with timeout using poll()
    std::string out_buf, err_buf;
    out_buf.reserve(8192);
    err_buf.reserve(1024);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;

    struct pollfd fds[2];
    fds[0] = {out_pipe[0], POLLIN, 0};
    fds[1] = {err_pipe[0], POLLIN, 0};
    int open_fds = 2;

    char buf[8192];
    while (open_fds > 0) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) { timed_out = true; break; }

        int ret = poll(fds, 2, static_cast<int>(std::min(remaining_ms, (long long)1000)));
        if (ret < 0) break;
        if (ret == 0) continue;  // timeout on this poll, retry

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) out_buf.append(buf, static_cast<size_t>(n));
            else { fds[0].fd = -1; open_fds--; }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) err_buf.append(buf, static_cast<size_t>(n));
            else { fds[1].fd = -1; open_fds--; }
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return {false, out_buf, "Corax command timed out", -1};
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {exit_code == 0, out_buf, err_buf, exit_code};
}

// ─────────────────────────────────────────────────────────────
// § 工具注册：将 Corax CLI 命令注册为 AgentOS 工具
// ─────────────────────────────────────────────────────────────

/// 注册所有 Corax 流计算工具到 AgentOS ToolRegistry
inline void register_corax_tools(tools::ToolRegistry& registry,
                                  const std::string& corax_binary) {

    // Validate binary exists
    if (!std::filesystem::exists(corax_binary)) {
        LOG_WARN(fmt::format("Corax binary not found: {}", corax_binary));
    }

    // ═══════════════════════════════════════════════════════════
    // Tool 1: corax_run — 一站式运行 pipeline
    // 这是最核心的工具：AI 生成 pipeline config → 运行 → 返回结果
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_run",
            .description =
                "Run a Corax streaming analytics pipeline. Accepts a JSON pipeline config "
                "and inline data, returns computed results. This is the primary tool for "
                "creating filters, aggregations, anomaly detection, alerts, and windowed "
                "computations on data.\n"
                "Pipeline config format: {\"name\":\"...\",\"schema\":[{\"name\":\"col\",\"type\":\"float64\"}],"
                "\"stages\":[{\"type\":\"filter\",\"expr\":\"price > 100\"}]}\n"
                "Stage types: filter, window, anomaly, dedup, topn, alert, reactive, cross_sectional, select\n"
                "Schema types: timestamp, string, float64, int64, boolean",
            .params = {
                {.name = "config",
                 .type = tools::ParamType::String,
                 .description = "Pipeline config as JSON string. Must include 'name', 'schema', and 'stages'.",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects, e.g. [{\"price\":150},{\"price\":50}]",
                 .required = true},
            },
            .timeout_ms = 60000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto config = args.get("config");
            auto data = args.get("data");
            if (config.empty()) return tools::ToolResult::fail("'config' parameter is required");
            if (data.empty()) return tools::ToolResult::fail("'data' parameter is required");

            auto result = exec_corax(corax_binary,
                {"run", config, "--inline", data, "--raw"},
                "", std::chrono::seconds{30}, st);

            if (!result.success) {
                return tools::ToolResult::fail(
                    result.stderr_output.empty()
                        ? "Corax pipeline execution failed (exit " + std::to_string(result.exit_code) + ")"
                        : result.stderr_output);
            }
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 2: corax_sql — SQL 查询
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_sql",
            .description =
                "Run a SQL query on inline data using Corax engine. "
                "Supports SELECT, WHERE, GROUP BY, ORDER BY, LIMIT, and aggregate functions "
                "(sum, avg, count, min, max). Schema is auto-inferred from data.\n"
                "Example: SELECT symbol, avg(price) FROM t WHERE price > 100 GROUP BY symbol",
            .params = {
                {.name = "sql",
                 .type = tools::ParamType::String,
                 .description = "SQL query string. Table name is always 't'.",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto sql = args.get("sql");
            auto data = args.get("data");
            if (sql.empty()) return tools::ToolResult::fail("'sql' parameter is required");
            if (data.empty()) return tools::ToolResult::fail("'data' parameter is required");

            auto result = exec_corax(corax_binary,
                {"sql", sql, "--inline", data, "--raw"},
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "SQL execution failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 3: corax_filter — 快捷过滤
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_filter",
            .description =
                "Filter rows from data using an expression. Schema is auto-inferred.\n"
                "Expression syntax: price > 100, status == \"active\", age >= 18 AND score > 90\n"
                "Operators: ==, !=, >, <, >=, <=, AND, OR, NOT",
            .params = {
                {.name = "expr",
                 .type = tools::ParamType::String,
                 .description = "Filter expression, e.g. 'price > 100 AND volume > 1000'",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto expr = args.get("expr");
            auto data = args.get("data");
            if (expr.empty() || data.empty())
                return tools::ToolResult::fail("'expr' and 'data' are required");

            auto result = exec_corax(corax_binary,
                {"filter", expr, "--inline", data, "--raw"},
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Filter failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 4: corax_agg — 聚合计算
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_agg",
            .description =
                "Compute an aggregate function on a column. Supports: avg, sum, count, min, max, stddev.\n"
                "Optionally group by a key column and/or apply a time window.",
            .params = {
                {.name = "function",
                 .type = tools::ParamType::String,
                 .description = "Aggregate function: avg, sum, count, min, max, stddev",
                 .required = true},
                {.name = "column",
                 .type = tools::ParamType::String,
                 .description = "Column to aggregate",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
                {.name = "group_by",
                 .type = tools::ParamType::String,
                 .description = "Optional: column to group by",
                 .required = false},
                {.name = "window",
                 .type = tools::ParamType::String,
                 .description = "Optional: time window size, e.g. '60s', '5m', '1h'",
                 .required = false},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto func = args.get("function");
            auto col = args.get("column");
            auto data = args.get("data");
            if (func.empty() || col.empty() || data.empty())
                return tools::ToolResult::fail("'function', 'column', and 'data' are required");

            std::vector<std::string> cmd_args = {"agg", func, col, "--inline", data, "--raw"};

            auto group_by = args.get("group_by");
            if (!group_by.empty()) {
                cmd_args.push_back("--by");
                cmd_args.push_back(group_by);
            }
            auto window = args.get("window");
            if (!window.empty()) {
                cmd_args.push_back("--window");
                cmd_args.push_back(window);
            }

            auto result = exec_corax(corax_binary, cmd_args,
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Aggregation failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 5: corax_anomaly — 异常检测
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_anomaly",
            .description =
                "Detect anomalies in a numeric column using statistical methods.\n"
                "Methods: zscore (default, flags values beyond N standard deviations), "
                "iqr (interquartile range based).\n"
                "Returns rows flagged as anomalous with anomaly scores.",
            .params = {
                {.name = "column",
                 .type = tools::ParamType::String,
                 .description = "Numeric column to check for anomalies",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
                {.name = "method",
                 .type = tools::ParamType::String,
                 .description = "Detection method: 'zscore' (default) or 'iqr'",
                 .required = false,
                 .default_value = "zscore"},
                {.name = "threshold",
                 .type = tools::ParamType::Float,
                 .description = "Threshold for anomaly detection (default: 3.0 for zscore)",
                 .required = false,
                 .default_value = "3.0"},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto col = args.get("column");
            auto data = args.get("data");
            if (col.empty() || data.empty())
                return tools::ToolResult::fail("'column' and 'data' are required");

            std::vector<std::string> cmd_args = {"anomaly", col, "--inline", data, "--raw"};

            auto method = args.get("method", "zscore");
            cmd_args.push_back("--method");
            cmd_args.push_back(method);

            auto threshold = args.get("threshold", "3.0");
            cmd_args.push_back("--threshold");
            cmd_args.push_back(threshold);

            auto result = exec_corax(corax_binary, cmd_args,
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Anomaly detection failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 6: corax_alert — 条件告警
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_alert",
            .description =
                "Find rows matching an alert condition. Use this for monitoring thresholds.\n"
                "Example: alert when cpu > 90, alert when price drop > 1%",
            .params = {
                {.name = "condition",
                 .type = tools::ParamType::String,
                 .description = "Alert condition expression, e.g. 'cpu > 90' or 'change_pct < -1.0'",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto cond = args.get("condition");
            auto data = args.get("data");
            if (cond.empty() || data.empty())
                return tools::ToolResult::fail("'condition' and 'data' are required");

            auto result = exec_corax(corax_binary,
                {"alert", cond, "--inline", data, "--raw"},
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Alert check failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 7: corax_top — Top N
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_top",
            .description =
                "Get the top N rows ranked by a column. Useful for leaderboards, "
                "ranking stocks by performance, finding top movers, etc.",
            .params = {
                {.name = "n",
                 .type = tools::ParamType::Integer,
                 .description = "Number of top rows to return",
                 .required = true},
                {.name = "column",
                 .type = tools::ParamType::String,
                 .description = "Column to sort/rank by",
                 .required = true},
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
                {.name = "group_by",
                 .type = tools::ParamType::String,
                 .description = "Optional: group by column (top N per group)",
                 .required = false},
            },
            .timeout_ms = 30000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto n = args.get("n", "10");
            auto col = args.get("column");
            auto data = args.get("data");
            if (col.empty() || data.empty())
                return tools::ToolResult::fail("'column' and 'data' are required");

            std::vector<std::string> cmd_args = {"top", n, col, "--inline", data, "--raw"};

            auto group_by = args.get("group_by");
            if (!group_by.empty()) {
                cmd_args.push_back("--by");
                cmd_args.push_back(group_by);
            }

            auto result = exec_corax(corax_binary, cmd_args,
                "", std::chrono::seconds{30}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Top-N failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 8: corax_schema — 推断数据 Schema
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_schema",
            .description =
                "Infer the schema (column names and types) from a JSON data sample. "
                "Useful as a first step before creating pipelines. "
                "Returns [{\"name\":\"col\",\"type\":\"float64\"}, ...]",
            .params = {
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Sample data as JSON array of objects",
                 .required = true},
            },
            .timeout_ms = 10000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto data = args.get("data");
            if (data.empty()) return tools::ToolResult::fail("'data' parameter is required");

            // Write data to a temp file for schema inference
            auto tmp = std::filesystem::temp_directory_path() / "corax_schema_tmp.json";
            {
                std::ofstream f(tmp);
                f << data;
            }

            auto result = exec_corax(corax_binary,
                {"schema", tmp.string()},
                "", std::chrono::seconds{10}, st);

            std::filesystem::remove(tmp);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Schema inference failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 9: corax_capabilities — 列出引擎能力
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_capabilities",
            .description =
                "List all available Corax streaming engine types and their parameters. "
                "Call this to discover what pipeline stages are available.",
            .params = {},
            .timeout_ms = 5000,
        },
        [corax_binary](const tools::ParsedArgs& /*args*/, std::stop_token st) -> tools::ToolResult {
            auto result = exec_corax(corax_binary, {"capabilities"},
                "", std::chrono::seconds{5}, st);

            if (!result.success)
                return tools::ToolResult::fail("Failed to list capabilities");
            return tools::ToolResult::ok(result.stdout_output);
        });

    // ═══════════════════════════════════════════════════════════
    // Tool 10: corax_count — 数据行数统计
    // ═══════════════════════════════════════════════════════════
    registry.register_fn(
        tools::ToolSchema{
            .id = "corax_count",
            .description =
                "Count rows in data, optionally filtering by a condition.\n"
                "Example: count all rows where price > 100",
            .params = {
                {.name = "data",
                 .type = tools::ParamType::String,
                 .description = "Input data as JSON array of objects",
                 .required = true},
                {.name = "where",
                 .type = tools::ParamType::String,
                 .description = "Optional: filter condition before counting",
                 .required = false},
            },
            .timeout_ms = 10000,
        },
        [corax_binary](const tools::ParsedArgs& args, std::stop_token st) -> tools::ToolResult {
            auto data = args.get("data");
            if (data.empty()) return tools::ToolResult::fail("'data' parameter is required");

            std::vector<std::string> cmd_args = {"count", "--inline", data};

            auto where = args.get("where");
            if (!where.empty()) {
                cmd_args.push_back("--where");
                cmd_args.push_back(where);
            }

            auto result = exec_corax(corax_binary, cmd_args,
                "", std::chrono::seconds{10}, st);

            if (!result.success)
                return tools::ToolResult::fail(result.stderr_output.empty()
                    ? "Count failed" : result.stderr_output);
            return tools::ToolResult::ok(result.stdout_output);
        });

    LOG_INFO(fmt::format("Corax tools registered (10 tools, binary: {})", corax_binary));
}

} // namespace agentos::corax
