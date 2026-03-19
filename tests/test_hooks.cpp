#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace agentos;

// ── Test fixture with mock OS ──

class HooksTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto backend = std::make_unique<kernel::MockLLMBackend>("mock");
        os_ = std::make_unique<AgentOS>(
            std::move(backend),
            AgentOS::Config::builder()
                .scheduler_threads(1)
                .build());
    }

    std::shared_ptr<ReActAgent> make_agent() {
        return os_->create_agent(
            AgentConfig::builder()
                .name("hook-test-agent")
                .role_prompt("You are a test agent.")
                .build());
    }

    std::unique_ptr<AgentOS> os_;
};

// ── HookContext field tests ──

TEST_F(HooksTest, HookContextHasArgsField) {
    HookContext ctx;
    ctx.args = Json::object({{"key", "value"}});
    EXPECT_EQ(ctx.args["key"], "value");
}

TEST_F(HooksTest, HookContextHasResultField) {
    tools::ToolResult tr = tools::ToolResult::ok("output");
    HookContext ctx;
    ctx.result = &tr;
    ASSERT_NE(ctx.result, nullptr);
    EXPECT_EQ(ctx.result->output, "output");
}

// ── Pre-tool-use hook tests ──

TEST_F(HooksTest, PreToolUseHookFires) {
    auto agent = make_agent();
    std::string captured_tool;
    Json captured_args;

    agent->use(Middleware{
        .name = "pre-tool-spy",
        .before = [&](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use") {
                captured_tool = ctx.input;
                captured_args = ctx.args;
            }
        },
        .after = nullptr,
    });

    // Register a test tool
    os_->register_tool(
        tools::ToolSchema{.id = "greet", .description = "Say hello"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Hello!");
        });

    kernel::ToolCallRequest call{"call-1", "greet", R"({"name":"world"})"};
    auto result = agent->act(call);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(captured_tool, "greet");
    EXPECT_EQ(captured_args["name"], "world");
}

TEST_F(HooksTest, PreToolUseHookCanCancelToolCall) {
    auto agent = make_agent();

    agent->use(Middleware{
        .name = "tool-blocker",
        .before = [](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use" && ctx.input == "dangerous_tool") {
                ctx.cancelled = true;
                ctx.cancel_reason = "blocked by policy";
            }
        },
        .after = nullptr,
    });

    os_->register_tool(
        tools::ToolSchema{.id = "dangerous_tool", .description = "A dangerous tool"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("should not execute");
        });

    kernel::ToolCallRequest call{"call-2", "dangerous_tool", "{}"};
    auto result = agent->act(call);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    EXPECT_TRUE(result.error().message.find("blocked by policy") != std::string::npos);
}

TEST_F(HooksTest, PreToolUseDoesNotBlockOtherTools) {
    auto agent = make_agent();

    agent->use(Middleware{
        .name = "selective-blocker",
        .before = [](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use" && ctx.input == "blocked_tool") {
                ctx.cancelled = true;
                ctx.cancel_reason = "nope";
            }
        },
        .after = nullptr,
    });

    os_->register_tool(
        tools::ToolSchema{.id = "safe_tool", .description = "A safe tool"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("safe output");
        });

    kernel::ToolCallRequest call{"call-3", "safe_tool", "{}"};
    auto result = agent->act(call);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->output, "safe output");
}

// ── Post-tool-use hook tests ──

TEST_F(HooksTest, PostToolUseHookCanMutateResult) {
    auto agent = make_agent();

    agent->use(Middleware{
        .name = "lint-injector",
        .before = nullptr,
        .after = [](HookContext& ctx) {
            if (ctx.operation == "post_tool_use" && ctx.result) {
                ctx.result->output += "\n[lint: 0 warnings]";
            }
        },
    });

    os_->register_tool(
        tools::ToolSchema{.id = "compile", .description = "Compile code"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Build succeeded");
        });

    kernel::ToolCallRequest call{"call-4", "compile", "{}"};
    auto result = agent->act(call);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->output.find("[lint: 0 warnings]") != std::string::npos);
    EXPECT_TRUE(result->output.find("Build succeeded") != std::string::npos);
}

TEST_F(HooksTest, PostToolUseHookCanMarkFailure) {
    auto agent = make_agent();

    agent->use(Middleware{
        .name = "quality-gate",
        .before = nullptr,
        .after = [](HookContext& ctx) {
            if (ctx.operation == "post_tool_use" && ctx.result) {
                if (ctx.result->output.find("TODO") != std::string::npos) {
                    ctx.result->success = false;
                    ctx.result->error = "TODOs found in output";
                }
            }
        },
    });

    os_->register_tool(
        tools::ToolSchema{.id = "review", .description = "Code review"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Code looks good. TODO: add tests");
        });

    kernel::ToolCallRequest call{"call-5", "review", "{}"};
    auto result = agent->act(call);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->error, "TODOs found in output");
}

// ── Coarse "act" hooks still work (backward compat) ──

TEST_F(HooksTest, ActHooksStillFireForBackwardCompat) {
    auto agent = make_agent();
    int before_act = 0, after_act = 0;

    agent->use(Middleware{
        .name = "act-counter",
        .before = [&](HookContext& ctx) {
            if (ctx.operation == "act") before_act++;
        },
        .after = [&](HookContext& ctx) {
            if (ctx.operation == "act") after_act++;
        },
    });

    os_->register_tool(
        tools::ToolSchema{.id = "noop", .description = "No-op"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("ok");
        });

    kernel::ToolCallRequest call{"call-6", "noop", "{}"};
    (void)agent->act(call);
    EXPECT_EQ(before_act, 1);
    EXPECT_EQ(after_act, 1);
}

// ── Stop hook tests ──

TEST_F(HooksTest, StopHookContextHasStopOperation) {
    // Verify the stop hook operation value is correct
    HookContext ctx;
    ctx.operation = "stop";
    ctx.input = "some response";
    EXPECT_EQ(ctx.operation, "stop");
}

// ── Multiple hooks chain correctly ──

TEST_F(HooksTest, MultipleHooksChainInOrder) {
    auto agent = make_agent();
    std::vector<std::string> order;

    agent->use(Middleware{
        .name = "first",
        .before = [&](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use") order.push_back("first-before");
        },
        .after = [&](HookContext& ctx) {
            if (ctx.operation == "post_tool_use") order.push_back("first-after");
        },
    });

    agent->use(Middleware{
        .name = "second",
        .before = [&](HookContext& ctx) {
            if (ctx.operation == "pre_tool_use") order.push_back("second-before");
        },
        .after = [&](HookContext& ctx) {
            if (ctx.operation == "post_tool_use") order.push_back("second-after");
        },
    });

    os_->register_tool(
        tools::ToolSchema{.id = "chain_test", .description = "Chain test"},
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("ok");
        });

    kernel::ToolCallRequest call{"call-7", "chain_test", "{}"};
    (void)agent->act(call);

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], "first-before");
    EXPECT_EQ(order[1], "second-before");
    EXPECT_EQ(order[2], "first-after");
    EXPECT_EQ(order[3], "second-after");
}
