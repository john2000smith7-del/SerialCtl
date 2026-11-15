#include "CmdConsoleBridge.h"
#include "MainWindow.h"
#include "UpdateInstaller.h"
#include "GuiInstance.h"
#include "NetworkConnect.h"
#include "McpStdioBridge.h"
#include "McpService.h"
#include "AiVoiceInput.h"
#include "OperationAuditLogger.h"
#include "PowerService.h"
#include "SessionLogger.h"

// Windows SDK GDI+ declarations require the COM/property types first.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <dbghelp.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <vector>

namespace {
std::atomic<bool> g_crashHandling{false};

void PruneCrashDumps(const std::wstring &crashDir, size_t keepCount) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((crashDir + L"\\*.dmp").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    struct Entry {
        std::wstring path;
        ULARGE_INTEGER time{};
    };
    std::vector<Entry> entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Entry e;
        e.path = crashDir + L"\\" + fd.cFileName;
        e.time.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        e.time.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        entries.push_back(std::move(e));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (entries.size() <= keepCount) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.time.QuadPart > b.time.QuadPart; });
    for (size_t i = keepCount; i < entries.size(); ++i)
        DeleteFileW(entries[i].path.c_str());
}

void WriteCrashMiniDump(EXCEPTION_POINTERS *exceptionPointers) {
    using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                               PMINIDUMP_EXCEPTION_INFORMATION,
                                               PMINIDUMP_USER_STREAM_INFORMATION,
                                               PMINIDUMP_CALLBACK_INFORMATION);
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    HMODULE dbghelp = LoadLibraryW((std::wstring(sysDir) + L"\\dbghelp.dll").c_str());
    if (!dbghelp) dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (writeDump) {
        std::error_code ec;
        const std::wstring crashDir = serialctl::SessionLogger::DefaultLogDirectory() + L"\\crash";
        std::filesystem::create_directories(std::filesystem::path(crashDir), ec);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t filename[96]{};
        swprintf_s(filename, L"\\serialctl-%04u%02u%02u-%02u%02u%02u-%03u-%lu.dmp",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                   now.wMilliseconds, GetCurrentProcessId());
        const std::wstring dumpPath = crashDir + filename;
        HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = exceptionPointers;
            mei.ClientPointers = FALSE;
            const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithHandleData);
            writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, flags,
                      exceptionPointers ? &mei : nullptr, nullptr, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
            PruneCrashDumps(crashDir, 5);
        }
    }
    FreeLibrary(dbghelp);
}

void HandleFatalCrash(EXCEPTION_POINTERS *exceptionPointers) {
    if (g_crashHandling.exchange(true)) return;
    serialctl::PowerService::MarkEmergencyRecoveryNeededAll();
    serialctl::SessionLogger::EmergencyFlushAll();
    serialctl::OperationAuditLogger::EmergencyFlushAll();
    WriteCrashMiniDump(exceptionPointers);
}

