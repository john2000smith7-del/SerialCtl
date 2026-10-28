#include "CmdConsoleBridge.h"
#include "MainWindow.h"
#include "UpdateInstaller.h"
#include "GuiInstance.h"
#include "NetworkConnect.h"

// Windows SDK GDI+ declarations require the COM/property types first.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
// clang-format on

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR parameters, int showCommand) {
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
    int updateExitCode = 0;
    if (serialctl::update::HandleUpdateCommandLine(parameters, updateExitCode))
        return updateExitCode;
    serialctl::GuiInstance guiInstance;
    const auto instanceResult = guiInstance.Begin();
    if (instanceResult == serialctl::GuiInstance::Result::Existing) return 0;
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
    return static_cast<int>(message.wParam);
}
