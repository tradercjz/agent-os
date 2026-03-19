// ============================================================
// AgentOS MCP Server Demo
// 演示 Model Context Protocol JSON-RPC 适配器
// ============================================================
#include <agentos/mcp/mcp_server.hpp>
#include <iostream>

using namespace agentos;
using namespace agentos::mcp;

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
    // 创建工具注册表并注册工具
    tools::ToolRegistry registry;

    tools::ToolSchema calc_schema;
    calc_schema.id = "calculator";
    calc_schema.description = "Perform basic arithmetic";
    calc_schema.params.push_back({
        .name = "expression",
        .type = tools::ParamType::String,
        .description = "Math expression to evaluate",
        .required = true,
    });

    registry.register_fn(calc_schema,
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string expr = args.values.count("expression")
                ? args.values.at("expression") : "0";
            return tools::ToolResult::ok("Result: " + expr + " = 42");
        });

    tools::ToolSchema weather_schema;
    weather_schema.id = "get_weather";
    weather_schema.description = "Get current weather for a city";
    weather_schema.params.push_back({
        .name = "city",
        .type = tools::ParamType::String,
        .description = "City name",
        .required = true,
    });

    registry.register_fn(weather_schema,
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string city = args.values.count("city")
                ? args.values.at("city") : "Unknown";
            return tools::ToolResult::ok(city + ": 22°C, Sunny");
        });

    // 创建 MCP Server
    MCPServer server(registry, "agentos-mcp", "1.0.0");

    // ── 1. Initialize ──
    section("1. Initialize (握手)");
    info("发送: {\"method\": \"initialize\"}");

    auto resp = server.handle_json(R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    auto parsed = Json::parse(resp.to_json());
    info(("服务器: " + parsed["result"]["serverInfo"]["name"].get<std::string>()).c_str());
    info(("版本: " + parsed["result"]["serverInfo"]["version"].get<std::string>()).c_str());
    info(("协议: " + parsed["result"]["protocolVersion"].get<std::string>()).c_str());
    ok("握手成功");

    // ── 2. Ping ──
    section("2. Ping (心跳检测)");
    auto ping_resp = server.handle_json(R"({"jsonrpc":"2.0","method":"ping","id":2})");
    info(("响应: " + ping_resp.to_json()).c_str());
    ok("Pong!");

    // ── 3. Tools List ──
    section("3. Tools/List (列出可用工具)");
    info("发送: {\"method\": \"tools/list\"}");

    auto list_resp = server.handle_json(R"({"jsonrpc":"2.0","method":"tools/list","id":3})");
    auto list_parsed = Json::parse(list_resp.to_json());
    auto& tools_arr = list_parsed["result"]["tools"];
    info(("发现 " + std::to_string(tools_arr.size()) + " 个工具:").c_str());
    for (const auto& tool : tools_arr) {
        std::string name = tool["name"].get<std::string>();
        std::string desc = tool["description"].get<std::string>();
        info(("  - " + name + ": " + desc).c_str());
        // 显示参数
        if (tool.contains("inputSchema") && tool["inputSchema"].contains("properties")) {
            for (auto& [key, val] : tool["inputSchema"]["properties"].items()) {
                info(("      param: " + key + " ("
                      + val["type"].get<std::string>() + ")").c_str());
            }
        }
    }
    ok("工具列表获取成功");

    // ── 4. Tools Call ──
    section("4. Tools/Call (调用工具)");

    // 调用 calculator
    info("调用 calculator(expression=\"2+2\")");
    auto calc_resp = server.handle_json(R"({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": "calculator", "arguments": {"expression": "2+2"}},
        "id": 4
    })");
    auto calc_parsed = Json::parse(calc_resp.to_json());
    info(("结果: " + calc_parsed["result"]["content"][0]["text"].get<std::string>()).c_str());
    info(("isError: " + std::string(calc_parsed["result"]["isError"].get<bool>() ? "true" : "false")).c_str());
    ok("calculator 调用成功");

    // 调用 get_weather
    info("调用 get_weather(city=\"Beijing\")");
    auto weather_resp = server.handle_json(R"({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": "get_weather", "arguments": {"city": "Beijing"}},
        "id": 5
    })");
    auto weather_parsed = Json::parse(weather_resp.to_json());
    info(("结果: " + weather_parsed["result"]["content"][0]["text"].get<std::string>()).c_str());
    ok("get_weather 调用成功");

    // ── 5. 错误处理 ──
    section("5. 错误处理");

    // 未知方法
    auto err1 = server.handle_json(R"({"jsonrpc":"2.0","method":"unknown","id":6})");
    auto err1_parsed = Json::parse(err1.to_json());
    info(("未知方法 → code: " + std::to_string(err1_parsed["error"]["code"].get<int>())).c_str());
    info(("message: " + err1_parsed["error"]["message"].get<std::string>()).c_str());

    // 不存在的工具
    auto err2 = server.handle_json(R"({
        "jsonrpc":"2.0","method":"tools/call",
        "params":{"name":"nonexistent"},"id":7
    })");
    auto err2_parsed = Json::parse(err2.to_json());
    info(("不存在工具 → " + err2_parsed["error"]["message"].get<std::string>()).c_str());

    // 无效 JSON
    auto err3 = server.handle_json("not json{{{");
    auto err3_parsed = Json::parse(err3.to_json());
    info(("无效 JSON → code: " + std::to_string(err3_parsed["error"]["code"].get<int>())).c_str());

    ok("所有错误都正确返回 JSON-RPC error");

    // ── 6. 集成提示 ──
    section("6. 实际集成方式");
    info("MCPServer 是传输无关的 — 只处理消息，不管网络:");
    info("  HTTP:      POST /mcp → server.handle_json(body) → response");
    info("  WebSocket: on_message → server.handle_json(msg) → send");
    info("  stdio:     readline → server.handle_json(line) → println");
    info("  进程内:     直接调用 server.handle(req) → MCPResponse");
    info("");
    info(".mcp.json 可提交到 git，团队共享工具配置");
    ok("MCP 适配器演示完成");

    std::cout << "\n\033[1;32m🎉 MCP Server Demo 完成！\033[0m\n\n";
    return 0;
}
