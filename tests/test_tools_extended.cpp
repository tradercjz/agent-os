#include <agentos/tools/tool_manager.hpp>
#include <agentos/tools/tool_pipeline.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::tools;

// ── Helpers ─────────────────────────────────────────────────

static ToolSchema make_echo_schema(const std::string& id = "echo_tool",
                                   uint32_t timeout_ms = 30000) {
  return ToolSchema{
      .id = id,
      .description = "Echoes the input value",
      .params = {{.name = "input",
                  .type = ParamType::String,
                  .description = "Value to echo",
                  .required = true,
                  .default_value = std::nullopt}},
      .timeout_ms = timeout_ms,
  };
}

static ToolSchema make_adder_schema(const std::string& id = "adder") {
  return ToolSchema{
      .id = id,
      .description = "Adds two integers",
      .params = {{.name = "a",
                  .type = ParamType::Integer,
                  .description = "First number",
                  .required = true,
                  .default_value = std::nullopt},
                 {.name = "b",
                  .type = ParamType::Integer,
                  .description = "Second number",
                  .required = true,
                  .default_value = std::nullopt}},
  };
}

// ── Fixture ─────────────────────────────────────────────────

class ToolsExtendedTest : public ::testing::Test {
protected:
  ToolManager tm;
};

// ── 1. Register tool with schema ────────────────────────────

TEST_F(ToolsExtendedTest, RegisterToolWithSchema) {
  tm.registry().register_fn(
      make_echo_schema("my_echo"),
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("input"));
      });

  EXPECT_TRUE(tm.registry().has("my_echo"));
  auto tool = tm.registry().find("my_echo");
  ASSERT_NE(tool, nullptr);
  EXPECT_EQ(tool->schema().id, "my_echo");
  EXPECT_EQ(tool->schema().params.size(), 1u);
}

// ── 2. Tool dispatch with valid arguments ───────────────────

TEST_F(ToolsExtendedTest, DispatchWithValidArguments) {
  tm.registry().register_fn(
      make_adder_schema(),
      [](const ParsedArgs& args) -> ToolResult {
        int a = std::stoi(args.get("a", "0"));
        int b = std::stoi(args.get("b", "0"));
        return ToolResult::ok(std::to_string(a + b));
      });

  kernel::ToolCallRequest req{"call_1", "adder", R"({"a": 7, "b": 3})"};
  auto result = tm.dispatch(req);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "10");
}

// ── 3. Tool dispatch with invalid arguments returns error ───

TEST_F(ToolsExtendedTest, DispatchWithInvalidArgumentsReturnsError) {
  tm.registry().register_fn(
      make_adder_schema(),
      [](const ParsedArgs& /*args*/) -> ToolResult {
        return ToolResult::ok("should not reach");
      });

  // Missing required parameter "a" and "b"
  kernel::ToolCallRequest req{"call_2", "adder", R"({})"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  // Error should mention the missing required parameter
  EXPECT_NE(result.error.find("Missing required"), std::string::npos);
}

// ── 4. Unregister tool ──────────────────────────────────────

TEST_F(ToolsExtendedTest, UnregisterTool) {
  tm.registry().register_fn(
      make_echo_schema("removable"),
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("input"));
      });

  EXPECT_TRUE(tm.registry().has("removable"));
  tm.registry().unregister("removable");
  EXPECT_FALSE(tm.registry().has("removable"));
  EXPECT_EQ(tm.registry().find("removable"), nullptr);

  // Dispatching to unregistered tool should fail
  kernel::ToolCallRequest req{"call_3", "removable", R"({"input":"hi"})"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("not found"), std::string::npos);
}

// ── 5. List registered tools ────────────────────────────────

TEST_F(ToolsExtendedTest, ListRegisteredTools) {
  // ToolManager registers built-in tools (kv_store, shell_exec, http_fetch)
  auto schemas = tm.registry().list_schemas();
  EXPECT_GE(schemas.size(), 3u); // at least the 3 built-ins

  // Register a custom tool
  tm.registry().register_fn(
      make_echo_schema("list_test"),
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("input"));
      });

  auto schemas2 = tm.registry().list_schemas();
  EXPECT_EQ(schemas2.size(), schemas.size() + 1);

  // Verify our tool is in the list
  bool found = false;
  for (const auto& s : schemas2) {
    if (s.id == "list_test") { found = true; break; }
  }
  EXPECT_TRUE(found);
}