LONG WINAPI SerialCtlUnhandledExceptionFilter(EXCEPTION_POINTERS *exceptionPointers) {
    HandleFatalCrash(exceptionPointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void SerialCtlTerminateHandler() {
    RaiseException(0xE06D7363u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR parameters, int showCommand) {
    SetUnhandledExceptionFilter(&SerialCtlUnhandledExceptionFilter);
    std::set_terminate(&SerialCtlTerminateHandler);
    if (std::wstring(parameters).rfind(serialctl::NetworkResolverArgument, 0) == 0) {
        int count = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        const int result = arguments && count == 3 && std::wstring(arguments[1]) == serialctl::NetworkResolverArgument
            ? serialctl::RunNetworkResolver(arguments[2]) : 1;
        if (arguments) LocalFree(arguments);
        return result;
    }
    if (std::wstring(parameters) == L"--serialctl-cmd-bridge")
        return serialctl::RunCmdConsoleBridge();
    if (std::wstring(parameters) == L"--mcp-stdio")
        return serialctl::mcp::RunMcpStdioBridge();
    if (std::wstring_view(parameters).rfind(serialctl::ai::VoiceWorkerArgument, 0) == 0) {
        int count = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        const int result = arguments && count == 4 && std::wstring_view(arguments[1]) == serialctl::ai::VoiceWorkerArgument
            ? serialctl::ai::RunVoiceWorker(arguments[2], arguments[3]) : 1;
        if (arguments) LocalFree(arguments);
        return result;
    }
    // Legacy resident-service entrypoints are intentionally disabled: the user starts
    // the GUI manually and MCP/TFTP end when the GUI exits.
    if (std::wstring(parameters) == L"--mcp-service" ||
        std::wstring(parameters) == L"--mcp-agent") return 1;
    int updateExitCode = 0;
    if (serialctl::update::HandleUpdateCommandLine(parameters, updateExitCode))
        return updateExitCode;
    // Remove a legacy per-user Run entry on the first manual launch after upgrade.
    serialctl::mcp::McpService::RemoveLegacyUserAutoStart();
    serialctl::mcp::McpService::SignalUserAgentStop();
    serialctl::GuiInstance guiInstance;
    const auto instanceResult = guiInstance.Begin();
    if (instanceResult == serialctl::GuiInstance::Result::Existing) return 0;
    if (instanceResult == serialctl::GuiInstance::Result::CrossSessionExisting) {
        MessageBoxW(nullptr, L"SerialCtl 已在此计算机的其他登录会话中运行，请先返回原会话或关闭原实例。", L"SerialCtl", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (instanceResult == serialctl::GuiInstance::Result::Failed) {
        MessageBoxW(nullptr, L"无法建立应用实例，请稍后重新打开。", L"SerialCtl", MB_OK | MB_ICONERROR);
        return 1;
    }
    serialctl::update::StartupGuard startup;
    std::wstring startupError;
    const auto startupAction = startup.Begin(parameters, startupError);
    if (startupAction == serialctl::update::StartupAction::Exit) return 0;
    if (startupAction == serialctl::update::StartupAction::Failed) {
        MessageBoxW(nullptr, startupError.c_str(), L"SerialCtl 更新恢复", MB_OK | MB_ICONERROR);
        return 1;
    }
    struct GraphicsSession {
        ULONG_PTR token = 0;
        ~GraphicsSession() {
            if (token)
                Gdiplus::GdiplusShutdown(token);
        }
    } graphics;
    Gdiplus::GdiplusStartupInput graphicsInput;
    if (Gdiplus::GdiplusStartup(&graphics.token, &graphicsInput, nullptr) != Gdiplus::Ok)
        return 1;
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WSADATA socketData{};
    if (WSAStartup(MAKEWORD(2, 2), &socketData) != 0) {
        MessageBoxW(nullptr, L"无法初始化 Windows 网络组件。", L"SerialCtl", MB_OK | MB_ICONERROR);
        return 1;
    }
    struct SocketSession { ~SocketSession() { WSACleanup(); } } socketSession;

    serialctl::MainWindow mainWindow;
    if (!mainWindow.Create(instance, showCommand)) {
        MessageBoxW(nullptr, L"无法创建主窗口。", L"SerialCtl", MB_OK | MB_ICONERROR);
        return 1;
    }
    guiInstance.Publish(mainWindow.Handle());
    if (!startup.Confirm(startupError, SERIALCTL_APP_VERSION)) {
        // The worker owns rollback. Close the just-created, idle GUI normally
        // without blocking its health timeout behind a modal error dialog.
        SendMessageW(mainWindow.Handle(), WM_CLOSE, 0, 0);
        return 1;
    }
    if (startup.ShouldCheckForUpdate()) mainWindow.StartStartupUpdate();

    MSG message{};
    bool quit = false;
    const HANDLE activation = guiInstance.Activation();
    while (!quit) {
        const auto wait = MsgWaitForMultipleObjects(1, &activation, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_FAILED) break;
        if (wait == WAIT_OBJECT_0 && !mainWindow.Closing()) guiInstance.Activate();
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { quit = true; break; }
            if (mainWindow.PreTranslate(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    serialctl::OperationAuditLogger::Instance().Shutdown();
    return static_cast<int>(message.wParam);
}
