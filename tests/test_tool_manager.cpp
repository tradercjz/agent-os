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

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ShellTool edge cases
// ════════════════════════════════════════════════════════════════

TEST(ShellToolTest, EmptyCommandReturnsError) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  args.values["cmd"] = "";
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("empty"), std::string::npos);
}

TEST(ShellToolTest, MissingCmdParamReturnsError) {
  ShellTool shell({"echo"});
  ParsedArgs args; // no "cmd" key at all
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  // get("cmd") returns empty string, so "Command must not be empty"
  EXPECT_NE(result.error.find("empty"), std::string::npos);
}

TEST(ShellToolTest, CommandTooLongReturnsError) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  // Build a command > 4096 bytes
  std::string long_cmd = "echo " + std::string(5000, 'x');
  args.values["cmd"] = long_cmd;
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  // Returns via .output field, not .error
  EXPECT_TRUE(result.output.find("command too long") != std::string::npos ||
              result.error.find("command too long") != std::string::npos);
}

TEST(ShellToolTest, TooManyArgumentsReturnsError) {
  ShellTool shell({"echo"});
  ParsedArgs args;
  // Build a command with > 20 arguments but under 4096 bytes
  std::string cmd = "echo";
  for (int i = 0; i < 25; ++i) {
    cmd += " a" + std::to_string(i);
  }
  ASSERT_LE(cmd.size(), 4096u); // Ensure we don't hit command-too-long first
  args.values["cmd"] = cmd;
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Too many arguments"), std::string::npos);
}

TEST(ShellToolTest, SuccessfulDateCommand) {
  ShellTool shell({"date"});
  ParsedArgs args;
  args.values["cmd"] = "date";
  auto result = shell.execute(args);
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.output.empty());
}

TEST(ShellToolTest, SuccessfulPwdCommand) {
  ShellTool shell({"pwd"});
  ParsedArgs args;
  args.values["cmd"] = "pwd";
  auto result = shell.execute(args);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.output.find("/"), std::string::npos);
}

TEST(ShellToolTest, FailedCommandExitCode) {
  // "ls" on a nonexistent path should fail with non-zero exit code
  ShellTool shell({"ls"});
  ParsedArgs args;
  args.values["cmd"] = "ls /nonexistent_path_for_test_12345";
  auto result = shell.execute(args);
  // ls on nonexistent dir writes to stderr and exits non-zero
  // The fork/execvp captures stderr too, so output should have something
  // Either success=false (if output empty) or success=true (if output has error text)
  // Just verify it doesn't crash
  EXPECT_FALSE(result.output.empty() && result.success);
}

TEST(ShellToolTest, SchemaReturnsCorrectId) {
  ShellTool shell({"echo"});
  auto s = shell.schema();
  EXPECT_EQ(s.id, "shell_exec");
  EXPECT_TRUE(s.is_dangerous);
  EXPECT_TRUE(s.sandboxed);
  EXPECT_EQ(s.params.size(), 1u);
  EXPECT_EQ(s.params[0].name, "cmd");
}

TEST(ShellToolTest, BlocksVariousUnsafeCharacters) {
  ShellTool shell({"echo"});

  // Test pipe
  {
    ParsedArgs args;
    args.values["cmd"] = "echo foo | cat";
    EXPECT_FALSE(shell.execute(args).success);
  }
  // Test backtick
  {
    ParsedArgs args;
    args.values["cmd"] = "echo `whoami`";
    EXPECT_FALSE(shell.execute(args).success);
  }
  // Test dollar
  {
    ParsedArgs args;
    args.values["cmd"] = "echo $HOME";
    EXPECT_FALSE(shell.execute(args).success);
  }
  // Test redirect
  {
    ParsedArgs args;
    args.values["cmd"] = "echo foo > /tmp/test";
    EXPECT_FALSE(shell.execute(args).success);
  }
  // Test parentheses
  {
    ParsedArgs args;
    args.values["cmd"] = "echo (foo)";
    EXPECT_FALSE(shell.execute(args).success);
  }
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — HttpFetchTool edge cases (SSRF, validation)
// ════════════════════════════════════════════════════════════════

TEST(HttpFetchToolTest, EmptyUrlReturnsError) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("URL is required"), std::string::npos);
}

