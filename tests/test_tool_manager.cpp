#include <agentos/tools/tool_manager.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::tools;

class ToolManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // ...
  }
};

TEST_F(ToolManagerTest, ToolSchemaJSONReflection) {
  ToolSchema schema{.id = "my_custom_tool",
                    .description = "A custom tool for testing",
                    .params = {{.name = "age",
                                .type = ParamType::Integer,
                                .description = "Age of user",
                                .required = true,
                                .default_value = std::nullopt},
                               {.name = "height",
                                .type = ParamType::Float,
                                .description = "Height in meters",
                                .required = false,
                                .default_value = std::nullopt},
                               {.name = "name",
                                .type = ParamType::String,
                                .description = "Full name",
                                .required = true,
                                .default_value = std::nullopt}}};

  std::string json_str = schema.to_function_json();
  Json parsed = Json::parse(json_str);

  EXPECT_EQ(parsed["type"], "function");
  EXPECT_EQ(parsed["function"]["name"], "my_custom_tool");
  EXPECT_EQ(parsed["function"]["description"], "A custom tool for testing");

  auto props = parsed["function"]["parameters"]["properties"];
  EXPECT_EQ(props["age"]["type"], "integer");
  EXPECT_EQ(props["height"]["type"], "number");
  EXPECT_EQ(props["name"]["type"], "string");

  auto req = parsed["function"]["parameters"]["required"];
  EXPECT_EQ(req.size(), 2u);
  EXPECT_EQ(req[0], "age");
  EXPECT_EQ(req[1], "name");
}

TEST_F(ToolManagerTest, ParamParsingResilience) {
  // Valid JSON
  std::string raw_json = R"({"name": "Alice", "age": 30, "height": 1.75})";
  ParsedArgs args = parse_args(raw_json);

  EXPECT_EQ(args.get("name"), "Alice");
  EXPECT_EQ(args.get("age"), "30"); // Converted to string
  EXPECT_EQ(args.get("height"), "1.75");

  // Missing key with default
  EXPECT_EQ(args.get("weight", "60"), "60");

  // Invalid JSON Should not crash, just empty args or partial
  std::string bad_json = R"({"name": "Bob", age: })";
  ParsedArgs bad_args = parse_args(bad_json);
  EXPECT_TRUE(
      bad_args.values
          .empty()); // Parson throws exception internally and returns empty
}

TEST_F(ToolManagerTest, ToolDispatchAndExecution) {
  ToolManager tm;

  tm.registry().register_fn(
      ToolSchema{.id = "math_adder",
                 .description = "Adds two numbers",
                 .params = {{.name = "a",
                             .type = ParamType::Integer,
                             .description = "First number",
                             .required = true,
                             .default_value = std::nullopt},
                            {.name = "b",
                             .type = ParamType::Integer,
                             .description = "Second number",
                             .required = true,
                             .default_value = std::nullopt}}},
      [](const ParsedArgs &args) -> ToolResult {
        int a = std::stoi(args.get("a", "0"));
        int b = std::stoi(args.get("b", "0"));
        return ToolResult::ok(std::to_string(a + b));
      });

  // Call existing tool
  kernel::ToolCallRequest req{"call_1", "math_adder", R"({"a": 5, "b": 10})"};
  auto res = tm.dispatch(req);
  EXPECT_TRUE(res.success);
  EXPECT_EQ(res.output, "15");

  // Call non-existent tool
  kernel::ToolCallRequest bad_req{"call_2", "non_existent", "{}"};
  auto bad_res = tm.dispatch(bad_req);
  EXPECT_FALSE(bad_res.success);
  EXPECT_NE(bad_res.error.find("not found"), std::string::npos);

  // Call with allowed list restriction
  std::unordered_set<std::string> allowed = {"kv_store"};
  auto restrict_res = tm.dispatch(req, allowed);
  EXPECT_FALSE(restrict_res.success);
  EXPECT_NE(restrict_res.error.find("not in allowed set"), std::string::npos);
}

// ── ShellTool Security Tests ──────────────────────────────

TEST(ShellToolTest, BlocksUnsafeCharacters) {
  ShellTool shell({"echo", "ls"});
  ParsedArgs args;
  args.values["cmd"] = "echo hello; rm -rf /";
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Unsafe characters"), std::string::npos);
}

