#include "McpStdioBridge.h"
#include "McpIpc.h"
#include "McpProtocol.h"
#include <array>
#include <iostream>
#include <string>

namespace serialctl::mcp {
namespace {
HANDLE OpenGuiProcess() {
    HANDLE pipe = CreateFileW(DefaultMcpPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(DefaultMcpPipeName, 1000))
        pipe = CreateFileW(DefaultMcpPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return nullptr;
    ULONG pid = 0;
    HANDLE process = GetNamedPipeServerProcessId(pipe, &pid) ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    CloseHandle(pipe);
    return process;
}

void CALLBACK GuiExited(void*, BOOLEAN) {
    // The host can stop consuming stdout. End this dedicated bridge even if
    // its synchronous stdin, stdout, or IPC operation has not returned yet.
    TerminateProcess(GetCurrentProcess(), 0);
}
}

int RunMcpStdioBridge() {
    struct Process {
        HANDLE handle = OpenGuiProcess();
        ~Process() { if (handle) CloseHandle(handle); }
    } gui;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    if (!gui.handle || !input || input == INVALID_HANDLE_VALUE) {
        std::cerr << "Start SerialCtl manually before attaching MCP stdio.\n";
        return 1;
    }
    struct LifetimeWait {
        HANDLE handle = nullptr;
        ~LifetimeWait() { if (handle) UnregisterWaitEx(handle, INVALID_HANDLE_VALUE); }
    } lifetime;
    if (!RegisterWaitForSingleObject(&lifetime.handle, gui.handle, GuiExited, nullptr, INFINITE, WT_EXECUTEONLYONCE))
        return 1;

    constexpr size_t maxMessage = 1024 * 1024;
    std::array<char, 4096> bytes{};
    std::string line;
    while (WaitForSingleObject(gui.handle, 0) == WAIT_TIMEOUT) {
        DWORD count = 0;
        if (!ReadFile(input, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || !count)
            break;
        for (DWORD i = 0; i < count; ++i) {
            if (bytes[i] != '\n') {
                if (line.size() == maxMessage) return 1;
                line.push_back(bytes[i]);
                continue;
            }
            if (WaitForSingleObject(gui.handle, 0) != WAIT_TIMEOUT) return 0;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            const Json request{{"method", "internal.mcp_stdio"}, {"params", {{"request", line}}}};
            std::string response;
            if (!SendIpcRequest(DefaultMcpPipeName, request.dump(-1, ' ', false, Json::error_handler_t::replace), response))
                return 1;
            if (!response.empty() && response != "{}") std::cout << response << '\n' << std::flush;
            if (!std::cout) return 1;
            line.clear();
        }
    }
    return 0;
}

} // namespace serialctl::mcp