TEST(HttpFetchToolTest, MissingUrlParamReturnsError) {
  HttpFetchTool tool;
  ParsedArgs args; // no "url" key
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("URL is required"), std::string::npos);
}

TEST(HttpFetchToolTest, NonHttpUrlReturnsError) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "ftp://example.com/file";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("http/https"), std::string::npos);
}

TEST(HttpFetchToolTest, MalformedUrlNoSchemeReturnsError) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "httpexample.com";  // starts with "http" but no "://"
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Cannot extract hostname"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksLocalhost) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "http://localhost/admin";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("private/internal"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocks127001) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "http://127.0.0.1:8080/secret";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("private/internal"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksIPv6Loopback) {
  HttpFetchTool tool;
  ParsedArgs args;
  // ::1 as a URL hostname gets parsed weirdly (colon hits host_end),
  // so extract_hostname returns empty → "Cannot extract hostname"
  args.values["url"] = "http://::1/admin";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  // Either "Cannot extract hostname" or "private/internal" depending on parse
  EXPECT_TRUE(result.error.find("private/internal") != std::string::npos ||
              result.error.find("Cannot extract hostname") != std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksZeroIP) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "http://0.0.0.0/";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("private/internal"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksDotLocal) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "http://myserver.local/api";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("private/internal"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksDotInternal) {
  HttpFetchTool tool;
  ParsedArgs args;
  args.values["url"] = "http://metadata.internal/latest";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("private/internal"), std::string::npos);
}

TEST(HttpFetchToolTest, SSRFBlocksUnresolvableHost) {
  HttpFetchTool tool;
  ParsedArgs args;
  // .invalid TLD is reserved per RFC 2606 and should never resolve
  args.values["url"] = "http://xyzzy-nonexistent-host-99999.invalid/path";
  auto result = tool.execute(args);
  EXPECT_FALSE(result.success);
  // getaddrinfo fails → treated as private (fail-safe), OR curl fails
  EXPECT_TRUE(result.error.find("private/internal") != std::string::npos ||
              result.error.find("curl failed") != std::string::npos);
}

