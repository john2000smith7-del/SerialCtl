#include "McpStdioBridge.h"
#include "McpService.h"
#include "McpIpc.h"
#include "McpProtocol.h"
#include <iostream>
#include <string>

namespace serialctl::mcp {

int RunMcpStdioBridge() {
    McpService::Instance().ConfigureDispatcher();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        try {
            Json req = Json::parse(line);
            if (req.is_object() && req.value("method", "") == "notifications/initialized") {
                continue;
            }
        } catch (...) {}

        std::string response = McpService::Instance().Dispatcher().Dispatch("127.0.0.1", line);
        if (!response.empty() && response != "{}") {
            std::cout << response << "\n";
            std::cout.flush();
        }
    }
    return 0;
}

} // namespace serialctl::mcp
