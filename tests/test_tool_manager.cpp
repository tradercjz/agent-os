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
  EXPECT_EQ(req.size(), 2);
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
