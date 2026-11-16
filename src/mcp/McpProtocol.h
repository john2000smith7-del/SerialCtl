#pragma once
#include "json.hpp"
#include <string>
#include <vector>

namespace serialctl::mcp {

using Json = nlohmann::json;

struct McpToolDefinition {
    std::string name;
    std::string description;
    Json inputSchema;
};

class McpProtocol {
public:
    static const std::vector<McpToolDefinition>& ToolDefinitions();

    static Json HandleInitialize(const Json& id);
    static Json HandlePing(const Json& id);
    static Json HandleToolsList(const Json& id);

    static Json MakeSuccessResponse(const Json& id, const Json& result);
    static Json MakeErrorResponse(const Json& id, int code, const std::string& message, const Json& data = nullptr);
    static Json MakeToolResult(const Json& contentObj, bool isError = false);
    static Json MakeToolTextResult(const std::string& text, bool isError = false);
};

} // namespace serialctl::mcp