// ── 6. Tool not found returns error ─────────────────────────

TEST_F(ToolsExtendedTest, ToolNotFoundReturnsError) {
  kernel::ToolCallRequest req{"call_4", "nonexistent_tool_xyz", R"({})"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("not found"), std::string::npos);
}

// ── 7. Tool with async execution ────────────────────────────

TEST_F(ToolsExtendedTest, ToolWithAsyncExecution) {
  tm.registry().register_fn(
      make_echo_schema("async_echo"),
      [](const ParsedArgs& args) -> ToolResult {
        // Simulate async work
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return ToolResult::ok("async_" + args.get("input"));
      });

  // Run dispatch in a future to test async behavior
  auto future = std::async(std::launch::async, [&] {
    kernel::ToolCallRequest req{"call_5", "async_echo", R"({"input":"hello"})"};
    return tm.dispatch(req);
  });

  auto result = future.get();
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "async_hello");
}

// ── 8. Tool pipeline: chain two tools ───────────────────────

TEST_F(ToolsExtendedTest, PipelineChainTwoTools) {
  // Tool 1: uppercaser (simulated by appending "_UPPER")
  tm.registry().register_fn(
      ToolSchema{
          .id = "uppercaser",
          .description = "Uppercases input",
          .params = {{.name = "text",
                      .type = ParamType::String,
                      .description = "Text to uppercase",
                      .required = true,
                      .default_value = std::nullopt}},
      },
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("text") + "_UPPER");
      });

  // Tool 2: prefixer
  tm.registry().register_fn(
      ToolSchema{
          .id = "prefixer",
          .description = "Prefixes input",
          .params = {{.name = "text",
                      .type = ParamType::String,
                      .description = "Text to prefix",
                      .required = true,
                      .default_value = std::nullopt}},
      },
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok("PREFIX_" + args.get("text"));
      });

  ToolPipeline pipeline(tm);
  pipeline.then("uppercaser", [](const std::string& prev) {
    nlohmann::json j;
    j["text"] = prev;
    return j.dump();
  });
  pipeline.then("prefixer", [](const std::string& prev) {
    nlohmann::json j;
    j["text"] = prev;
    return j.dump();
  });

  auto result = pipeline.execute("hello");
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.final_output, "PREFIX_hello_UPPER");
  EXPECT_EQ(result.steps_executed, 2u);
  EXPECT_EQ(result.step_results.size(), 2u);
}

// ── 9. Tool pipeline: error in first tool stops pipeline ────

TEST_F(ToolsExtendedTest, PipelineErrorInFirstToolStops) {
  // Tool that always fails
  tm.registry().register_fn(
      ToolSchema{
          .id = "failing_tool",
          .description = "Always fails",
          .params = {{.name = "input",
                      .type = ParamType::String,
                      .description = "input",
                      .required = true,
                      .default_value = std::nullopt}},
      },
      [](const ParsedArgs&) -> ToolResult {
        return ToolResult::fail("intentional failure");
      });

  // Tool that should never run
  tm.registry().register_fn(
      make_echo_schema("never_runs"),
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("input"));
      });

  ToolPipeline pipeline(tm);
  pipeline.then("failing_tool", [](const std::string& prev) {
    nlohmann::json j;
    j["input"] = prev;
    return j.dump();
  });
  pipeline.then("never_runs", [](const std::string& prev) {
    nlohmann::json j;
    j["input"] = prev;
    return j.dump();
  });

  auto result = pipeline.execute("test");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.steps_executed, 1u); // only the failing step ran
  EXPECT_NE(result.error.find("failed"), std::string::npos);
}

// ── 10. Concurrent tool dispatch ────────────────────────────

