#include "CmdConsoleBridge.h"
#include "MainWindow.h"

// Windows SDK GDI+ declarations require the COM/property types first.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
// clang-format on

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR parameters, int showCommand) {
    if (std::wstring(parameters) == L"--serialctl-cmd-bridge")
        return serialctl::RunCmdConsoleBridge();
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

    serialctl::MainWindow mainWindow;
    if (!mainWindow.Create(instance, showCommand)) {
        MessageBoxW(nullptr, L"无法创建主窗口。", L"SerialCtl", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (mainWindow.PreTranslate(message))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WSACleanup();
    return static_cast<int>(message.wParam);
}
