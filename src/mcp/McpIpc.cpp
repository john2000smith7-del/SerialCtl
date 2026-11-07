#include "McpIpc.h"
#include <sddl.h>
#include <vector>
#include <cstdint>

namespace serialctl::mcp {

namespace {
bool OverlappedReadExact(HANDLE pipe, void* buffer, DWORD length, HANDLE stopEvent) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    DWORD total = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    while (total < length) {
        ResetEvent(ov.hEvent);
        DWORD chunk = 0;
        BOOL ok = ReadFile(pipe, ptr + total, length - total, &chunk, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            HANDLE events[2] = {stopEvent, ov.hEvent};
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, 5000);
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &chunk, FALSE) || chunk == 0) {
                CloseHandle(ov.hEvent);
                return false;
            }
        } else if (chunk == 0) {
            CloseHandle(ov.hEvent);
            return false;
        }
        total += chunk;
    }
    CloseHandle(ov.hEvent);
    return true;
}

bool OverlappedWriteExact(HANDLE pipe, const void* buffer, DWORD length, HANDLE stopEvent) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
    DWORD total = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    while (total < length) {
        ResetEvent(ov.hEvent);
        DWORD chunk = 0;
        BOOL ok = WriteFile(pipe, ptr + total, length - total, &chunk, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            HANDLE events[2] = {stopEvent, ov.hEvent};
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, 5000);
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &chunk, FALSE) || chunk == 0) {
                CloseHandle(ov.hEvent);
                return false;
            }
        } else if (chunk == 0) {
            CloseHandle(ov.hEvent);
            return false;
        }
        total += chunk;
    }
    CloseHandle(ov.hEvent);
    return true;
}
} // namespace

bool SendIpcRequest(const std::wstring& pipeName, const std::string& request,
                    std::string& response, DWORD timeoutMs) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const DWORD startTick = GetTickCount();

    while (true) {
        pipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }

        if (GetLastError() != ERROR_PIPE_BUSY) {
            return false;
        }

        const DWORD elapsed = GetTickCount() - startTick;
        if (elapsed >= timeoutMs) {
            return false;
        }

        if (!WaitNamedPipeW(pipeName.c_str(), timeoutMs - elapsed)) {
            return false;
        }
    }

    // Write 4-byte length prefix + request payload
    uint32_t reqLen = static_cast<uint32_t>(request.size());
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe, &reqLen, sizeof(reqLen), &bytesWritten, nullptr) || bytesWritten != sizeof(reqLen)) {
        CloseHandle(pipe);
        return false;
    }

    if (reqLen > 0) {
        if (!WriteFile(pipe, request.data(), reqLen, &bytesWritten, nullptr) || bytesWritten != reqLen) {
            CloseHandle(pipe);
            return false;
        }
    }

    // Read 4-byte length prefix + response payload
    uint32_t respLen = 0;
    DWORD bytesRead = 0;
    if (!ReadFile(pipe, &respLen, sizeof(respLen), &bytesRead, nullptr) || bytesRead != sizeof(respLen)) {
        CloseHandle(pipe);
        return false;
    }

    if (respLen > 16 * 1024 * 1024) { // 16 MB sanity bound
        CloseHandle(pipe);
        return false;
    }

    response.resize(respLen);
    size_t totalRead = 0;
    while (totalRead < respLen) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, &response[totalRead], static_cast<DWORD>(respLen - totalRead), &chunk, nullptr) || chunk == 0) {
            CloseHandle(pipe);
            return false;
        }
        totalRead += chunk;
    }

    CloseHandle(pipe);
    return true;
}

McpIpcServer::McpIpcServer(std::wstring pipeName)
    : pipeName_(std::move(pipeName)) {
}

McpIpcServer::~McpIpcServer() {
    Stop();
}

bool McpIpcServer::Start(Handler handler) {
    if (running_.load()) return true;
    handler_ = std::move(handler);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;

    running_.store(true);
    workerThread_ = std::thread([this] { RunServer(); });
    return true;
}

void McpIpcServer::Stop() {
    if (!running_.exchange(false)) return;

    if (stopEvent_) {
        SetEvent(stopEvent_);
    }

    // Connect to unblock ConnectNamedPipe if waiting
    HANDLE unblock = CreateFileW(
        pipeName_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (unblock != INVALID_HANDLE_VALUE) {
        CloseHandle(unblock);
    }

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

void McpIpcServer::RunServer() {
    PSECURITY_DESCRIPTOR pSd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)",
            SDDL_REVISION_1, &pSd, nullptr)) {
        sa.lpSecurityDescriptor = pSd;
    }

    auto createPipeInstance = [&]() -> HANDLE {
        return CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536,
            65536,
            3000,
            &sa);
    };

    HANDLE nextPipe = createPipeInstance();

    while (running_.load()) {
        HANDLE pipe = nextPipe != INVALID_HANDLE_VALUE ? nextPipe : createPipeInstance();
        nextPipe = INVALID_HANDLE_VALUE;

        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(20);
            continue;
        }

        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(pipe, &connectOverlapped);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE events[2] = {stopEvent_, connectOverlapped.hEvent};
                const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0) { // Stop
                    CancelIo(pipe);
                    CloseHandle(connectOverlapped.hEvent);
                    CloseHandle(pipe);
                    break;
                }
                connected = TRUE;
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(connectOverlapped.hEvent);

        if (!running_.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }

        // Pre-create next pipe instance BEFORE servicing/closing current pipe so clients never see ERROR_FILE_NOT_FOUND
        nextPipe = createPipeInstance();

        if (connected) {
            uint32_t reqLen = 0;
            if (OverlappedReadExact(pipe, &reqLen, sizeof(reqLen), stopEvent_)) {
                if (reqLen > 0 && reqLen <= 16 * 1024 * 1024) {
                    std::string req(reqLen, '\0');
                    if (OverlappedReadExact(pipe, &req[0], reqLen, stopEvent_) && handler_) {
                        std::string resp = handler_(req);
                        uint32_t respLen = static_cast<uint32_t>(resp.size());
                        if (OverlappedWriteExact(pipe, &respLen, sizeof(respLen), stopEvent_) && respLen > 0) {
                            OverlappedWriteExact(pipe, resp.data(), respLen, stopEvent_);
                        }
                        FlushFileBuffers(pipe);
                    }
                }
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }

    if (nextPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(nextPipe);
    }

    if (pSd) {
        LocalFree(pSd);
    }
}

} // namespace serialctl::mcp