TEST_F(ToolsExtendedTest, ConcurrentToolDispatch) {
  std::atomic<int> call_count{0};
  tm.registry().register_fn(
      make_echo_schema("concurrent_echo"),
      [&](const ParsedArgs& args) -> ToolResult {
        call_count++;
        return ToolResult::ok(args.get("input"));
      });

  constexpr int kNumThreads = 8;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, idx = i] {
      kernel::ToolCallRequest req{
          "call_" + std::to_string(idx),
          "concurrent_echo",
          R"({"input":"concurrent"})"};
      auto result = tm.dispatch(req);
      if (result.success) success_count++;
    });
  }

  for (auto& th : threads) th.join();

  EXPECT_EQ(success_count.load(), kNumThreads);
  EXPECT_EQ(call_count.load(), kNumThreads);
}

// ── 11. Tool with JSON schema validation ────────────────────

TEST_F(ToolsExtendedTest, JSONSchemaValidation) {
  ToolSchema schema{
      .id = "validated_tool",
      .description = "Tool with multiple required params",
      .params = {{.name = "name",
                  .type = ParamType::String,
                  .description = "User name",
                  .required = true,
                  .default_value = std::nullopt},
                 {.name = "age",
                  .type = ParamType::Integer,
                  .description = "User age",
                  .required = true,
                  .default_value = std::nullopt},
                 {.name = "email",
                  .type = ParamType::String,
                  .description = "User email",
                  .required = false,
                  .default_value = std::nullopt}},
  };

  tm.registry().register_fn(
      schema,
      [](const ParsedArgs& args) -> ToolResult {
        return ToolResult::ok(args.get("name") + ":" + args.get("age"));
      });

  // Valid call with all required params
  {
    kernel::ToolCallRequest req{"v1", "validated_tool",
                                R"({"name":"Alice","age":30})"};
    auto result = tm.dispatch(req);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.output, "Alice:30");
  }

  // Missing "age" required param
  {
    kernel::ToolCallRequest req{"v2", "validated_tool",
                                R"({"name":"Bob"})"};
    auto result = tm.dispatch(req);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("age"), std::string::npos);
  }

  // Missing "name" required param
  {
    kernel::ToolCallRequest req{"v3", "validated_tool",
                                R"({"age":25})"};
    auto result = tm.dispatch(req);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("name"), std::string::npos);
  }

  // Optional param missing is fine
  {
    kernel::ToolCallRequest req{"v4", "validated_tool",
                                R"({"name":"Charlie","age":20})"};
    auto result = tm.dispatch(req);
    EXPECT_TRUE(result.success);
  }

  // Verify JSON schema output format
  auto json_str = schema.to_function_json();
  auto parsed = nlohmann::json::parse(json_str);
  EXPECT_EQ(parsed["type"], "function");
  EXPECT_EQ(parsed["function"]["name"], "validated_tool");
  auto req_arr = parsed["function"]["parameters"]["required"];
  EXPECT_EQ(req_arr.size(), 2u); // name and age
}

// ── 12. Tool timeout enforcement ────────────────────────────

TEST_F(ToolsExtendedTest, ToolTimeoutEnforcement) {
  // Register a slow tool with a short timeout
  tm.registry().register_fn(
      ToolSchema{
          .id = "slow_tool_ext",
          .description = "Sleeps too long",
          .params = {},
          .timeout_ms = 200, // 200ms timeout
      },
      [](const ParsedArgs&) -> ToolResult {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return ToolResult::ok("should not reach");
      });

  kernel::ToolCallRequest req{"t1", "slow_tool_ext", "{}"};
  auto start = std::chrono::steady_clock::now();
  auto result = tm.dispatch(req);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("timed out"), std::string::npos);
  // Should return well before the 5s sleep
  EXPECT_LT(elapsed.count(), 3000);

  // Verify per-tool timeout override via registry
  tm.registry().set_tool_timeout("slow_tool_ext", std::chrono::seconds(100));
  EXPECT_EQ(tm.registry().get_tool_timeout("slow_tool_ext").count(), 100);
}