TEST(ShellToolTest, BlocksNullBytes) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  std::string cmd_with_null = "echo hello";
  cmd_with_null += '\0';
  cmd_with_null += "rm -rf /";
  args.values["cmd"] = cmd_with_null;
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Null bytes"), std::string::npos);
}

TEST(ShellToolTest, BlocksCommandNotInAllowlist) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  args.values["cmd"] = "rm file.txt";
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("not in allowlist"), std::string::npos);
}

TEST(ShellToolTest, AllowsValidCommand) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  args.values["cmd"] = "echo hello world";
  auto result = shell.execute(args);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.output.find("hello world"), std::string::npos);
}

// ── Tool Schema Validation ──────────────────────────────────

TEST(ToolValidationTest, RejectsWhenMissingRequiredParam) {
  ToolSchema schema{
      .id = "test_tool",
      .description = "test",
      .params = {{.name = "required_field",
                  .type = ParamType::String,
                  .description = "must be present",
                  .required = true}},
  };
  ParsedArgs args; // empty
  auto result = validate_tool_args(schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(result.error().message.find("required_field") != std::string::npos);
}

TEST(ToolValidationTest, AcceptsWhenRequiredParamPresent) {
  ToolSchema schema{
      .id = "test_tool",
      .description = "test",
      .params = {{.name = "key",
                  .type = ParamType::String,
                  .description = "k",
                  .required = true}},
  };
  ParsedArgs args;
  args.values["key"] = "value";
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

TEST(ToolValidationTest, OptionalParamAllowedMissing) {
  ToolSchema schema{
      .id = "test_tool",
      .description = "test",
      .params = {{.name = "opt",
                  .type = ParamType::String,
                  .description = "optional",
                  .required = false}},
  };
  ParsedArgs args; // empty — should be fine
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

TEST(ToolValidationTest, RequiredWithDefaultAllowedMissing) {
  ToolSchema schema{
      .id = "test_tool",
      .description = "test",
      .params = {{.name = "with_default",
                  .type = ParamType::String,
                  .description = "has default",
                  .required = true,
                  .default_value = "fallback"}},
  };
  ParsedArgs args;
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

// ── Tool Timeout ────────────────────────────────────────────

TEST_F(ToolManagerTest, ToolTimeoutKillsSlowTool) {
  ToolManager tm;
  // Register a tool with 500ms timeout that sleeps 2s
  tm.registry().register_fn(
      ToolSchema{
          .id = "slow_tool",
          .description = "A slow tool",
          .params = {},
          .timeout_ms = 500,
      },
      [](const ParsedArgs &) -> ToolResult {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return ToolResult::ok("done");
      });

  kernel::ToolCallRequest req{"call_slow", "slow_tool", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error.find("timed out") != std::string::npos);
}

TEST_F(ToolManagerTest, FastToolCompletesWithinTimeout) {
  ToolManager tm;
  tm.registry().register_fn(
      ToolSchema{
          .id = "fast_tool",
          .description = "A fast tool",
          .params = {},
          .timeout_ms = 5000,
      },
      [](const ParsedArgs &) -> ToolResult {
        return ToolResult::ok("fast");
      });

  kernel::ToolCallRequest req{"call_fast", "fast_tool", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "fast");
}

TEST_F(ToolManagerTest, DispatchValidatesRequiredParams) {
  ToolManager tm;
  tm.registry().register_fn(
      ToolSchema{
          .id = "param_tool",
          .description = "needs params",
          .params = {{.name = "required_key",
                      .type = ParamType::String,
                      .description = "required",
                      .required = true}},
      },
      [](const ParsedArgs &args) -> ToolResult {
        return ToolResult::ok(args.get("required_key"));
      });

  // Missing required param
  kernel::ToolCallRequest req{"call_1", "param_tool", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.error.find("required_key") != std::string::npos);

  // With required param
  kernel::ToolCallRequest req2{"call_2", "param_tool", R"({"required_key":"hello"})"};
  auto result2 = tm.dispatch(req2);
  EXPECT_TRUE(result2.success);
  EXPECT_EQ(result2.output, "hello");
}