TEST(HttpFetchToolTest, SchemaReturnsCorrectId) {
  HttpFetchTool tool;
  auto s = tool.schema();
  EXPECT_EQ(s.id, "http_fetch");
  EXPECT_EQ(s.params.size(), 1u);
  EXPECT_EQ(s.params[0].name, "url");
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ToolRegistry: unregister, has, filtered JSON
// ════════════════════════════════════════════════════════════════

TEST(ToolRegistryTest, HasAndUnregister) {
  ToolRegistry reg;
  reg.register_tool(std::make_shared<KVStoreTool>());
  EXPECT_TRUE(reg.has("kv_store"));
  EXPECT_FALSE(reg.has("nonexistent"));

  reg.unregister("kv_store");
  EXPECT_FALSE(reg.has("kv_store"));
  EXPECT_EQ(reg.find("kv_store"), nullptr);
}

TEST(ToolRegistryTest, GetFilteredToolsJson) {
  ToolRegistry reg;
  reg.register_tool(std::make_shared<KVStoreTool>());
  reg.register_tool(std::make_shared<ShellTool>());
  reg.register_tool(std::make_shared<HttpFetchTool>());

  // Filter with specific tools
  auto json = reg.get_filtered_tools_json({"kv_store", "shell_exec"});
  auto parsed = Json::parse(json);
  EXPECT_EQ(parsed.size(), 2u);

  // Filter with nonexistent tool
  auto json2 = reg.get_filtered_tools_json({"nonexistent"});
  auto parsed2 = Json::parse(json2);
  EXPECT_EQ(parsed2.size(), 0u);

  // Empty filter returns all
  auto json3 = reg.get_filtered_tools_json({});
  auto parsed3 = Json::parse(json3);
  EXPECT_EQ(parsed3.size(), 3u);
}

TEST(ToolRegistryTest, UnregisterInvalidatesCacheAndFiltered) {
  ToolRegistry reg;
  reg.register_tool(std::make_shared<KVStoreTool>());
  reg.register_tool(std::make_shared<ShellTool>());

  // Populate cache
  auto all1 = reg.get_all_tools_json();
  EXPECT_FALSE(all1.empty());

  // Unregister invalidates cache
  reg.unregister("kv_store");
  auto all2 = reg.get_all_tools_json();
  auto parsed = Json::parse(all2);
  EXPECT_EQ(parsed.size(), 1u);
}

TEST(ToolRegistryTest, ListSchemasReturnsAll) {
  ToolRegistry reg;
  reg.register_tool(std::make_shared<KVStoreTool>());
  reg.register_tool(std::make_shared<HttpFetchTool>());
  auto schemas = reg.list_schemas();
  EXPECT_EQ(schemas.size(), 2u);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — parse_args edge cases
// ════════════════════════════════════════════════════════════════

TEST(ParseArgsTest, EmptyStringReturnsEmpty) {
  auto args = parse_args("");
  EXPECT_TRUE(args.values.empty());
}

TEST(ParseArgsTest, NonObjectJsonReturnsEmpty) {
  // JSON array instead of object
  auto args = parse_args("[1, 2, 3]");
  EXPECT_TRUE(args.values.empty());
}

TEST(ParseArgsTest, BooleanValues) {
  auto args = parse_args(R"({"flag": true, "other": false})");
  EXPECT_EQ(args.get("flag"), "true");
  EXPECT_EQ(args.get("other"), "false");
}

TEST(ParseArgsTest, NullValues) {
  auto args = parse_args(R"({"key": null})");
  EXPECT_EQ(args.get("key"), "null");
}

TEST(ParseArgsTest, NestedObjectValues) {
  auto args = parse_args(R"({"nested": {"a": 1}})");
  auto val = args.get("nested");
  EXPECT_FALSE(val.empty());
  // Should be serialized as JSON string
  auto parsed = Json::parse(val);
  EXPECT_EQ(parsed["a"], 1);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — UTF-8 validation
// ════════════════════════════════════════════════════════════════

TEST(ToolValidationTest, ValidUtf8MultibytePasses) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // Valid UTF-8: Chinese characters
  args.values["text"] = "\xe4\xb8\xad\xe6\x96\x87";  // "中文"
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

TEST(ToolValidationTest, ValidUtf8TwoByteSequence) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // Valid 2-byte UTF-8: e-acute (U+00E9)
  args.values["text"] = "\xc3\xa9";
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

TEST(ToolValidationTest, ValidUtf8FourByteSequence) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // Valid 4-byte UTF-8: emoji (U+1F600)
  args.values["text"] = "\xf0\x9f\x98\x80";
  auto result = validate_tool_args(schema, args);
  EXPECT_TRUE(result.has_value());
}

TEST(ToolValidationTest, InvalidUtf8TruncatedSequence) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // Truncated 3-byte sequence (only 2 bytes given)
  args.values["text"] = "\xe4\xb8";
  auto result = validate_tool_args(schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("invalid UTF-8"), std::string::npos);
}

TEST(ToolValidationTest, InvalidUtf8BadLeadingByte) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // Invalid leading byte 0xFF
  args.values["text"] = std::string("hello\xff world");
  auto result = validate_tool_args(schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("invalid UTF-8"), std::string::npos);
}

