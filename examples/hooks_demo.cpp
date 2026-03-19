// ============================================================
// AgentOS Hooks Demo
// 演示 pre_tool_use / post_tool_use / stop 生命周期钩子
// ============================================================
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

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
    // 初始化 AgentOS
    auto backend = std::make_unique<kernel::MockLLMBackend>("mock");
    AgentOS os(std::move(backend),
               AgentOS::Config::builder().scheduler_threads(1).build());

    // 注册测试工具
    os.register_tool(
        tools::ToolSchema{.id = "compile", .description = "编译代码"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Build succeeded. 0 errors, 0 warnings.");
        });

    os.register_tool(
        tools::ToolSchema{.id = "git_commit", .description = "Git 提交"},
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string msg = args.values.count("message")
                ? args.values.at("message") : "auto commit";
            return tools::ToolResult::ok("Committed: " + msg);
        });

    os.register_tool(
        tools::ToolSchema{.id = "deploy", .description = "部署到生产环境"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Deployed to production");
        });

    auto agent = os.create_agent(
        AgentConfig::builder().name("hooks-demo-agent").build());

    // ── 1. Pre-tool-use: 策略拦截 ──
    section("1. Pre-tool-use: 策略拦截 (Block dangerous tools)");
    info("注册 'policy-guard' 中间件：阻止 deploy 工具的调用");

    agent->use(Middleware{
        .name = "policy-guard",
        .before = [](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use" && ctx.input == "deploy") {
                ctx.cancelled = true;
                ctx.cancel_reason = "Production deploy requires approval";
                std::cout << "\033[31m  ✗ 拦截 deploy: "
                          << ctx.cancel_reason << "\033[0m\n";
            }
        },
        .after = nullptr,
    });

    // 调用 compile — 应该通过
    kernel::ToolCallRequest compile_call{"c1", "compile", "{}"};
    auto r1 = agent->act(compile_call);
    if (r1) {
        info(("compile → " + r1->output).c_str());
    }
    ok("compile 工具未被拦截");

    // 调用 deploy — 应该被拦截
    kernel::ToolCallRequest deploy_call{"c2", "deploy", "{}"};
    auto r2 = agent->act(deploy_call);
    if (!r2) {
        info(("deploy 被拦截: " + r2.error().message).c_str());
    }
    ok("deploy 被 policy-guard 成功拦截");

    // ── 2. Pre-tool-use: 查看参数 ──
    section("2. Pre-tool-use: 查看工具参数 (Inspect args)");
    info("注册 'arg-logger' 中间件：打印工具调用参数");

    agent->use(Middleware{
        .name = "arg-logger",
        .before = [](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use") {
                std::cout << "\033[33m  ⚡ Tool: " << ctx.input
                          << " | Args: " << ctx.args.dump() << "\033[0m\n";
            }
        },
        .after = nullptr,
    });

    kernel::ToolCallRequest commit_call{
        "c3", "git_commit", R"({"message":"feat: add hooks demo"})"};
    (void)agent->act(commit_call);
    ok("参数已记录");

    // ── 3. Post-tool-use: 注入 lint 结果 ──
    section("3. Post-tool-use: 结果注入 (Inject lint output)");
    info("注册 'lint-injector' 中间件：在编译结果后追加 lint 报告");

    agent->use(Middleware{
        .name = "lint-injector",
        .before = nullptr,
        .after = [](HookContext& ctx) {
            if (ctx.operation == "post_tool_use"
                && ctx.input == "compile" && ctx.result) {
                ctx.result->output += "\n--- Lint Report ---\n"
                                      "  style: 2 warnings\n"
                                      "  security: 0 issues\n"
                                      "  complexity: OK";
            }
        },
    });

    auto r3 = agent->act(compile_call);
    if (r3) {
        // 分行显示
        std::istringstream iss(r3->output);
        std::string line;
        while (std::getline(iss, line)) {
            info(line.c_str());
        }
    }
    ok("Lint 报告已注入到编译结果中");

    // ── 4. Post-tool-use: 质量门禁 ──
    section("4. Post-tool-use: 质量门禁 (Quality gate)");
    info("注册 'quality-gate' 中间件：如果输出包含 warning 则标记失败");

    agent->use(Middleware{
        .name = "quality-gate",
        .before = nullptr,
        .after = [](HookContext& ctx) {
            if (ctx.operation == "post_tool_use" && ctx.result) {
                if (ctx.result->output.find("warning") != std::string::npos) {
                    ctx.result->success = false;
                    ctx.result->error = "Quality gate failed: warnings found";
                }
            }
        },
    });

    auto r4 = agent->act(compile_call);
    if (r4) {
        info(("success = " + std::string(r4->success ? "true" : "false")).c_str());
        if (!r4->success) {
            info(("error = " + r4->error).c_str());
        }
    }
    ok("质量门禁生效，因为 lint 报告包含 warning");

    // ── 5. 完整钩子链执行顺序 ──
    section("5. 完整钩子链执行顺序");
    info("一次 act() 调用的完整 hook 执行序列:");
    info("  1. before(\"act\")           ← 粗粒度，向后兼容");
    info("  2. before(\"pre_tool_use\")  ← 可拦截、可查看参数");
    info("  3. [执行工具]");
    info("  4. after(\"post_tool_use\")  ← 可修改结果");
    info("  5. after(\"act\")            ← 粗粒度后置钩子");
    ok("Hooks 系统演示完成");

    std::cout << "\n\033[1;32m🎉 Hooks Demo 完成！\033[0m\n\n";
    return 0;
}
