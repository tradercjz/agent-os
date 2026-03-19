// ============================================================
// AgentOS Headless Runner Demo
// 演示无头模式：JSON 配置 → 执行任务 → 结构化结果
// 用于 CI/CD、GitHub Actions、Webhook 集成
// ============================================================
#include <agentos/headless/runner.hpp>
#include <iostream>

using namespace agentos;
using namespace agentos::headless;

void ok(std::string_view msg) {
  std::cout << "\033[32m  ✓ " << msg << "\033[0m\n";
}
void info(std::string_view msg) {
  std::cout << "\033[34m  · " << msg << "\033[0m\n";
}
void section(std::string_view title) {
  std::cout << "\n\033[1;36m══════════════════════════════════════\033[0m\n  "
            << "\033[1;33m" << title << "\033[0m\n"
            << "\033[1;36m══════════════════════════════════════\033[0m\n";
}

int main() {
    // ── 1. 基本用法 ──
    section("1. 基本用法 (RunRequest)");

    HeadlessRunner runner(
        std::make_unique<kernel::MockLLMBackend>("mock"),
        AgentOS::Config::builder().scheduler_threads(1).build());

    RunRequest req;
    req.task = "分析这段代码并找出潜在的 bug";
    req.agent_name = "code-reviewer";
    req.role_prompt = "你是一个资深代码审查员，擅长发现安全漏洞和性能问题。";
    req.timeout = Duration{10000};
    req.context_limit = 4096;

    info(("任务: " + req.task).c_str());
    info(("Agent: " + req.agent_name).c_str());
    info(("超时: " + std::to_string(req.timeout.count()) + "ms").c_str());

    auto result = runner.run(req);

    info(("成功: " + std::string(result.success ? "是" : "否")).c_str());
    info(("输出: " + result.output.substr(0, 60) + "...").c_str());
    info(("耗时: " + std::to_string(result.duration_ms) + "ms").c_str());
    info(("Token: " + std::to_string(result.tokens_used)).c_str());
    ok("任务执行完成");

    // ── 2. 从 JSON 字符串运行 ──
    section("2. 从 JSON 运行 (run_json)");
    info("模拟 webhook 或 CI 触发的 JSON payload:");

    std::string json_payload = R"({
        "task": "修复 #1234 Bug: 用户登录时 token 未刷新",
        "agent_name": "bug-fixer",
        "role_prompt": "你是一个自动修复 agent，根据 bug 描述修复代码。",
        "timeout_ms": 30000,
        "context_limit": 8192
    })";

    info("JSON payload:");
    info("  task: 修复 #1234 Bug");
    info("  agent: bug-fixer");
    info("  timeout: 30000ms");

    auto result2 = runner.run_json(json_payload);
    info(("结果: " + std::string(result2.success ? "成功" : "失败")).c_str());
    ok("JSON 触发执行完成");

    // ── 3. 结果序列化 ──
    section("3. 结果序列化 (to_json)");
    info("RunResult 可序列化为 JSON，用于 API 响应或日志:");

    auto result_json = result.to_json();
    info("JSON 输出:");
    info(("  success: " + std::to_string(result_json["success"].get<bool>())).c_str());
    info(("  duration_ms: " + std::to_string(result_json["duration_ms"].get<uint64_t>())).c_str());
    info(("  tokens_used: " + std::to_string(result_json["tokens_used"].get<uint64_t>())).c_str());
    ok("结果已序列化");

    // ── 4. 注册自定义工具 ──
    section("4. 注册自定义工具后运行");
    info("通过 runner.os() 访问底层 AgentOS，注册工具:");

    runner.os().register_tool(
        tools::ToolSchema{.id = "jira_update", .description = "更新 JIRA ticket 状态"},
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string ticket = args.values.count("ticket")
                ? args.values.at("ticket") : "UNKNOWN";
            return tools::ToolResult::ok("Updated " + ticket + " → Done");
        });
    info("已注册: jira_update");

    RunRequest req3;
    req3.task = "修复完成后更新 JIRA ticket";
    req3.allowed_tools = {"jira_update"};

    auto result3 = runner.run(req3);
    info(("结果: " + std::string(result3.success ? "成功" : "失败")).c_str());
    ok("带自定义工具的任务执行完成");

    // ── 5. 错误处理 ──
    section("5. 错误处理");

    // 空任务
    auto err1 = runner.run(RunRequest{});
    info(("空任务 → error: " + err1.error).c_str());

    // 无效 JSON
    auto err2 = runner.run_json("not valid json");
    info(("无效 JSON → error: " + err2.error).c_str());
    ok("错误正确处理");

    // ── 6. CI/CD 集成模式 ──
    section("6. CI/CD 集成模式");
    info("典型 GitHub Actions 工作流:");
    info("  1. Webhook 触发 → 解析 event payload");
    info("  2. 构造 RunRequest JSON");
    info("  3. HeadlessRunner::run_json(payload)");
    info("  4. 检查 RunResult.success");
    info("  5. 输出 RunResult.to_json() 到 GHA 日志");
    info("  6. 成功 → 创建 PR / 失败 → 报告错误");
    info("");
    info("数据飞轮: Bug → Agent 修复 → 日志分析 → 改进 prompt");
    ok("Headless Runner 演示完成");

    std::cout << "\n\033[1;32m🎉 Headless Runner Demo 完成！\033[0m\n\n";
    return 0;
}