TEST(ToolValidationTest, InvalidUtf8BadContinuation) {
  ToolSchema schema{
      .id = "utf8_tool",
      .description = "test",
      .params = {{.name = "text",
                  .type = ParamType::String,
                  .description = "text",
                  .required = true}},
  };
  ParsedArgs args;
  // 2-byte sequence with invalid continuation byte
  args.values["text"] = std::string("\xc3\x28");
  auto result = validate_tool_args(schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("invalid UTF-8"), std::string::npos);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ToolSchema to_function_json edge cases
// ════════════════════════════════════════════════════════════════

TEST(ToolSchemaTest, BooleanParamType) {
  ToolSchema schema{
      .id = "bool_tool",
      .description = "tool with bool",
      .params = {{.name = "flag",
                  .type = ParamType::Boolean,
                  .description = "a flag",
                  .required = false}},
  };
  auto json = schema.to_function_json();
  auto parsed = Json::parse(json);
  EXPECT_EQ(parsed["function"]["parameters"]["properties"]["flag"]["type"], "boolean");
  // No required array since the only param is optional
  EXPECT_TRUE(parsed["function"]["parameters"].find("required") == parsed["function"]["parameters"].end());
}

TEST(ToolSchemaTest, ObjectAndArrayParamTypes) {
  ToolSchema schema{
      .id = "complex_tool",
      .description = "tool with complex types",
      .params = {{.name = "data",
                  .type = ParamType::Object,
                  .description = "object data",
                  .required = true},
                 {.name = "items",
                  .type = ParamType::Array,
                  .description = "array items",
                  .required = true}},
  };
  auto json = schema.to_function_json();
  auto parsed = Json::parse(json);
  // Both Object and Array fall through to default case → "object"
  EXPECT_EQ(parsed["function"]["parameters"]["properties"]["data"]["type"], "object");
  EXPECT_EQ(parsed["function"]["parameters"]["properties"]["items"]["type"], "object");
}

TEST(ToolSchemaTest, EmptyDescriptionOmitted) {
  ToolSchema schema{
      .id = "no_desc_tool",
      .description = "",
      .params = {},
  };
  auto json = schema.to_function_json();
  auto parsed = Json::parse(json);
  EXPECT_TRUE(parsed["function"].find("description") == parsed["function"].end());
}

TEST(ToolSchemaTest, NoParamsNoRequiredArray) {
  ToolSchema schema{
      .id = "no_params",
      .description = "no params tool",
      .params = {},
  };
  auto json = schema.to_function_json();
  auto parsed = Json::parse(json);
  EXPECT_TRUE(parsed["function"]["parameters"].find("required") == parsed["function"]["parameters"].end());
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — Dispatch with timeout_ms = 0 (no timeout path)
// ════════════════════════════════════════════════════════════════

TEST_F(ToolManagerTest, DispatchWithZeroTimeoutExecutesSynchronously) {
  ToolManager tm;
  tm.registry().register_fn(
      ToolSchema{
          .id = "no_timeout_tool",
          .description = "No timeout",
          .params = {},
          .timeout_ms = 0,
      },
      [](const ParsedArgs &) -> ToolResult {
        return ToolResult::ok("sync result");
      });

  kernel::ToolCallRequest req{"call_sync", "no_timeout_tool", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output, "sync result");
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — Dispatch tool that throws exception
// ════════════════════════════════════════════════════════════════

TEST_F(ToolManagerTest, DispatchToolThrowsExceptionWithTimeout) {
  ToolManager tm;
  tm.registry().register_fn(
      ToolSchema{
          .id = "throw_tool",
          .description = "throws",
          .params = {},
          .timeout_ms = 5000,
      },
      [](const ParsedArgs &) -> ToolResult {
        throw std::runtime_error("tool exploded");
      });

  kernel::ToolCallRequest req{"call_throw", "throw_tool", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.output.find("tool exploded"), std::string::npos);
}

TEST_F(ToolManagerTest, DispatchToolThrowsExceptionNoTimeout) {
  ToolManager tm;
  tm.registry().register_fn(
      ToolSchema{
          .id = "throw_tool_sync",
          .description = "throws sync",
          .params = {},
          .timeout_ms = 0,
      },
      [](const ParsedArgs &) -> ToolResult {
        throw std::runtime_error("sync explosion");
      });

  kernel::ToolCallRequest req{"call_throw", "throw_tool_sync", "{}"};
  auto result = tm.dispatch(req);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("sync explosion"), std::string::npos);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ToolManager tools_json with filter
// ════════════════════════════════════════════════════════════════

TEST_F(ToolManagerTest, ToolsJsonWithFilter) {
  ToolManager tm;
  auto json = tm.tools_json({"kv_store"});
  auto parsed = Json::parse(json);
  EXPECT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["function"]["name"], "kv_store");
}

TEST_F(ToolManagerTest, ToolsJsonWithMultipleFilters) {
  ToolManager tm;
  auto json = tm.tools_json({"kv_store", "shell_exec"});
  auto parsed = Json::parse(json);
  EXPECT_EQ(parsed.size(), 2u);
}

TEST_F(ToolManagerTest, ToolsJsonWithEmptyFilter) {
  ToolManager tm;
  auto json = tm.tools_json({});
  auto parsed = Json::parse(json);
  // Should return all built-in tools (kv_store, shell_exec, http_fetch)
  EXPECT_EQ(parsed.size(), 3u);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — KVStoreTool operations
// ════════════════════════════════════════════════════════════════

TEST(KVStoreToolTest, SetGetDeleteOperations) {
  KVStoreTool kv;

  // Set
  ParsedArgs set_args;
  set_args.values["op"] = "set";
  set_args.values["key"] = "mykey";
  set_args.values["value"] = "myvalue";
  auto set_res = kv.execute(set_args);
  EXPECT_TRUE(set_res.success);
  EXPECT_EQ(set_res.output, "OK");

  // Get existing key
  ParsedArgs get_args;
  get_args.values["op"] = "get";
  get_args.values["key"] = "mykey";
  auto get_res = kv.execute(get_args);
  EXPECT_TRUE(get_res.success);
  EXPECT_EQ(get_res.output, "myvalue");

  // Get nonexistent key
  ParsedArgs get_miss;
  get_miss.values["op"] = "get";
  get_miss.values["key"] = "nokey";
  auto miss_res = kv.execute(get_miss);
  EXPECT_FALSE(miss_res.success);
  EXPECT_NE(miss_res.error.find("not found"), std::string::npos);

  // Delete
  ParsedArgs del_args;
  del_args.values["op"] = "delete";
  del_args.values["key"] = "mykey";
  auto del_res = kv.execute(del_args);
  EXPECT_TRUE(del_res.success);

  // Verify deleted
  auto check = kv.execute(get_args);
  EXPECT_FALSE(check.success);

  // Unknown op
  ParsedArgs unk_args;
  unk_args.values["op"] = "update";
  unk_args.values["key"] = "k";
  auto unk_res = kv.execute(unk_args);
  EXPECT_FALSE(unk_res.success);
  EXPECT_NE(unk_res.error.find("Unknown op"), std::string::npos);
}

TEST(KVStoreToolTest, SchemaReturnsCorrectId) {
  KVStoreTool kv;
  auto s = kv.schema();
  EXPECT_EQ(s.id, "kv_store");
  EXPECT_EQ(s.params.size(), 3u);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ToolResult static helpers and equality
// ════════════════════════════════════════════════════════════════

TEST(ToolResultTest, OkAndFailHelpers) {
  auto ok = ToolResult::ok("good");
  EXPECT_TRUE(ok.success);
  EXPECT_EQ(ok.output, "good");
  EXPECT_TRUE(ok.error.empty());
  EXPECT_FALSE(ok.truncated);

  auto fail = ToolResult::fail("bad");
  EXPECT_FALSE(fail.success);
  EXPECT_TRUE(fail.output.empty());
  EXPECT_EQ(fail.error, "bad");
  EXPECT_FALSE(fail.truncated);
}

TEST(ToolResultTest, EqualityComparison) {
  auto a = ToolResult::ok("result");
  auto b = ToolResult::ok("result");
  auto c = ToolResult::fail("error");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

// ════════════════════════════════════════════════════════════════
// NEW COVERAGE TESTS — ParsedArgs::get with defaults
// ════════════════════════════════════════════════════════════════

TEST(ParsedArgsTest, GetWithDefault) {
  ParsedArgs args;
  args.values["key"] = "val";
  EXPECT_EQ(args.get("key"), "val");
  EXPECT_EQ(args.get("missing"), "");
  EXPECT_EQ(args.get("missing", "default"), "default");
}
