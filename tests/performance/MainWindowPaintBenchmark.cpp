#include "MainWindow.h"
#include "json.hpp"
#include <commctrl.h>
#include <gdiplus.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace serialctl {
namespace {
struct BenchmarkTransport : IConnection {
    size_t sentBytes = 0;
    bool Start(DataCallback, StatusCallback, std::wstring &) override { return true; }
    void Stop() override {}
    bool IsConnected() const override { return true; }
    bool Send(const Bytes &bytes, std::wstring &) override { sentBytes += bytes.size(); return true; }
};
}
struct AiDeviceTestAccess {
    static inline size_t resizeMessages = 0;
    static LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        if (m == WM_SIZE) ++resizeMessages;
        if (m == WM_PAINT || m == WM_SIZE || m == WM_TIMER || m == WM_DRAWITEM || m == WM_MEASUREITEM ||
            (m >= WM_APP && m <= WM_APP + 100) || (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC))
            return reinterpret_cast<MainWindow *>(data)->HandleMessage(m, w, l);
        return DefSubclassProc(h, m, w, l);
    }
    static nlohmann::json Run() {
        const auto resourcesBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        nlohmann::json result;
        {
            MainWindow main;
            auto session = std::make_unique<MainWindow::SessionState>();
            session->id = 1;
            session->mode = 1;
            auto transport = std::make_shared<BenchmarkTransport>();
            session->connection = transport;
            main.sessionService_.Register({{"id", "session-1"}, {"kind", "serial"}}, transport);
            main.selectedMode_ = 1;
            main.connection_ = transport.get();
            main.localEchoEnabled_ = true;
            std::wstring terminalContent;
            for (int i = 0; i < 1000; ++i) terminalContent += L"benchmark terminal content 0123456789\r\n";
            session->terminal.Feed(terminalContent, L"");
            main.activeSession_ = session.get();
            main.sessions_.push_back(std::move(session));
            const HWND parent = CreateWindowExW(0, L"STATIC", L"SerialCtl paint benchmark",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 1400, 850, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!parent) throw std::runtime_error("Cannot create paint fixture window");
            struct WindowGuard { HWND h; ~WindowGuard() { if (IsWindow(h)) DestroyWindow(h); } } guard{parent};
            main.window_ = parent;
            main.instance_ = GetModuleHandleW(nullptr);
            main.petSettings_.enabled = false;
            if (!SetWindowSubclass(parent, ParentProc, 7201, reinterpret_cast<DWORD_PTR>(&main)))
                throw std::runtime_error("Cannot install production paint fixture");
            main.CreateControls();
            main.ApplyTheme();
            ShowWindow(parent, SW_SHOW);
            RECT workArea{};
            if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
                throw std::runtime_error("Cannot determine desktop work area");
            const int wide = (std::min)(1400, static_cast<int>(workArea.right - workArea.left) - 20);
            const int tall = (std::min)(850, static_cast<int>(workArea.bottom - workArea.top) - 20);
            if (wide < 800 || tall < 600) throw std::runtime_error("Desktop work area is too small for the benchmark");
            SetWindowPos(parent, HWND_TOPMOST, workArea.left, workArea.top, wide, tall, SWP_SHOWWINDOW);
            SetForegroundWindow(parent);
            RECT viewport{};
            GetClientRect(parent, &viewport);
            main.LayoutControls(viewport.right, viewport.bottom);
            RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            HDC screen = GetDC(parent);
            RECT clip{};
            const int clipKind = screen ? GetClipBox(screen, &clip) : ERROR;
            if (screen) ReleaseDC(parent, screen);
            if (clipKind == NULLREGION || clipKind == ERROR)
                throw std::runtime_error("Paint fixture is not exposed on the desktop");
            if (!main.terminal_ || !IsWindowVisible(main.terminal_))
                throw std::runtime_error("Terminal fixture is not visible");
            const auto paintOnce = [&](HWND target) {
                InvalidateRect(target, nullptr, FALSE);
                if (!GetUpdateRect(target, nullptr, FALSE)) throw std::runtime_error("Missing paint region");
                const auto started = std::chrono::steady_clock::now();
                SendMessageW(target, WM_PAINT, 0, 0);
                GdiFlush();
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            };
            for (int i = 0; i < 20; ++i) { paintOnce(parent); paintOnce(main.terminal_); }
            const auto warmGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            FILETIME created{}, exited{}, kernelStart{}, userStart{}, kernelEnd{}, userEnd{};
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelStart, &userStart))
                throw std::runtime_error("Cannot read process CPU counters");
            std::vector<double> paints, terminalPaints, resizes, steadyResizes;
            std::vector<std::vector<double>> resizeRoundSamples;
            const auto wallStart = std::chrono::steady_clock::now();
            for (int i = 0; i < 200; ++i) {
                paints.push_back(paintOnce(parent));
                terminalPaints.push_back(paintOnce(main.terminal_));
            }
            const auto afterPaintGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            std::vector<DWORD> resizeGdi;
            for (int round = 0; round < 3; ++round) {
                std::vector<double> currentResizes;
                resizeMessages = 0;
                for (int i = 0; i < 40; ++i) {
                    const int width = i % 2 ? wide - 80 : wide;
                    const int height = i % 2 ? tall - 50 : tall;
                    const auto started = std::chrono::steady_clock::now();
                    if (!SetWindowPos(parent, nullptr, 0, 0, width, height,
                                      SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE))
                        throw std::runtime_error("Cannot resize paint fixture");
                    UpdateWindow(parent);
                    GdiFlush();
                    const double elapsed = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();
                    if (round == 0) resizes.push_back(elapsed);
                    else steadyResizes.push_back(elapsed);
                    currentResizes.push_back(elapsed);
                    RECT bounds{};
                    if (!GetWindowRect(parent, &bounds)) throw std::runtime_error("Cannot read resized window bounds");
                    if (bounds.right - bounds.left != width || bounds.bottom - bounds.top != height)
                        throw std::runtime_error("Requested resize " + std::to_string(width) + "x" +
                            std::to_string(height) + " yielded " + std::to_string(bounds.right - bounds.left) + "x" +
                            std::to_string(bounds.bottom - bounds.top));
                }
                if (resizeMessages < 39) throw std::runtime_error("Resize did not reach the production WM_SIZE handler");
                resizeRoundSamples.push_back(std::move(currentResizes));
                resizeGdi.push_back(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
            }
            // Model an actual resize gesture: production defers terminal model
            // reflow between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE.
            std::vector<double> interactiveResizes;
            const int initialColumns = main.activeSession_->terminal.Columns();
            main.HandleMessage(WM_ENTERSIZEMOVE, 0, 0);
            for (int i = 0; i < 80; ++i) {
                const int width = i % 2 ? wide : wide - 80;
                const int height = i % 2 ? tall : tall - 50;
                const auto started = std::chrono::steady_clock::now();
                if (!SetWindowPos(parent, nullptr, 0, 0, width, height,
                                  SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE))
                    throw std::runtime_error("Cannot resize interactive fixture");
                UpdateWindow(parent);
                GdiFlush();
                interactiveResizes.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
                if (main.activeSession_->terminal.Columns() != initialColumns)
                    throw std::runtime_error("Terminal reflow was not deferred during interactive resize");
            }
            const auto commitStart = std::chrono::steady_clock::now();
            main.HandleMessage(WM_EXITSIZEMOVE, 0, 0);
            const auto resizeCommitMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - commitStart).count();
            if (main.activeSession_->terminal.Columns() == initialColumns)
                throw std::runtime_error("Terminal did not commit its final interactive dimensions");
            const auto wallMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wallStart).count();
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelEnd, &userEnd))
                throw std::runtime_error("Cannot read final process CPU counters");
            const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            const auto cpuMs = double(ticks(kernelEnd) + ticks(userEnd) - ticks(kernelStart) - ticks(userStart)) / 10000;
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelStart, &userStart))
                throw std::runtime_error("Cannot read input process CPU counters");
            std::vector<double> inputs;
            const auto inputStart = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; ++i) {
                const auto started = std::chrono::steady_clock::now();
                SendMessageW(main.terminal_, WM_CHAR, L'a', 1);
                UpdateWindow(main.terminal_);
                GdiFlush();
                inputs.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
            }
            const auto inputWallMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - inputStart).count();
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelEnd, &userEnd))
                throw std::runtime_error("Cannot read final input CPU counters");
            const auto inputCpuMs = double(ticks(kernelEnd) + ticks(userEnd) - ticks(kernelStart) - ticks(userStart)) / 10000;
            if (transport->sentBytes != 1000) throw std::runtime_error("Input benchmark lost or added bytes");
            const auto timedInputBytes = transport->sentBytes;
            const auto afterInputGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (int i = 0; i < 1000; ++i) {
                SendMessageW(main.terminal_, WM_CHAR, L'a', 1);
                UpdateWindow(main.terminal_);
                GdiFlush();
            }
            if (transport->sentBytes != 2000) throw std::runtime_error("Repeated input lost or added bytes");
            const auto afterGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            PROCESS_MEMORY_COUNTERS memory{};
            if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
                throw std::runtime_error("Cannot read paint working set");
            std::sort(paints.begin(), paints.end());
            std::sort(terminalPaints.begin(), terminalPaints.end());
            std::sort(inputs.begin(), inputs.end());
            std::sort(resizes.begin(), resizes.end());
            std::sort(steadyResizes.begin(), steadyResizes.end());
            std::sort(interactiveResizes.begin(), interactiveResizes.end());
            result = {{"clientWidth", viewport.right}, {"clientHeight", viewport.bottom},
                {"paintSamples", paints}, {"paintP50Ms", paints[100]}, {"paintP95Ms", paints[190]},
                {"terminalPaintSamples", terminalPaints}, {"terminalPaintP50Ms", terminalPaints[100]},
                {"terminalPaintP95Ms", terminalPaints[190]},
                {"resizeSamples", resizes}, {"resizeP50Ms", resizes[20]}, {"resizeP95Ms", resizes[38]},
                {"resizeSteadySamples", steadyResizes}, {"resizeSteadyP50Ms", steadyResizes[40]},
                {"resizeSteadyP95Ms", steadyResizes[76]}, {"resizeRoundSamples", resizeRoundSamples},
                {"interactiveResizeSamples", interactiveResizes}, {"interactiveResizeP50Ms", interactiveResizes[40]},
                {"interactiveResizeP95Ms", interactiveResizes[76]}, {"resizeCommitMs", resizeCommitMs},
                {"inputSamples", inputs}, {"inputP50Ms", inputs[500]}, {"inputP95Ms", inputs[950]},
                {"inputWallMs", inputWallMs}, {"inputCpuMs", inputCpuMs}, {"inputBytes", timedInputBytes},
                {"inputBytesPerSecond", 1000 * 1000 / inputWallMs},
                {"paintCount", paints.size()}, {"resizeCount", resizes.size()}, {"resizeMessages", resizeMessages},
                {"wallMs", wallMs}, {"cpuMs", cpuMs}, {"gdiBeforeApp", resourcesBefore},
                {"gdiWarm", warmGdi}, {"gdiAfterPaint", afterPaintGdi}, {"gdiResizeRounds", resizeGdi},
                {"gdiAfterTimedInput", afterInputGdi}, {"gdiAfterWork", afterGdi}, {"peakWorkingSetBytes", memory.PeakWorkingSetSize},
                {"scope", "actual MainWindow and terminal paint/layout and WM_CHAR local echo through a visible Win32 fixture with 1000 seeded lines and fake transport; no UART, physical keyboard or display scan"}};
            main.aiPet_.Shutdown();
            main.powerService_.Shutdown();
            main.aiPane_.Shutdown();
            DestroyWindow(parent);
            for (auto font : {main.uiFont_, main.smallFont_, main.titleFont_, main.terminalFont_}) if (font) DeleteObject(font);
            for (auto brush : {main.windowBrush_, main.panelBrush_, main.terminalBrush_}) if (brush) DeleteObject(brush);
            main.sessionService_.Unregister("session-1");
            main.connection_ = nullptr;
            main.activeSession_ = nullptr;
            main.sessions_.clear();
        }
        result["gdiAfterApp"] = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        return result;
    }
};
} // namespace serialctl

int main() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 1;
    WSADATA sockets{};
    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) { Gdiplus::GdiplusShutdown(token); return 1; }
    int status = 0;
    try { std::cout << serialctl::AiDeviceTestAccess::Run().dump() << '\n'; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; status = 1; }
    WSACleanup();
    Gdiplus::GdiplusShutdown(token);
    return status;
}
