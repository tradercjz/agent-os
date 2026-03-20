#pragma once
// ============================================================
// AgentOS :: MCP Server
// Model Context Protocol JSON-RPC 适配器
// 将 ToolRegistry 暴露为标准 MCP 接口
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <string>

namespace agentos::mcp {

// ── MCP JSON-RPC message types ──

struct MCPRequest {
    std::string jsonrpc{"2.0"};
    std::string method;
    Json params;
    Json id;  // string or int
};

struct MCPResponse {
    std::string jsonrpc{"2.0"};
    Json result;
    Json error;   // null or {code, message, data}
    Json id;

    std::string to_json() const {
        Json j;
        j["jsonrpc"] = jsonrpc;
        j["id"] = id;
        if (!error.is_null()) {
            j["error"] = error;
        } else {
            j["result"] = result;
        }
        return j.dump();
    }
};

// ── MCP error codes (JSON-RPC standard + MCP extensions) ──

namespace error_code {
    inline constexpr int ParseError = -32700;
    inline constexpr int InvalidRequest = -32600;
    inline constexpr int MethodNotFound = -32601;
    inline constexpr int InvalidParams = -32602;
    inline constexpr int InternalError = -32603;
}

// ── MCP Server ──

class MCPServer {
public:
    MCPServer(tools::ToolManager& tool_manager,
              std::string server_name,
              std::string version)
        : tool_manager_(tool_manager),
          server_name_(std::move(server_name)),
          version_(std::move(version)) {}

    /// Handle a parsed MCP request
    MCPResponse handle(const MCPRequest& req) {
        if (req.method == "initialize") return handle_initialize(req);
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        if (req.method == "ping") return handle_ping(req);

        return make_error_response(req.id, error_code::MethodNotFound,
                                   "Method not found: " + req.method);
    }

    /// Handle raw JSON string → MCPResponse
    MCPResponse handle_json(const std::string& json_str) {
        Json j;
        try {
            j = Json::parse(json_str);
        } catch (...) {
            return make_error_response(nullptr, error_code::ParseError,
                                       "Parse error");
        }

        MCPRequest req;
        req.jsonrpc = j.value("jsonrpc", "2.0");
        req.method = j.value("method", "");
        req.params = j.value("params", Json::object());
        req.id = j.value("id", Json(nullptr));

        if (req.method.empty()) {
            return make_error_response(req.id, error_code::InvalidRequest,
                                       "Missing method");
        }

        return handle(req);
    }

    const std::string& server_name() const { return server_name_; }
    const std::string& version() const { return version_; }

private:
    tools::ToolManager& tool_manager_;
    std::string server_name_;
    std::string version_;

    MCPResponse handle_initialize(const MCPRequest& req) {
        Json result;
        result["protocolVersion"] = "2024-11-05";
        result["capabilities"]["tools"]["listChanged"] = false;
        result["serverInfo"]["name"] = server_name_;
        result["serverInfo"]["version"] = version_;
        return {.jsonrpc = "2.0", .result = result, .error = nullptr, .id = req.id};
    }

    MCPResponse handle_ping(const MCPRequest& req) {
        return {.jsonrpc = "2.0", .result = Json::object(), .error = nullptr, .id = req.id};
    }

    MCPResponse handle_tools_list(const MCPRequest& req) {
        Json tools_array = Json::array();
        auto schemas = tool_manager_.registry().list_schemas();
        for (const auto& schema : schemas) {
            Json tool_json;
            tool_json["name"] = schema.id;
            tool_json["description"] = schema.description;

            // Build inputSchema from params
            Json properties = Json::object();
            Json required_arr = Json::array();
            for (const auto& p : schema.params) {
                Json param_json;
                param_json["type"] = param_type_str(p.type);
                param_json["description"] = p.description;
                properties[p.name] = param_json;
                if (p.required) required_arr.push_back(p.name);
            }

            Json input_schema;
            input_schema["type"] = "object";
            input_schema["properties"] = properties;
            if (!required_arr.empty()) input_schema["required"] = required_arr;
            tool_json["inputSchema"] = input_schema;

            tools_array.push_back(tool_json);
        }

        Json result;
        result["tools"] = tools_array;
        return {.jsonrpc = "2.0", .result = result, .error = nullptr, .id = req.id};
    }

    MCPResponse handle_tools_call(const MCPRequest& req) {
        if (!req.params.contains("name")) {
            return make_error_response(req.id, error_code::InvalidParams,
                                       "Missing 'name' in params");
        }

        std::string tool_name = req.params["name"].get<std::string>();
        if (!tool_manager_.registry().find(tool_name)) {
            return make_error_response(req.id, error_code::InvalidParams,
                                       "Tool not found: " + tool_name);
        }

        kernel::ToolCallRequest call;
        call.id = req.id.is_null() ? "mcp-call" : req.id.dump();
        call.name = tool_name;
        call.args_json = req.params.contains("arguments")
            ? req.params["arguments"].dump()
            : Json::object().dump();

        auto result = tool_manager_.dispatch(call);

        Json content = Json::array();
        Json text_content;
        text_content["type"] = "text";
        text_content["text"] = result.success ? result.output : result.error;
        content.push_back(text_content);

        Json res;
        res["content"] = content;
        res["isError"] = !result.success;
        return {.jsonrpc = "2.0", .result = res, .error = nullptr, .id = req.id};
    }

    static MCPResponse make_error_response(const Json& id, int code, const std::string& message) {
        Json err;
        err["code"] = code;
        err["message"] = message;
        return {.jsonrpc = "2.0", .result = nullptr, .error = err, .id = id};
    }

    static std::string param_type_str(tools::ParamType t) {
        switch (t) {
            case tools::ParamType::String:  return "string";
            case tools::ParamType::Integer: return "integer";
            case tools::ParamType::Float:   return "number";
            case tools::ParamType::Boolean: return "boolean";
            case tools::ParamType::Object:  return "object";
            case tools::ParamType::Array:   return "array";
            default: return "string";
        }
    }
};

} // namespace agentos::mcp
