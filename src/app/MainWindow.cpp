#include <future>
#include <chrono>
#include "TerminalDecoder.h"
#include "MainWindow.h"
#include "UiDialogViewport.h"
#include "HostKeyDialog.h"

#include "CmdConnection.h"
#include "PlinkConnection.h"
#include "QueuedConnection.h"
#include "SerialConnection.h"
#include "SharedSerialConnection.h"
#include "TcpConnection.h"
#include "UpdateClient.h"
#include "UpdateInstaller.h"
#include "Win32Helpers.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <new>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace serialctl {

namespace {

constexpr std::uint16_t DefaultSharePort = 7000;
constexpr wchar_t WindowClassName[] = L"SerialCtlWin7NativeWindowV3";
constexpr UINT MessageData = WM_APP + 1;
constexpr UINT MessageStatus = WM_APP + 2;
constexpr UINT MessageSftp = WM_APP + 3;
constexpr UINT MessageSftpProgress = WM_APP + 4;
constexpr UINT MessageConnection = WM_APP + 5;
constexpr UINT MessageHostKey = WM_APP + 6;
constexpr UINT MessageDiscovery = WM_APP + 7;
constexpr UINT MessageApi = WM_APP + 8;
constexpr UINT MessageUpdate = WM_APP + 20;
constexpr UINT MessageUpdatePrompt = WM_APP + 24;
struct UpdatePrompt {
    std::string version;
    std::promise<bool> answer;
};
constexpr UINT MessageLogExport = WM_APP + 21;

void DiscardUpdate(update::UpdateResult& result) {
    if (!result.stagingDirectory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(result.stagingDirectory, ignored);
        result.stagingDirectory.clear();
    }
}

constexpr UINT_PTR DiscoveryTimerId = 4003;
constexpr int IdSharedAdvancedPort = 9010;
constexpr int IdSharedAutoPort = 9011;
constexpr UINT_PTR StatusTimerId = 4001;
constexpr UINT_PTR CommandTimerId = 4002;
constexpr UINT_PTR SideAnimationTimerId = 4003;
constexpr UINT_PTR LogExportTimerId = 4004;
constexpr UINT_PTR CommandDragTimerId = 4005;
constexpr UINT_PTR BootPulseTimerId = 4006;
constexpr UINT_PTR ReceivePumpTimerId = 4007;
constexpr UINT_PTR SerialScanTimerId = 4008;
constexpr int IdCommandDragGhost = 9020;
constexpr std::uint32_t DefaultCommandIntervalMs = 500;
constexpr std::uint32_t MaximumCommandIntervalMs = 60000;
constexpr size_t MaximumCommandSteps = 10;

enum ControlId {
    IdSsh = 100,
    IdSerial,
    IdTelnet,
    IdShareSerial,
    IdTheme,
    IdDisconnect,
    IdConnectionList,
    IdTerminal,
    IdInput,
    IdSend,
    IdLineEnding,
    IdStatus,
    IdCommandList,
    IdImport,
    IdExport,
    IdAddCommand,
    IdDeleteCommand,
    IdCommandTab,
    IdSftpTab,
    IdSftpPath,
    IdSftpList,
    IdSftpUp,
    IdSftpRefresh,
    IdSftpUpload,
    IdSftpDownload,
    IdRightPanelToggle,
    IdSftpNameHeader,
    IdSftpSizeHeader,
    IdSftpModifiedHeader,
    IdSftpTransferToggle,
    IdSftpTransferList,
    IdSftpClearTransfers,
    IdSaveCommands,
    IdCmd = 300,
    IdPower,
    IdDisconnectAll,
    IdSettings,
    IdAi,
    IdSendSelectionToAi,
    IdAiWrap,
    IdPowerMonitorTarget
};

bool IsToolbarButton(int id) {
    return (id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower ||
        id == IdAi || id == IdSettings;
}

enum TerminalMenuId {
    IdMenuLocalEcho = 2001,
    IdMenuTimestamp,
    IdMenuSaveLog,
    IdMenuClear,
    IdMenuCopy,
    IdMenuSelectAll,
    IdMenuPaste,
    IdMenuEncodingUtf8 = 2201,
    IdMenuEncodingGbk,
    IdMenuEncodingGb2312,
    IdMenuRawTrace,
    IdMenuEncodingCmdAuto,
    IdMenuNetworkDiagnostics,
    IdMenuCancelLogExport
};

enum LineEndingMenuId {
    IdMenuEndingCr = 2101,
    IdMenuEndingLf,
    IdMenuEndingCrLf,
    IdMenuEndingNone
};

enum SettingsControlId {
    IdSettingsEcho = 2501,
    IdSettingsTimestamp,
    IdSettingsRawTrace,
    IdSettingsEncoding,
    IdSettingsEnding,
    IdSettingsExportNote
};

enum SftpMenuId {
    IdMenuSftpDownload = 2301,
    IdMenuSftpUpload,
    IdMenuSftpCreateDirectory,
    IdMenuSftpRename,
    IdMenuSftpDelete,
    IdMenuSftpRefresh,
    IdMenuSftpCopyPath,
    IdMenuSftpEnterDirectory,
    IdMenuSftpChangeMode,
    IdMenuSftpPath,
    IdMenuSftpFollow,
    IdMenuSftpPwd,
    IdMenuSftpHook
};

enum SftpTransferMenuId {
    IdMenuTransferCancel = 2401,
    IdMenuTransferRetry,
    IdMenuTransferClearFinished
};

namespace Ui {
int Dpi=96;
int Scale(int value) { return MulDiv(value,Dpi,96); }
int Radius = 10;
int CardRadius = 12;
int Space = 8;
int Gap = 12;
int OverlayScrollLaneWidth = 16;
int OverlayScrollThumbWidth = 5;
int TerminalPadding = 8;
int TerminalTimestampWidth = 112;
int Section = 16;
int ToolbarHeight = 72;
constexpr int ToolbarButtonWidths[] = {89, 88, 98, 118, 92, 88, 80};
constexpr int SettingsButtonWidth = 88;
int StandardHeight = 36;
int CompactHeight = 32;
int SegmentHeight = 32;
int SegmentWidth = 70;
int PanelPadding = 16;
int IconButtonSize = 28;
int MinLeftWidth = 190;
int MaxLeftWidth = 190;
int MinRightWidth = 260;
int MaxRightWidth = 650;
int CollapsedRightWidth = 48;
int MinCenterWidth = 400;
int SftpTableRowHeight = 36;
int SftpTransferRowHeight = 48;
int SftpTransferDrawerHeight = 144;
int TerminalFontMinimumHeight = 10;
int TerminalFontMaximumHeight = 32;
constexpr wchar_t HoverProperty[] = L"SerialCtl.ButtonHover";
constexpr wchar_t FieldFrameProperty[] = L"SerialCtl.FieldFrame";
constexpr wchar_t FieldEditProperty[] = L"SerialCtl.FieldEdit";
}

void ScaleUiMetrics(HWND window) {
    HDC dc=GetDC(window); int dpi=GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(window,dc);
    if (Ui::Dpi != 96) return;
    Ui::Dpi=dpi;
    Ui::Radius=Ui::Scale(Ui::Radius);
    Ui::CardRadius=Ui::Scale(Ui::CardRadius);
    Ui::Space=Ui::Scale(Ui::Space);
    Ui::Gap=Ui::Scale(Ui::Gap);
    Ui::OverlayScrollLaneWidth=Ui::Scale(Ui::OverlayScrollLaneWidth);
    Ui::OverlayScrollThumbWidth=Ui::Scale(Ui::OverlayScrollThumbWidth);
    Ui::TerminalPadding=Ui::Scale(Ui::TerminalPadding);
    Ui::TerminalTimestampWidth=Ui::Scale(Ui::TerminalTimestampWidth);
    Ui::Section=Ui::Scale(Ui::Section);
    Ui::ToolbarHeight=Ui::Scale(Ui::ToolbarHeight);
    Ui::StandardHeight=Ui::Scale(Ui::StandardHeight);
    Ui::CompactHeight=Ui::Scale(Ui::CompactHeight);
    Ui::SegmentHeight=Ui::Scale(Ui::SegmentHeight);
    Ui::SegmentWidth=Ui::Scale(Ui::SegmentWidth);
    Ui::PanelPadding=Ui::Scale(Ui::PanelPadding);
    Ui::IconButtonSize=Ui::Scale(Ui::IconButtonSize);
    Ui::MinLeftWidth=Ui::Scale(Ui::MinLeftWidth);
    Ui::MaxLeftWidth=Ui::Scale(Ui::MaxLeftWidth);
    Ui::MinRightWidth=Ui::Scale(Ui::MinRightWidth);
    Ui::MaxRightWidth=Ui::Scale(Ui::MaxRightWidth);
    Ui::CollapsedRightWidth=Ui::Scale(Ui::CollapsedRightWidth);
    Ui::MinCenterWidth=Ui::Scale(Ui::MinCenterWidth);
    Ui::SftpTableRowHeight=Ui::Scale(Ui::SftpTableRowHeight);
    Ui::SftpTransferRowHeight=Ui::Scale(Ui::SftpTransferRowHeight);
    Ui::SftpTransferDrawerHeight=Ui::Scale(Ui::SftpTransferDrawerHeight);
    Ui::TerminalFontMinimumHeight=Ui::Scale(Ui::TerminalFontMinimumHeight);
    Ui::TerminalFontMaximumHeight=Ui::Scale(Ui::TerminalFontMaximumHeight);
}

struct MainLayoutMetrics {
    int leftWidth;
    int rightWidth;
    int rightLeft;
    int centerLeft;
    int centerRight;
    int contentBottom;
};

bool CompactNavigation(int width) {
    int full = Ui::Gap + Ui::Space;
    for (int item : Ui::ToolbarButtonWidths) full += Ui::Scale(item) + Ui::Space;
    full += Ui::Scale(Ui::SettingsButtonWidth) + Ui::Space;
    return full + Ui::Gap + Ui::StandardHeight + Ui::Gap > width;
}

RECT NavigationFrame(int width) {
    int right = Ui::Gap + Ui::Space;
    const bool compact = CompactNavigation(width);
    for (int item : Ui::ToolbarButtonWidths) right += Ui::Scale(compact ? 48 : item) + Ui::Space;
    right += Ui::Scale(compact ? 48 : Ui::SettingsButtonWidth) + Ui::Space;
    return {Ui::Gap, Ui::Section, right, Ui::ToolbarHeight - Ui::Section};
}

int MainCardTop() {
    // Match the horizontal card/splitter gaps, measuring from the visible bar.
    return Ui::ToolbarHeight - Ui::Section + Ui::Gap;
}

MainLayoutMetrics GetMainLayoutMetrics(int width, int height, int preferredRightWidth, bool rightPanelCollapsed,
                                       int animationWidth = -1, bool ai = false) {
    const int leftWidth = std::max(Ui::MinLeftWidth, std::min(Ui::MaxLeftWidth, width * 18 / 100));
    const int availableRight =
        std::max(Ui::MinRightWidth, width - leftWidth - Ui::Gap * 2 - (ai ? Ui::MinRightWidth : Ui::MinCenterWidth));
    const int rightWidth =
        animationWidth >= 0 ? animationWidth
        : rightPanelCollapsed
            ? Ui::CollapsedRightWidth + Ui::Gap
            : std::max(Ui::MinRightWidth,
                       std::min({ai ? availableRight : Ui::MaxRightWidth, availableRight, preferredRightWidth}));
    return {leftWidth, rightWidth, width - rightWidth, leftWidth + Ui::Gap,
        width - rightWidth - Ui::Gap, height - Ui::Gap};
}

struct CommandActionRects {
    RECT run{};
    RECT edit{};
};

CommandActionRects GetCommandActionRects(const RECT& card) {
    const int top = card.top + ((card.bottom - card.top) - Ui::IconButtonSize) / 2;
    RECT edit{card.right - Ui::Gap - Ui::IconButtonSize, top, card.right - Ui::Gap, top + Ui::IconButtonSize};
    RECT run{edit.left - Ui::Gap - Ui::IconButtonSize, top, edit.left - Ui::Gap, top + Ui::IconButtonSize};
    return {run, edit};
}

bool PointInRect(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

struct DiscoveryMessage {
    std::string instance;
    HWND dialog;
    unsigned generation;
    std::uint16_t port = 0;
    std::vector<std::wstring> names;
    std::wstring error;
    bool success = false;
};

struct ApiMessage { std::string method,path; Json body; std::promise<Json> result; std::atomic_int state{0}; };
using ApiRequestMessage = std::shared_ptr<ApiMessage>;

struct ConnectionMessage {
    bool success = false;
    std::wstring error;
};
struct HostKeyRequest {
    HostKeyChallenge challenge;
    std::promise<bool> decision;
};
using HostKeyMessage = std::shared_ptr<HostKeyRequest>;

struct StatusMessage {
    std::uint64_t sessionId = 0;
    std::wstring text;
    bool isError = false;
};

struct DataMessage {
    std::uint64_t sessionId = 0;
    Bytes data;
};

enum class SftpMessageKind { List, Upload, Download, Mutation };

struct SftpMessage {
    std::uint64_t sessionId = 0;
    SftpMessageKind kind = SftpMessageKind::List;
    bool success = false;
    bool canceled = false;
    std::uint64_t transferId = 0;
    SftpListing listing;
    std::wstring error;
};

struct SftpProgressMessage {
    std::uint64_t transferId = 0;
    SftpTransferProgress progress;
};

struct Palette {
    COLORREF window;
    COLORREF panel;
    COLORREF panelAlt;
    COLORREF terminal;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF accentSoft;
    COLORREF danger;
};

Palette Colors(bool dark) {
    const auto colors = UiTheme(dark);
    return {colors.window, colors.surface, colors.raised, colors.field,
        colors.border, colors.text, colors.muted, colors.accent, colors.accentSoft, colors.danger};
}

std::wstring CurrentTerminalTimestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t value[24]{};
    swprintf_s(value, L"%02u:%02u:%02u.%03u",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return value;
}

COLORREF IndexedTerminalColor(int index, bool dark) {
    static const std::array<COLORREF, 16> DarkColors = {
        RGB(20, 20, 22), RGB(255, 95, 86), RGB(48, 209, 88), RGB(255, 214, 10),
        RGB(64, 156, 255), RGB(191, 90, 242), RGB(100, 210, 255), RGB(229, 229, 234),
        RGB(99, 99, 102), RGB(255, 105, 97), RGB(78, 230, 111), RGB(255, 224, 69),
        RGB(94, 176, 255), RGB(218, 143, 255), RGB(112, 215, 255), RGB(255, 255, 255)};
    static const std::array<COLORREF, 16> LightColors = {
        RGB(29, 29, 31), RGB(198, 40, 40), RGB(24, 128, 56), RGB(151, 112, 0),
        RGB(0, 102, 204), RGB(137, 49, 178), RGB(0, 125, 140), RGB(205, 205, 210),
        RGB(99, 99, 102), RGB(215, 54, 54), RGB(35, 150, 70), RGB(176, 128, 0),
        RGB(0, 122, 255), RGB(175, 82, 222), RGB(0, 145, 165), RGB(255, 255, 255)};
    index = std::max(0, std::min(255, index));
    if (index < 16) return dark ? DarkColors[static_cast<size_t>(index)] : LightColors[static_cast<size_t>(index)];
    if (index < 232) {
        const int value = index - 16;
        const int blue = value % 6;
        const int green = value / 6 % 6;
        const int red = value / 36;
        const auto component = [](int level) { return level == 0 ? 0 : 55 + level * 40; };
        return RGB(component(red), component(green), component(blue));
    }
    const int gray = 8 + (index - 232) * 10;
    return RGB(gray, gray, gray);
}

COLORREF ResolveTerminalColor(
    const TerminalColor& color, COLORREF fallback, bool dark, bool bold) {
    if (color.kind == TerminalColor::Kind::Rgb)
        return RGB(color.red, color.green, color.blue);
    if (color.kind == TerminalColor::Kind::Indexed) {
        int index = color.index;
        if (bold && index < 8) index += 8;
        return IndexedTerminalColor(index, dark);
    }
    return fallback;
}

struct TerminalScrollMetrics {
    bool visible = false;
    RECT track{};
    RECT thumb{};
    int maximumOffset = 0;
};

TerminalScrollMetrics GetTerminalScrollMetrics(
    HWND terminal, size_t totalLines, int visibleRows, int scrollOffset) {
    TerminalScrollMetrics metrics;
    RECT client{};
    GetClientRect(terminal, &client);
    visibleRows = std::max(1, visibleRows);
    metrics.maximumOffset = std::max(0, static_cast<int>(totalLines) - visibleRows);
    if (metrics.maximumOffset <= 0) return metrics;
    metrics.visible = true;
    metrics.track = {client.right - Ui::OverlayScrollLaneWidth, 8, client.right, client.bottom - 8};
    const int trackHeight = std::max(1,
        static_cast<int>(metrics.track.bottom - metrics.track.top));
    const int thumbHeight = std::max(28,
        trackHeight * std::min<int>(visibleRows, static_cast<int>(totalLines)) /
            std::max<int>(1, static_cast<int>(totalLines)));
    const int travel = std::max(1, trackHeight - thumbHeight);
    // scrollOffset is measured upward from the newest line. The thumb is therefore
    // at the bottom for offset zero and moves toward the top as the offset grows.
    const int thumbTop = metrics.track.top + travel -
        scrollOffset * travel / std::max(1, metrics.maximumOffset);
    const int left = client.right - (Ui::OverlayScrollLaneWidth + Ui::OverlayScrollThumbWidth) / 2;
    metrics.thumb = {left, thumbTop, left + Ui::OverlayScrollThumbWidth, thumbTop + thumbHeight};
    return metrics;
}

struct TerminalHorizontalScrollMetrics {
    bool visible = false;
    RECT track{};
    RECT thumb{};
    int maximumOffset = 0;
};

TerminalHorizontalScrollMetrics GetTerminalHorizontalScrollMetrics(
    HWND terminal, int textLeft, size_t totalColumns, int visibleColumns, int scrollOffset) {
    TerminalHorizontalScrollMetrics metrics;
    RECT client{};
    GetClientRect(terminal, &client);
    visibleColumns = std::max(1, visibleColumns);
    metrics.maximumOffset = std::max(0,
        static_cast<int>(totalColumns) - visibleColumns);
    if (metrics.maximumOffset <= 0) return metrics;
    metrics.visible = true;
    metrics.track = {textLeft, client.bottom - Ui::OverlayScrollLaneWidth,
        client.right - Ui::OverlayScrollLaneWidth, client.bottom};
    const int trackWidth = std::max(1,
        static_cast<int>(metrics.track.right - metrics.track.left));
    const int thumbWidth = std::max(28,
        trackWidth * std::min<int>(visibleColumns, static_cast<int>(totalColumns)) /
            std::max<int>(1, static_cast<int>(totalColumns)));
    const int travel = std::max(1, trackWidth - thumbWidth);
    const int thumbLeft = metrics.track.left +
        scrollOffset * travel / std::max(1, metrics.maximumOffset);
    const int top = client.bottom -
        (Ui::OverlayScrollLaneWidth + Ui::OverlayScrollThumbWidth) / 2;
    metrics.thumb = {thumbLeft, top, thumbLeft + thumbWidth,
        top + Ui::OverlayScrollThumbWidth};
    return metrics;
}

void DrawRoundedBox(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    UiBox(dc, rect, fill, border, radius);
}

RECT TerminalNewOutputRect(const RECT& client, bool horizontalScroll) {
    const LONG bottom = client.bottom - (horizontalScroll ? Ui::OverlayScrollLaneWidth + Ui::Scale(4) : Ui::Space);
    return {client.right - Ui::Scale(126), bottom - Ui::Scale(30),
        client.right - Ui::OverlayScrollLaneWidth - Ui::Space, bottom};
}

void ApplyDarkTitleBar(HWND window, bool dark) {
    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (!module) return;
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(module, "DwmSetWindowAttribute"));
    if (setAttribute) {
        const BOOL enabled = dark ? TRUE : FALSE;
        if (FAILED(setAttribute(window, 20, &enabled, sizeof(enabled))))
            setAttribute(window, 19, &enabled, sizeof(enabled));
        const COLORREF caption = dark ? RGB(24, 28, 33) : RGB(255, 255, 255);
        const COLORREF captionText = dark ? RGB(236, 240, 244) : RGB(28, 35, 43);
        const COLORREF border = dark ? RGB(54, 61, 70) : RGB(217, 224, 231);
        setAttribute(window, 35, &caption, sizeof(caption));
        setAttribute(window, 36, &captionText, sizeof(captionText));
        setAttribute(window, 34, &border, sizeof(border));
    }
    FreeLibrary(module);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void CenterOnOwner(HWND window, HWND owner) {
    RECT ownerRect{};
    RECT windowRect{};
    if (!owner || !GetWindowRect(owner, &ownerRect) || !GetWindowRect(window, &windowRect)) return;
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
        const int workTop = static_cast<int>(monitorInfo.rcWork.top);
        const int workRight = static_cast<int>(monitorInfo.rcWork.right);
        const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
        x = std::max(workLeft, std::min(x, workRight - width));
        y = std::max(workTop, std::min(y, workBottom - height));
    }
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

void DrawChevronDown(HDC dc, const RECT& rect, COLORREF color) {
    const int size = Ui::Scale(16);
    const int x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {x, y, x + size, y + size}, color, UiGlyph::ChevronDown);
}

void DrawChevronHorizontal(HDC dc, const RECT& rect, COLORREF color, bool right) {
    const int size = Ui::Scale(16), x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {x, y, x + size, y + size}, color, right ? UiGlyph::ChevronRight : UiGlyph::ChevronLeft);
}

void DrawSortChevron(HDC dc, const RECT& rect, COLORREF color, bool ascending) {
    const int size = Ui::Scale(12), y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {rect.right - Ui::Scale(20), y, rect.right - Ui::Scale(20) + size, y + size}, color,
                ascending ? UiGlyph::ChevronUp : UiGlyph::ChevronDown);
}

void DrawSftpEntryIcon(HDC dc, const RECT& rect, COLORREF color, bool directory, bool symlink) {
    const int size = Ui::Scale(18), x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {x, y, x + size, y + size}, color,
                symlink     ? UiGlyph::Link
                : directory ? UiGlyph::Folder
                            : UiGlyph::Document);
}

void DrawPlusIcon(HDC dc, const RECT& rect, COLORREF color) {
    const int size = Ui::Scale(16);
    const int x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {x, y, x + size, y + size}, color, UiGlyph::Plus);
}

void DrawEditIcon(HDC dc, const RECT& rect, COLORREF color) {
    const int size = Ui::Scale(16);
    const int x = (rect.left + rect.right - size) / 2, y = (rect.top + rect.bottom - size) / 2;
    UiDrawGlyph(dc, {x, y, x + size, y + size}, color, UiGlyph::Edit);
}

void DrawStatusDot(HDC dc, const RECT& rect, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush(UiColor(color));
    graphics.FillEllipse(&brush, Gdiplus::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)));
}

void DrawCommandProgress(HDC dc, const RECT& rect, size_t completed, size_t total,
    COLORREF trackColor, COLORREF progressColor) {
    RECT ring = rect;
    InflateRect(&ring, -Ui::Scale(2), -Ui::Scale(2));
    const float inset = static_cast<float>(Ui::Scale(1));
    const Gdiplus::RectF bounds(ring.left + inset, ring.top + inset,
        ring.right - ring.left - 2 * inset, ring.bottom - ring.top - 2 * inset);
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen track(UiColor(trackColor), 1.0f);
    graphics.DrawEllipse(&track, bounds);
    if (total > 0 && completed > 0) {
        Gdiplus::Pen progress(UiColor(progressColor), static_cast<float>(Ui::Scale(2)));
        progress.SetStartCap(Gdiplus::LineCapRound);
        progress.SetEndCap(Gdiplus::LineCapRound);
        graphics.DrawArc(&progress, bounds, -90.0f,
            360.0f * static_cast<float>(std::min(1.0, static_cast<double>(completed) / total)));
    }
}

void DrawTextSimple(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, oldFont);
}

std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

DWORD ParsePositive(const std::wstring& text, DWORD fallback = 0) {
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    return end && *end == L'\0' && value > 0 ? static_cast<DWORD>(value) : fallback;
}

bool ParseMacroHeader(const std::wstring& line, std::uint32_t& intervalMs) {
    constexpr wchar_t Prefix[] = L"[[macro interval=";
    constexpr wchar_t Suffix[] = L"]]";
    const size_t prefixLength = std::size(Prefix) - 1;
    const size_t suffixLength = std::size(Suffix) - 1;
    if (line.size() <= prefixLength + suffixLength || line.rfind(Prefix, 0) != 0 ||
        line.compare(line.size() - suffixLength, suffixLength, Suffix) != 0)
        return false;
    const std::wstring value = line.substr(prefixLength,
        line.size() - prefixLength - suffixLength);
    const DWORD parsed = ParsePositive(value);
    if (parsed == 0 || parsed > MaximumCommandIntervalMs) return false;
    intervalMs = parsed;
    return true;
}

std::wstring EscapeSingleCommandLine(std::wstring line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    const std::wstring content = firstContent == std::wstring::npos ? std::wstring() : line.substr(firstContent);
    if (content.rfind(L"#", 0) == 0 || content.rfind(L"[[", 0) == 0)
        line.insert(line.begin(), L'\\');
    return line;
}

std::wstring EscapeMacroCommandLine(std::wstring line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent != std::wstring::npos && line.substr(firstContent) == L"[[/macro]]")
        line.insert(line.begin(), L'\\');
    return line;
}

bool UnescapeSingleCommandLine(std::wstring& line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent == 0 || firstContent == std::wstring::npos) return false;
    const std::wstring content = line.substr(firstContent);
    if (content.rfind(L"#", 0) != 0 && content.rfind(L"[[", 0) != 0) return false;
    line.erase(0, 1);
    return true;
}

void UnescapeMacroCommandLine(std::wstring& line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent != 0 && firstContent != std::wstring::npos &&
        line.substr(firstContent) == L"[[/macro]]")
        line.erase(0, 1);
}

void AddComboItems(HWND combo, std::initializer_list<const wchar_t*> items, int selection) {
    for (const wchar_t* item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(combo, CB_SETCURSEL, selection, 0);
}

int CALLBACK FindUiFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

const wchar_t* UiFontFamily() {
    HDC dc = GetDC(nullptr);
    const wchar_t* selected = L"Segoe UI";
    for (const wchar_t* candidate : {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimSun"}) {
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(query.lfFaceName, candidate, LF_FACESIZE);
        bool found = false;
        EnumFontFamiliesExW(dc, &query, FindUiFont, reinterpret_cast<LPARAM>(&found), 0);
        if (found) { selected = candidate; break; }
    }
    ReleaseDC(nullptr, dc);
    return selected;
}

int DialogLabelHeight(HWND control) {
    HDC dc = GetDC(control);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int length = GetWindowTextW(control, &text[0], static_cast<int>(text.size()));
    SIZE extent{};
    if (length) GetTextExtentPoint32W(dc, text.c_str(), length, &extent);
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
    return std::max(static_cast<int>(metrics.tmHeight), static_cast<int>(extent.cy));
}

void FitDialogLabels(HWND dialog) {
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[32]{};
        GetClassNameW(child, name, static_cast<int>(std::size(name)));
        if (_wcsicmp(name, L"Static") != 0 || GetDlgCtrlID(child) == 0) continue;
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&rect), 2);
        const int height = std::max(static_cast<int>(rect.bottom - rect.top), DialogLabelHeight(child));
        SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void MoveDialogItemDlu(HWND dialog, int id, int x, int y, int width, int height) {
    RECT rect{x, y, x + width, y + height};
    MapDialogRect(dialog, &rect);
    HWND control = GetDlgItem(dialog, id);
    if (!control) return;
    HWND frame = reinterpret_cast<HWND>(GetPropW(control, Ui::FieldFrameProperty));
    const int pixelWidth = rect.right - rect.left;
    // Compact single-line fields have a logical height independent of the
    // resource template font's dialog-unit conversion on the current system.
    const int pixelHeight = frame && height <= 14 ? Ui::Scale(28)
                            : frame               ? std::max(Ui::Scale(28), static_cast<int>(rect.bottom - rect.top))
                                                  : static_cast<int>(rect.bottom - rect.top);
    if (frame) {
        MoveWindow(frame, rect.left, rect.top, pixelWidth, pixelHeight, TRUE);
        const int horizontalInset = Ui::Space;
        const int verticalInset = Ui::Scale(4);
        MoveWindow(control, rect.left + horizontalInset, rect.top + verticalInset,
            std::max(1, pixelWidth - horizontalInset * 2),
            std::max(1, pixelHeight - verticalInset * 2), TRUE);
        SetWindowPos(frame, control, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        MoveWindow(control, rect.left, rect.top, pixelWidth, pixelHeight, TRUE);
    }
}

void ShowDialogItem(HWND dialog, int id, bool show) {
    HWND control = GetDlgItem(dialog, id);
    if (!control) return;
    const int command = show ? SW_SHOW : SW_HIDE;
    HWND frame = reinterpret_cast<HWND>(GetPropW(control, Ui::FieldFrameProperty));
    if (frame) ShowWindow(frame, command);
    ShowWindow(control, command);
}

void MatchDialogControlHeight(HWND dialog, int referenceId, int targetId) {
    HWND reference = GetDlgItem(dialog, referenceId);
    if (const auto frame = reinterpret_cast<HWND>(GetPropW(reference, Ui::FieldFrameProperty)))
        reference = frame;
    HWND target = GetDlgItem(dialog, targetId);
    if (!reference || !target) return;
    RECT referenceRect{};
    RECT targetRect{};
    GetWindowRect(reference, &referenceRect);
    GetWindowRect(target, &targetRect);
    MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&referenceRect), 2);
    MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&targetRect), 2);
    SetWindowPos(target, nullptr, targetRect.left, referenceRect.top,
        targetRect.right - targetRect.left, referenceRect.bottom - referenceRect.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

struct OverlayScrollMetrics {
    RECT track{};
    RECT thumb{};
    int topIndex = 0;
    int pageItems = 1;
    int maxTopIndex = 0;
};

bool GetOverlayScrollMetrics(HWND list, OverlayScrollMetrics& metrics) {
    const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (count <= 0) return false;
    RECT client{};
    GetClientRect(list, &client);
    const int itemHeight = std::max(1, static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
    metrics.pageItems = std::max(1, static_cast<int>(client.bottom - client.top) / itemHeight);
    if (count <= metrics.pageItems) return false;
    metrics.topIndex = std::max(0, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
    metrics.maxTopIndex = std::max(1, count - metrics.pageItems);
    metrics.topIndex = std::min(metrics.topIndex, metrics.maxTopIndex);
    const int thumbRight = client.right - 5;
    metrics.track = {thumbRight - Ui::OverlayScrollThumbWidth, client.top + 8,
        thumbRight, client.bottom - 8};
    const int trackHeight = std::max(1, static_cast<int>(metrics.track.bottom - metrics.track.top));
    const int thumbHeight = std::min(trackHeight,
        std::max(24, trackHeight * metrics.pageItems / count));
    const int travel = std::max(0, trackHeight - thumbHeight);
    const int thumbTop = metrics.track.top +
        (travel == 0 ? 0 : travel * metrics.topIndex / metrics.maxTopIndex);
    metrics.thumb = {metrics.track.left, thumbTop, metrics.track.right, thumbTop + thumbHeight};
    return true;
}

void DrawOverlayScrollbar(HWND list, bool darkMode) {
    OverlayScrollMetrics metrics;
    if (!GetOverlayScrollMetrics(list, metrics)) return;

    RECT client{};
    GetClientRect(list, &client);
    HDC dc = GetDC(list);
    if (!dc) return;

    wchar_t className[32]{};
    GetClassNameW(list, className, static_cast<int>(std::size(className)));
    const bool comboDropList = _wcsicmp(className, L"ComboLBox") == 0;
    const Palette colors = Colors(darkMode);
    RECT lane{std::max(client.left, client.right - Ui::OverlayScrollLaneWidth),
        client.top, client.right, client.bottom};
    HBRUSH laneBrush = CreateSolidBrush(comboDropList ? colors.terminal : colors.panel);
    FillRect(dc, &lane, laneBrush);
    DeleteObject(laneBrush);
    DrawRoundedBox(dc, metrics.thumb, 2, colors.border, colors.border);
    ReleaseDC(list, dc);
}

void ResizeDialogClientDlu(HWND dialog, int width, int height) {
    RECT rect{0, 0, width, height};
    MapDialogRect(dialog, &rect);
    AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring LocalFileNameOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring JoinLocalPath(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    if (directory.back() == L'\\' || directory.back() == L'/') return directory + name;
    return directory + L"\\" + name;
}

bool ExistingLocalPathsReferToSameFile(
    const std::wstring& left, const std::wstring& right) {
    if (CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;
    HANDLE leftFile = CreateFileW(left.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (leftFile == INVALID_HANDLE_VALUE) return false;
    HANDLE rightFile = CreateFileW(right.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (rightFile == INVALID_HANDLE_VALUE) {
        CloseHandle(leftFile);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION leftInfo{};
    BY_HANDLE_FILE_INFORMATION rightInfo{};
    const bool same = GetFileInformationByHandle(leftFile, &leftInfo) &&
        GetFileInformationByHandle(rightFile, &rightInfo) &&
        leftInfo.dwVolumeSerialNumber == rightInfo.dwVolumeSerialNumber &&
        leftInfo.nFileIndexHigh == rightInfo.nFileIndexHigh &&
        leftInfo.nFileIndexLow == rightInfo.nFileIndexLow;
    CloseHandle(rightFile);
    CloseHandle(leftFile);
    return same;
}

bool IsSafeAutomaticLocalFileName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t character : name) {
        if (character < 32 || wcschr(L"<>:\"/\\|?*", character)) return false;
    }
    std::wstring base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towupper(character));
    });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return false;
    if (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') return false;
    return true;
}

std::uint64_t LocalFileSize(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    const bool success = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    CloseHandle(file);
    return success ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}

std::wstring FileSizeText(std::uint64_t size) {
    wchar_t value[32]{};
    if (size >= 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(value, L"%.1f GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
        return value;
    }
    if (size >= 1024ULL * 1024ULL) {
        swprintf_s(value, L"%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
        return value;
    }
    if (size >= 1024ULL) {
        swprintf_s(value, L"%.1f KB", static_cast<double>(size) / 1024.0);
        return value;
    }
    return std::to_wstring(size) + L" B";
}

std::wstring QuoteShellArgument(const std::wstring& value) {
    std::wstring quoted = L"'";
    for (wchar_t character : value) {
        if (character == L'\'') quoted += L"'\\''";
        else quoted.push_back(character);
    }
    quoted += L"'";
    return quoted;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool success = false;
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            success = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            if (!success) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return success;
}

std::wstring TransferRateText(std::uint64_t bytesPerSecond, std::uint32_t remainingSeconds) {
    if (bytesPerSecond == 0) return {};
    std::wstring value = FileSizeText(bytesPerSecond) + L"/s";
    if (remainingSeconds > 0) {
        const std::uint32_t minutes = remainingSeconds / 60;
        const std::uint32_t seconds = remainingSeconds % 60;
        value += L" · ";
        if (minutes > 0) value += std::to_wstring(minutes) + L" 分 ";
        value += std::to_wstring(seconds) + L" 秒";
    }
    return value;
}
} // namespace

bool MainWindow::Create(HINSTANCE instance, int showCommand) {
    instance_ = instance;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = WindowClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    HDC desktop = GetDC(nullptr); int dpi=GetDeviceCaps(desktop,LOGPIXELSX);ReleaseDC(nullptr,desktop);
    int width = std::min(MulDiv(1180,dpi,96),static_cast<int>(work.right-work.left));
    int height = std::min(MulDiv(760,dpi,96),static_cast<int>(work.bottom-work.top));
    window_ = CreateWindowExW(0, WindowClassName, L"SerialCtl", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,
        nullptr,nullptr,instance,this);
    if (!window_) return false;
    ApplyDarkTitleBar(window_, darkMode_);
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

MainWindow::~MainWindow() {
    aiPane_.Shutdown();
    if (logExportJob_) logExportJob_->cancel.store(true);
    StopStartupUpdate();
    powerService_.Shutdown();
    StopLogExport();
}

void MainWindow::StartStartupUpdate() {
    if (updateThread_.joinable() || closing_.load()) return;
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return;
    const std::filesystem::path executable(std::wstring(path.data(), size));
    updateCancel_.store(false);
    try {
        const auto approved = updateApprovedVersion_;
        updateThread_ = std::thread([this, executable, approved] {
            update::UpdateCallbacks callbacks;
            callbacks.confirm = [this, approved](const std::string &version) {
                if (version == approved && !approved.empty())
                    return true;
                auto prompt = std::make_unique<UpdatePrompt>();
                prompt->version = version;
                auto future = prompt->answer.get_future();
                if (!PostMessageW(window_, MessageUpdatePrompt, 0, reinterpret_cast<LPARAM>(prompt.get())))
                    return false;
                prompt.release();
                while (!updateCancel_.load()) {
                    if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
                        return future.get();
                }
                return false;
            };
            callbacks.progress = [this](std::uint64_t received, std::uint64_t total, update::UpdatePhase phase) {
                updateReceived_ = received;
                updateTotal_ = total;
                updatePhase_ = phase == update::UpdatePhase::Download ? 1 : 2;
            };
            auto result = std::make_unique<update::UpdateResult>(
                update::CheckAndStage(executable, SERIALCTL_APP_VERSION, updateCancel_, callbacks));
            if (!updateCancel_.load() &&
                PostMessageW(window_, MessageUpdate, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
            else
                DiscardUpdate(*result);
        });
    } catch (const std::exception&) {
        AppendStatus(L"无法启动自动更新检查，继续使用当前版本。", true);
    }
}

INT_PTR CALLBACK MainWindow::UpdateDialogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(h, DWLP_USER));
    if (m == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow *>(l);
        SetWindowLongPtrW(h, DWLP_USER, l);
        self->updateDialog_ = h;
        SetWindowTextW(h, L"软件更新");
        ApplyDarkTitleBar(h, self->darkMode_);
        UiResizeDialog(h, Ui::Dpi, 460, self->updateConfirming_ ? 244 : 188);
        auto add = [&](const wchar_t *cls, const std::wstring &text, int id, int x, int y, int width, int height,
                       DWORD style = 0) {
            HWND child = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, Ui::Scale(x),
                                         Ui::Scale(y), Ui::Scale(width), Ui::Scale(height), h,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self->instance_, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont_), FALSE);
            if (style & BS_OWNERDRAW)
                SetWindowSubclass(child, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
            return child;
        };
        if (self->updateConfirming_) {
            const auto version = std::wstring(self->updatePromptVersion_.begin(), self->updatePromptVersion_.end());
            add(L"STATIC", L"发现新版本 " + version, -1, 16, 16, 428, 28);
            add(L"STATIC", L"更新后才能继续使用。点击确定开始下载，完成后自动重启。", -1, 16, 56, 428, 56);
            add(L"STATIC", L"连接配置、命令、日志和驱动将保留。", -1, 16, 124, 428, 24);
            add(L"STATIC", L"关闭此窗口将退出 SerialCtl。", -1, 16, 184, 304, 36);
            auto ok = add(L"BUTTON", L"确定", IDOK, 348, 184, 96, 36, BS_OWNERDRAW | WS_TABSTOP);
            SendMessageW(h, DM_SETDEFID, IDOK, 0);
            SetFocus(ok);
        } else {
            add(L"STATIC", L"准备下载更新…", 100, 16, 16, 428, 28);
            add(L"STATIC", L"", 101, 16, 56, 428, 24);
            add(L"STATIC", L"下载 → 校验 → 安装并重启", -1, 16, 128, 428, 28);
            SetTimer(h, 1, 100, nullptr);
        }
        CenterOnOwner(h, self->window_);
        return FALSE;
    }
    if (!self)
        return FALSE;
    if (m == WM_TIMER) {
        const auto received = self->updateReceived_.load(), total = self->updateTotal_.load();
        const int phase = self->updatePhase_.load();
        SetDlgItemTextW(h, 100, phase == 2 ? L"正在校验更新包…" : phase == 1 ? L"正在下载更新…" : L"准备更新…");
        const auto detail = phase == 1 && total
                                ? std::to_wstring(received / 1024) + L" / " + std::to_wstring(total / 1024) +
                                      L" KiB · " +
                                      std::to_wstring(std::min<std::uint64_t>(100, received * 100 / total)) + L"%"
                                : L"请等待，完成后将自动重启。";
        SetDlgItemTextW(h, 101, detail.c_str());
        InvalidateRect(h, nullptr, FALSE);
        return TRUE;
    }
    if (m == WM_PAINT || m == WM_ERASEBKGND) {
        PAINTSTRUCT paint{};
        HDC dc = m == WM_PAINT ? BeginPaint(h, &paint) : reinterpret_cast<HDC>(w);
        RECT rect{};
        GetClientRect(h, &rect);
        FillRect(dc, &rect, self->panelBrush_);
        const auto c = UiTheme(self->darkMode_);
        if (!self->updateConfirming_) {
            RECT bar{Ui::Scale(16), Ui::Scale(96), Ui::Scale(444), Ui::Scale(104)};
            UiBox(dc, bar, c.raised, c.raised, Ui::Scale(4));
            const auto total = self->updateTotal_.load(), received = self->updateReceived_.load();
            if (total && received) {
                bar.right = bar.left + static_cast<LONG>((bar.right - bar.left) * std::min(received, total) / total);
                UiBox(dc, bar, c.accent, c.accent, Ui::Scale(4));
            }
        }
        if (m == WM_PAINT)
            EndPaint(h, &paint);
        return TRUE;
    }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORDLG) {
        const auto c = UiTheme(self->darkMode_);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        SetBkColor(reinterpret_cast<HDC>(w), c.surface);
        return reinterpret_cast<INT_PTR>(self->panelBrush_);
    }
    if (m == WM_DRAWITEM) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(w) == IDOK && self->updateConfirming_) {
        UiEndOwnedDialog(h, IDOK);
        return TRUE;
    }
    if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(w) == IDCANCEL)) {
        UiEndOwnedDialog(h, IDCANCEL);
        if (!self->updateConfirming_) {
            self->updateCancel_ = true;
            PostMessageW(self->window_, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, 1);
        if (self->updateDialog_ == h)
            self->updateDialog_ = nullptr;
    }
    return FALSE;
}

void MainWindow::StopStartupUpdate() {
    updateCancel_.store(true);
    if (updateThread_.joinable()) updateThread_.join();
    MSG pending{};
    while (window_ && PeekMessageW(&pending, window_, MessageUpdatePrompt, MessageUpdatePrompt, PM_REMOVE)) {
        std::unique_ptr<UpdatePrompt> prompt(reinterpret_cast<UpdatePrompt *>(pending.lParam));
        if (prompt)
            prompt->answer.set_value(false);
    }
    while (window_ && PeekMessageW(&pending, window_, MessageUpdate, MessageUpdate, PM_REMOVE)) {
        std::unique_ptr<update::UpdateResult> result(reinterpret_cast<update::UpdateResult*>(pending.lParam));
        if (result) DiscardUpdate(*result);
    }
}

bool MainWindow::CanInstallStartupUpdate(bool confirmed) const {
    // PowerPageOpened also excludes a queued connect/task that has not yet
    // appeared in the service snapshot. A modal owner and mouse capture exclude
    // edits and ongoing gestures. Explicit consent may follow harmless UI use;
    // actual device work is still protected and defers installation.
    return !closing_.load() && (!startupUpdateDeferred_ || confirmed) && !logExportJob_ && sessions_.empty() &&
           !pendingSession_ && !powerPageOpened_ && !powerVisible_ && !aiPane_.Busy() && !aiPageOpened_ &&
           !disconnectAllPending_ && !disconnectAllInProgress_ && !commandsDirty_ && runningCommandIndex_ < 0 &&
           !sftpBusy_ && !activeSftpTransferId_ && !windowResizing_ && !rightPanelDragging_ && !rightPanelAnimating_ &&
           draggingCommandIndex_ < 0 && !sftpColumnDragging_ && !GetCapture() &&
           (IsWindowEnabled(window_) || updateRequired_);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::TerminalSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_GETDLGCODE) {
        return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTALLKEYS;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        self->PaintTerminal(dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_RBUTTONUP) {
        SetFocus(window);
        self->PasteToTerminal();
        // Consume the mouse release: DefWindowProc would generate a second
        // WM_CONTEXTMENU. Mouse context messages never submit another paste.
        return 0;
    } else if (message == WM_KEYUP && (wParam == VK_APPS || wParam == VK_F10)) {
        // The context-menu key was handled on key-down. Its native key-up
        // must not reopen the modal after the user has just dismissed it.
        return 0;
    } else if (message == WM_CONTEXTMENU) {
        if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1)
            self->ShowSettings();
        return 0;
    } else if (message == WM_COPY) {
        self->CopyTerminalSelection(false);
        return 0;
    }
    if (message == WM_CHAR) {
        self->SendTerminalCharacter(static_cast<wchar_t>(wParam));
        return 0;
    }
    if (message == WM_SYSCHAR) {
        if (self->activeSession_ && self->connection_ && self->connection_->IsConnected()) {
            const wchar_t character = static_cast<wchar_t>(wParam);
            const std::wstring text = std::wstring(1, 0x1b) + character;
            const std::string encoded = WideToMultiByte(text, self->SelectedCodePage());
            self->SendBytesToActive(Bytes(encoded.begin(), encoded.end()), false);
        }
        return 0;
    }
    if (message == WM_PASTE) {
        self->PasteToTerminal();
        return 0;
    }
    if (message == WM_KEYDOWN) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (wParam == VK_APPS || (shift && wParam == VK_F10)) {
            if (!(lParam & (1LL << 30))) self->ShowSettings();
            return 0;
        }
        if (control && shift && wParam == L'C') {
            self->CopyTerminalSelection(false);
            return 0;
        }
        if ((control && shift && wParam == L'V') || (shift && wParam == VK_INSERT)) {
            self->PasteToTerminal();
            return 0;
        }
        if (control && wParam == L'C' && self->HasTerminalSelection()) {
            self->CopyTerminalSelection(false);
            return 0;
        }
        const bool localHistory = !self->activeSession_ || !self->activeSession_->terminal.AlternateScreen();
        if (shift && localHistory && wParam == VK_PRIOR) {
            self->ScrollTerminal(std::max(1, self->terminalVisibleRows_ - 1));
            return 0;
        }
        if (shift && localHistory && wParam == VK_NEXT) {
            self->ScrollTerminal(-std::max(1, self->terminalVisibleRows_ - 1));
            return 0;
        }
        if (shift && localHistory && wParam == VK_HOME) {
            if (self->activeSession_) {
                const int maximum = std::max(0,
                    static_cast<int>(self->activeSession_->terminal.DisplayLineCount()) -
                        self->terminalVisibleRows_);
                self->activeSession_->terminalScrollOffset = maximum;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (shift && localHistory && wParam == VK_END) {
            self->ScrollTerminalToBottom();
            return 0;
        }
        if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT ||
            wParam == VK_HOME || wParam == VK_END || wParam == VK_INSERT ||
            wParam == VK_DELETE || wParam == VK_PRIOR || wParam == VK_NEXT ||
            wParam == VK_ESCAPE || (wParam >= VK_F1 && wParam <= VK_F12) ||
            (wParam == VK_TAB && shift)) {
            self->SendTerminalKey(wParam, shift, control, alt);
            return 0;
        }
    }
    if (message == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const UINT keys = GET_KEYSTATE_WPARAM(wParam);
        if (self->activeSession_ && self->selectedMode_ != 4) {
            auto &model = self->activeSession_->terminal;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const int column = std::clamp<int>((point.x - Ui::TerminalPadding) / std::max(1, self->terminalCellWidth_), 0, model.Columns() - 1);
            const int row = std::clamp<int>((point.y - Ui::TerminalPadding) / std::max(1, self->terminalLineHeight_), 0, model.Rows() - 1);
            const unsigned modifiers = ((keys & MK_SHIFT) ? 1u : 0u) | ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) |
                ((keys & MK_CONTROL) ? 4u : 0u);
            const auto sequence = model.WheelSequence(delta > 0, column, row, modifiers);
            if (!sequence.empty()) {
                for (int count = std::clamp(std::abs(delta) / WHEEL_DELTA, 1, 8); count > 0; --count)
                    self->SendBytesToActive(Bytes(sequence.begin(), sequence.end()), false);
                return 0;
            }
        }
        if ((keys & MK_CONTROL) != 0) {
            self->terminalZoomWheelRemainder_ += delta;
            const int steps = self->terminalZoomWheelRemainder_ / WHEEL_DELTA;
            self->terminalZoomWheelRemainder_ %= WHEEL_DELTA;
            if (steps != 0) self->ZoomTerminalFont(steps);
        } else {
            self->terminalWheelRemainder_ += delta;
            const int steps = self->terminalWheelRemainder_ / WHEEL_DELTA;
            self->terminalWheelRemainder_ %= WHEEL_DELTA;
            if (steps != 0) self->ScrollTerminal(steps * 3);
        }
        return 0;
    }
    if (message == WM_MOUSEHWHEEL) {
        self->terminalHorizontalWheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = self->terminalHorizontalWheelRemainder_ / WHEEL_DELTA;
        self->terminalHorizontalWheelRemainder_ %= WHEEL_DELTA;
        if (steps != 0) self->ScrollTerminalHorizontal(steps * 4);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        SetFocus(window);
        if (!self->activeSession_) return 0;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const TerminalModel& model = self->activeSession_->terminal;
        const int timestampWidth =
            self->timestampEnabled_ && !self->LiveMonitorPage() && !model.AlternateScreen() ? Ui::TerminalTimestampWidth : 0;
        const TerminalHorizontalScrollMetrics horizontalMetrics =
            GetTerminalHorizontalScrollMetrics(window, Ui::TerminalPadding + timestampWidth,
                model.MaximumDisplayColumns(), self->terminalVisibleColumns_,
                self->activeSession_->terminalHorizontalOffset);
        if (horizontalMetrics.visible && point.y >= horizontalMetrics.track.top &&
            point.x >= horizontalMetrics.track.left && point.x < horizontalMetrics.track.right) {
            if (PointInRect(horizontalMetrics.thumb, point)) {
                self->terminalHorizontalScrollbarDragging_ = true;
                self->terminalHorizontalScrollbarThumbOffset_ = point.x - horizontalMetrics.thumb.left;
                SetCapture(window);
            } else {
                const int direction = point.x < horizontalMetrics.thumb.left ? -1 : 1;
                self->ScrollTerminalHorizontal(
                    direction * std::max(1, self->terminalVisibleColumns_ - 1));
            }
            return 0;
        }
        const TerminalScrollMetrics metrics = GetTerminalScrollMetrics(window,
            self->activeSession_->terminal.DisplayLineCount(), self->terminalVisibleRows_,
            self->activeSession_->terminalScrollOffset);
        if (metrics.visible && point.x >= metrics.track.left) {
            if (PointInRect(metrics.thumb, point)) {
                self->terminalScrollbarDragging_ = true;
                self->terminalScrollbarThumbOffset_ = point.y - metrics.thumb.top;
                SetCapture(window);
            } else {
                const int direction = point.y < metrics.thumb.top ? 1 : -1;
                self->ScrollTerminal(direction * std::max(1, self->terminalVisibleRows_ - 1));
            }
            return 0;
        }
        RECT client{};
        GetClientRect(window, &client);
        if (self->activeSession_->terminalHasNewOutput && self->activeSession_->terminalScrollOffset > 0 &&
            PointInRect(TerminalNewOutputRect(client, horizontalMetrics.visible), point)) {
            self->ScrollTerminalToBottom();
            return 0;
        }
        size_t line = 0;
        int column = 0;
        if (self->TerminalPointToCell(point, line, column)) {
            self->UpdateTerminalSelection(line, column, false);
            self->terminalSelecting_ = true;
            SetCapture(window);
        }
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK && self->activeSession_) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        size_t lineIndex = 0;
        int column = 0;
        if (self->TerminalPointToCell(point, lineIndex, column)) {
            const TerminalLine& line = self->activeSession_->terminal.DisplayLine(lineIndex);
            const int columns = static_cast<int>(line.cells.size());
            if (columns <= 0) return 0;
            column = std::max(0, std::min(columns - 1, column));
            const auto wordCharacter = [](wchar_t character) {
                return character && (iswalnum(character) || wcschr(L"_./~:+-%@", character) != nullptr);
            };
            int first = column;
            int last = column;
            while (first > 0 && wordCharacter(line.cells[static_cast<size_t>(first - 1)].character)) --first;
            while (last + 1 < columns && wordCharacter(line.cells[static_cast<size_t>(last + 1)].character)) ++last;
            self->terminalSelectionAnchorLine_ = lineIndex;
            self->terminalSelectionAnchorColumn_ = first;
            self->terminalSelectionFocusLine_ = lineIndex;
            self->terminalSelectionFocusColumn_ = last;
            self->terminalSelectionActive_ = true;
            self->terminalSelecting_ = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            self->CopyTerminalSelection(false);
        }
        return 0;
    }
    if (message == WM_MOUSEMOVE && self->activeSession_) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (self->terminalHorizontalScrollbarDragging_) {
            const TerminalModel& model = self->activeSession_->terminal;
            const int timestampWidth = self->timestampEnabled_ && !self->LiveMonitorPage() && !model.AlternateScreen()
                                           ? Ui::TerminalTimestampWidth
                                           : 0;
            const TerminalHorizontalScrollMetrics metrics =
                GetTerminalHorizontalScrollMetrics(window, Ui::TerminalPadding + timestampWidth,
                    model.MaximumDisplayColumns(), self->terminalVisibleColumns_,
                    self->activeSession_->terminalHorizontalOffset);
            const int thumbWidth = static_cast<int>(metrics.thumb.right - metrics.thumb.left);
            const int travel = std::max(1,
                static_cast<int>(metrics.track.right - metrics.track.left) - thumbWidth);
            const int desiredLeft = std::max(static_cast<int>(metrics.track.left),
                std::min(static_cast<int>(metrics.track.right) - thumbWidth,
                    static_cast<int>(point.x) - self->terminalHorizontalScrollbarThumbOffset_));
            self->activeSession_->terminalHorizontalOffset = std::max(0,
                std::min(metrics.maximumOffset,
                    (desiredLeft - static_cast<int>(metrics.track.left)) *
                        metrics.maximumOffset / travel));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (self->terminalScrollbarDragging_) {
            const TerminalScrollMetrics metrics = GetTerminalScrollMetrics(window,
                self->activeSession_->terminal.DisplayLineCount(), self->terminalVisibleRows_,
                self->activeSession_->terminalScrollOffset);
            const int thumbHeight = static_cast<int>(metrics.thumb.bottom - metrics.thumb.top);
            const int travel = std::max(1,
                static_cast<int>(metrics.track.bottom - metrics.track.top) - thumbHeight);
            const int desiredTop = std::max(static_cast<int>(metrics.track.top),
                std::min(static_cast<int>(metrics.track.bottom) - thumbHeight,
                    static_cast<int>(point.y) - self->terminalScrollbarThumbOffset_));
            self->activeSession_->terminalScrollOffset = std::max(0,
                std::min(metrics.maximumOffset,
                    (static_cast<int>(metrics.track.top) + travel - desiredTop) *
                        metrics.maximumOffset / travel));
            self->activeSession_->terminalHasNewOutput =
                self->activeSession_->terminalScrollOffset > 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (self->terminalSelecting_) {
            size_t line = 0;
            int column = 0;
            if (self->TerminalPointToCell(point, line, column))
                self->UpdateTerminalSelection(line, column, true);
            return 0;
        }
    }
    if (message == WM_LBUTTONUP) {
        if (self->terminalSelecting_) {
            size_t line = 0;
            int column = 0;
            if (self->TerminalPointToCell({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, line, column))
                self->UpdateTerminalSelection(line, column, true);
        }
        const bool completedSelection = self->terminalSelecting_ && self->HasTerminalSelection();
        self->terminalSelecting_ = false;
        self->terminalScrollbarDragging_ = false;
        self->terminalScrollbarThumbOffset_ = 0;
        self->terminalHorizontalScrollbarDragging_ = false;
        self->terminalHorizontalScrollbarThumbOffset_ = 0;
        if (GetCapture() == window) ReleaseCapture();
        if (completedSelection) self->CopyTerminalSelection(false);
        return 0;
    }
    if (message == WM_CAPTURECHANGED) {
        self->terminalSelecting_ = false;
        self->terminalScrollbarDragging_ = false;
        self->terminalScrollbarThumbOffset_ = 0;
        self->terminalHorizontalScrollbarDragging_ = false;
        self->terminalHorizontalScrollbarThumbOffset_ = 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TerminalSubclassProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::ButtonSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    // DrawOwnerItem paints the whole button, including its rounded exterior.
    // Native erasure would expose COLOR_BTNFACE before that themed paint runs.
    if (message == WM_ERASEBKGND &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        return 1;
    // Owner-drawn buttons otherwise turn the second press into BN_DOUBLECLICKED.
    // Keep this toggle's normal release/cancel semantics for rapid reversal.
    if (GetDlgCtrlID(window) == IdRightPanelToggle && message == WM_LBUTTONDBLCLK)
        message = WM_LBUTTONDOWN;
    if (message == WM_MOUSEMOVE && !GetPropW(window, Ui::HoverProperty)) {
        SetPropW(window, Ui::HoverProperty, reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, TRUE);
    } else if (message == WM_MOUSELEAVE) {
        RemovePropW(window, Ui::HoverProperty);
        InvalidateRect(window, nullptr, TRUE);
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, TRUE);
        auto* self = reinterpret_cast<MainWindow*>(referenceData);
        if (self && GetParent(window) == self->window_ && IsToolbarButton(GetDlgCtrlID(window))) {
            RECT toolbar{};
            GetClientRect(self->window_, &toolbar);
            toolbar.bottom = Ui::ToolbarHeight;
            InvalidateRect(self->window_, &toolbar, FALSE);
        }
    } else if (message == WM_NCDESTROY) {
        RemovePropW(window, Ui::HoverProperty);
        RemoveWindowSubclass(window, ButtonSubclassProc, subclassId);
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    DWORD_PTR current = 0;
    if (GetWindowSubclass(window, ButtonSubclassProc, subclassId, &current) && current == referenceData &&
        UiButtonNeedsRestore(message) &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return result;
}

void MainWindow::PrepareDialogEditField(HWND dialog, HWND edit, MainWindow* self) {
    const LONG_PTR style = GetWindowLongPtrW(edit, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_BORDER);
    const LONG_PTR exStyle = GetWindowLongPtrW(edit, GWL_EXSTYLE) &
        ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrW(edit, GWL_STYLE, style);
    SetWindowLongPtrW(edit, GWL_EXSTYLE, exStyle);
    SetWindowRgn(edit, nullptr, TRUE);

    RECT rect{};
    GetWindowRect(edit, &rect);
    MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&rect), 2);
    HWND frame = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        dialog, nullptr, self->instance_, nullptr);
    if (!frame) return;
    SetPropW(edit, Ui::FieldFrameProperty, frame);
    SetPropW(frame, Ui::FieldEditProperty, edit);
    SetWindowSubclass(frame, DialogFieldFrameSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int textHeight = UiEditTextHeight(edit, std::max(1, height - Ui::Scale(8)));
    const bool multiline = (style & ES_MULTILINE) != 0;
    MoveWindow(edit, rect.left + Ui::Space, rect.top + (multiline ? Ui::Scale(4) : (height - textHeight) / 2),
               std::max(1, width - Ui::Space * 2), multiline ? std::max(1, height - Ui::Scale(8)) : textHeight, TRUE);
    SetWindowPos(frame, edit, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(edit, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

LRESULT CALLBACK MainWindow::DialogFieldFrameSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    HWND edit = reinterpret_cast<HWND>(GetPropW(window, Ui::FieldEditProperty));
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_LBUTTONDOWN) {
        if (edit && IsWindowEnabled(edit)) SetFocus(edit);
        return 0;
    }
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return TRUE;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const Palette colors = Colors(self->darkMode_);
        const bool enabled = edit && IsWindowEnabled(edit);
        FillRect(dc, &client, self->panelBrush_);
        DrawRoundedBox(dc, client, Ui::Radius,
            enabled ? colors.terminal : colors.panel,
            edit && GetFocus() == edit ? UiAccentInk(self->darkMode_) : colors.border);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, Ui::FieldEditProperty);
        RemoveWindowSubclass(window, DialogFieldFrameSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::DialogControlSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (UiEditSelectAll(window, message, wParam)) return 0;
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        HWND frame = reinterpret_cast<HWND>(GetPropW(window, Ui::FieldFrameProperty));
        InvalidateRect(frame ? frame : window, nullptr, TRUE);
        return result;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, Ui::FieldFrameProperty);
        RemoveWindowSubclass(window, DialogControlSubclassProc, subclassId);
    }
    return result;
}

LRESULT CALLBACK MainWindow::CommandDragGhostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        DRAWITEMSTRUCT item{};
        item.CtlID = IdCommandDragGhost;
        item.hwndItem = window;
        item.hDC = dc;
        GetClientRect(window, &item.rcItem);
        self->DrawOwnerItem(item);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::CommandListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_ERASEBKGND && self->draggingCommandIndex_ >= 0) return 1;
    if (message == WM_DRAWITEM && reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlID == IdCommandDragGhost) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_DROPFILES) {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::wstring path;
        if (count == 1) {
            const UINT size = DragQueryFileW(drop, 0, nullptr, 0);
            if (size && size <= 32767) { path.resize(size + 1); DragQueryFileW(drop, 0, path.data(), size + 1); path.resize(size); }
        }
        DragFinish(drop);
        if (!path.empty()) self->ImportCommandFile(path);
        else self->AppendStatus(L"请一次拖入一个命令 TXT 文件", true);
        return 0;
    }
    if (message == WM_TIMER && wParam == CommandDragTimerId && self->draggingCommandIndex_ >= 0) {
        POINT point{}; GetCursorPos(&point); ScreenToClient(window, &point);
        self->UpdateCommandDrag(point, true);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        (self->draggingCommandIndex_ >= 0 || self->pendingCommandDragIndex_ >= 0)) {
        self->EndCommandDrag(false);
        return 0;
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        self->EndCommandDrag(false);
        self->armedCommandIndex_ = -1;
        self->armedCommandAction_ = 0;
        self->pressedCommandIndex_ = -1;
        self->pressedCommandAction_ = 0;
        if (message == WM_CANCELMODE && GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_F2)) {
        const auto index = SendMessageW(window, LB_GETCURSEL, 0, 0);
        if (index != LB_ERR && self->runningCommandIndex_ < 0) {
            if (wParam == VK_F2) self->EditCommand(static_cast<size_t>(index));
            else if (!(lParam & (1LL << 30))) self->SendCommand(static_cast<size_t>(index));
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && self->armedCommandIndex_ >= 0) {
        const int index = self->armedCommandIndex_, action = self->armedCommandAction_;
        self->armedCommandIndex_ = -1;
        self->armedCommandAction_ = 0;
        self->pressedCommandIndex_ = -1;
        self->pressedCommandAction_ = 0;
        if (GetCapture() == window) ReleaseCapture();
        RECT item{}, client{};
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        GetClientRect(window, &client);
        bool inside = PointInRect(client, point) && index < static_cast<int>(self->commands_.size()) &&
                      SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item)) != LB_ERR;
        if (inside) {
            OverlayScrollMetrics scroll;
            RECT card{item.left, item.top + Ui::Scale(4), item.right -
                (GetOverlayScrollMetrics(window, scroll) ? Ui::OverlayScrollLaneWidth : 0), item.bottom - Ui::Scale(4)};
            const auto actions = GetCommandActionRects(card);
            inside = PointInRect(action == 1 ? actions.run : actions.edit, point);
        }
        InvalidateRect(window, nullptr, FALSE);
        if (inside && self->runningCommandIndex_ < 0) {
            if (action == 1) self->SendCommand(static_cast<size_t>(index));
            else self->EditCommand(static_cast<size_t>(index));
        }
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        self->pressedCommandIndex_ = -1;
        self->pressedCommandAction_ = 0;
        InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT client{};
        GetClientRect(window, &client);
        OverlayScrollMetrics scrollMetrics;
        if (GetOverlayScrollMetrics(window, scrollMetrics) &&
            point.x >= client.right - Ui::OverlayScrollLaneWidth)
            return DefSubclassProc(window, message, wParam, lParam);
        const DWORD hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
        if (!HIWORD(hit) && LOWORD(hit) < self->commands_.size()) {
            const int index = static_cast<int>(LOWORD(hit));
            RECT itemRect{};
            SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&itemRect));
            const int cardRight = itemRect.right - (GetOverlayScrollMetrics(window, scrollMetrics) ?
                Ui::OverlayScrollLaneWidth : 0);
            RECT card{itemRect.left, itemRect.top + Ui::Scale(4), cardRight, itemRect.bottom - Ui::Scale(4)};
            const CommandActionRects actions = GetCommandActionRects(card);
            SetFocus(window);
            SendMessageW(window, LB_SETCURSEL, index, 0);
            self->UpdateCommandActions();
            self->pressedCommandIndex_ = index;
            self->pressedCommandAction_ = PointInRect(actions.run, point)    ? 1
                                          : PointInRect(actions.edit, point) ? 2
                                                                             : 0;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            if (PointInRect(actions.run, point)) {
                if (self->runningCommandIndex_ == index) {
                    // Stopping further submissions is a safety action and is
                    // never delayed by the Run repeat guard or mouse capture.
                    self->StopCommandSequence(CommandSequenceEnd::UserStopped);
                    return 0;
                }
                const bool writable = self->connection_ && self->connection_->IsConnected();
                if (writable && self->runningCommandIndex_ < 0) {
                    self->armedCommandIndex_ = index;
                    self->armedCommandAction_ = 1;
                    SetCapture(window);
                }
                return 0;
            }
            // A rapid Run -> Stop click arrives as WM_LBUTTONDBLCLK. Handle
            // the action above, but preserve the list's row double-click Run
            // shortcut everywhere outside that button.
            if (message == WM_LBUTTONDBLCLK)
                return DefSubclassProc(window, message, wParam, lParam);
            if (PointInRect(actions.edit, point)) {
                if (self->runningCommandIndex_ < 0) {
                    self->armedCommandIndex_ = index;
                    self->armedCommandAction_ = 2;
                    SetCapture(window);
                }
                return 0;
            }
            if (PointInRect(card, point) && self->runningCommandIndex_ < 0) {
                self->pendingCommandDragIndex_ = index;
                self->commandDragOrigin_ = point;
                self->commandDragGrab_ = {point.x - itemRect.left, point.y - itemRect.top};
                SetCapture(window);
                return 0;
            }
        }
    } else if (message == WM_MOUSEMOVE) {
        if (self->pendingCommandDragIndex_ >= 0 && self->draggingCommandIndex_ < 0) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (std::abs(point.x - self->commandDragOrigin_.x) >= std::max(1, GetSystemMetrics(SM_CXDRAG) / 2) ||
                std::abs(point.y - self->commandDragOrigin_.y) >= std::max(1, GetSystemMetrics(SM_CYDRAG) / 2)) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = CommandDragGhostProc;
                wc.hInstance = self->instance_;
                wc.lpszClassName = L"SerialCtlCommandDragGhost";
                wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
                RegisterClassW(&wc);
                RECT client{}, item{};
                GetClientRect(window, &client);
                SendMessageW(window, LB_GETITEMRECT, self->pendingCommandDragIndex_, reinterpret_cast<LPARAM>(&item));
                self->draggingCommandIndex_ = self->pendingCommandDragIndex_;
                self->pendingCommandDragIndex_ = -1;
                self->commandDropIndex_ = self->draggingCommandIndex_;
                self->commandInsertIndex_ = self->draggingCommandIndex_;
                self->commandDragInside_ = true;
                self->commandDragGhost_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    wc.lpszClassName, L"", WS_POPUP, 0, 0, client.right, item.bottom - item.top,
                    self->window_, nullptr, self->instance_, self);
                if (!self->commandDragGhost_) { self->EndCommandDrag(false); return 0; }
                OverlayScrollMetrics metrics;
                const int inset = GetOverlayScrollMetrics(window, metrics) ? Ui::OverlayScrollLaneWidth : 0;
                SetWindowRgn(self->commandDragGhost_, CreateRoundRectRgn(0, Ui::Gap / 2,
                    client.right - inset, item.bottom - item.top - Ui::Gap / 2,
                    Ui::CardRadius * 2, Ui::CardRadius * 2), FALSE);
                SetTimer(window, CommandDragTimerId, 160, nullptr);
                self->UpdateCommandDrag(point, false);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (self->draggingCommandIndex_ >= 0) {
            self->UpdateCommandDrag({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, false);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const DWORD hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
        int hoverIndex = -1;
        int hoverAction = 0;
        if (!HIWORD(hit) && LOWORD(hit) < self->commands_.size()) {
            RECT itemRect{};
            OverlayScrollMetrics metrics;
            SendMessageW(window, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&itemRect));
            const int cardRight = itemRect.right - (GetOverlayScrollMetrics(window, metrics) ?
                Ui::OverlayScrollLaneWidth : 0);
            RECT card{itemRect.left, itemRect.top + Ui::Scale(4), cardRight, itemRect.bottom - Ui::Scale(4)};
            const CommandActionRects actions = GetCommandActionRects(card);
            if (PointInRect(actions.run, point)) { hoverIndex = LOWORD(hit); hoverAction = 1; }
            else if (PointInRect(actions.edit, point)) { hoverIndex = LOWORD(hit); hoverAction = 2; }
        }
        if (hoverIndex != self->hoveredCommandIndex_ || hoverAction != self->hoveredCommandAction_) {
            self->hoveredCommandIndex_ = hoverIndex;
            self->hoveredCommandAction_ = hoverAction;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (self->armedCommandIndex_ >= 0) {
            const bool inside = hoverIndex == self->armedCommandIndex_ && hoverAction == self->armedCommandAction_;
            self->pressedCommandIndex_ = inside ? self->armedCommandIndex_ : -1;
            self->pressedCommandAction_ = inside ? self->armedCommandAction_ : 0;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        self->pressedCommandIndex_ = -1;
        self->pressedCommandAction_ = 0;
        self->hoveredCommandIndex_ = -1;
        self->hoveredCommandAction_ = 0;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_LBUTTONUP && self->draggingCommandIndex_ >= 0) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT client{}; GetClientRect(window, &client);
        self->UpdateCommandDrag(point, false);
        self->EndCommandDrag(PointInRect(client, point));
        return 0;
    } else if (message == WM_LBUTTONUP && self->pendingCommandDragIndex_ >= 0) {
        self->EndCommandDrag(false);
        return 0;
    } else if (message == WM_CAPTURECHANGED) {
        self->EndCommandDrag(false);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, CommandListSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::OverlayListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_MOUSEWHEEL) {
        OverlayScrollMetrics metrics;
        if (GetOverlayScrollMetrics(window, metrics)) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = std::max(1, std::abs(delta) / WHEEL_DELTA);
            const int direction = delta > 0 ? -1 : 1;
            const int target = std::max(0, std::min(metrics.maxTopIndex,
                metrics.topIndex + direction * notches * 3));
            SendMessageW(window, LB_SETTOPINDEX, target, 0);
            InvalidateRect(window, nullptr, FALSE);
            UpdateWindow(window);
            return 0;
        }
    } else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        OverlayScrollMetrics metrics;
        RECT client{};
        GetClientRect(window, &client);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (GetOverlayScrollMetrics(window, metrics) &&
            point.x >= client.right - Ui::OverlayScrollLaneWidth) {
            SetFocus(window);
            if (point.y >= metrics.thumb.top && point.y < metrics.thumb.bottom) {
                self->scrollingList_ = window;
                self->scrollingThumbOffset_ = point.y - metrics.thumb.top;
                SetCapture(window);
            } else {
                const int direction = point.y < metrics.thumb.top ? -1 : 1;
                const int target = std::max(0, std::min(metrics.maxTopIndex,
                    metrics.topIndex + direction * metrics.pageItems));
                SendMessageW(window, LB_SETTOPINDEX, target, 0);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
    } else if (message == WM_MOUSEMOVE && self->scrollingList_ == window) {
        OverlayScrollMetrics metrics;
        if (GetOverlayScrollMetrics(window, metrics)) {
            const int trackHeight = metrics.track.bottom - metrics.track.top;
            const int thumbHeight = metrics.thumb.bottom - metrics.thumb.top;
            const int travel = std::max(1, trackHeight - thumbHeight);
            const int trackTop = static_cast<int>(metrics.track.top);
            const int lastThumbTop = static_cast<int>(metrics.track.bottom) - thumbHeight;
            const int desiredTop = std::max(trackTop,
                std::min(lastThumbTop, GET_Y_LPARAM(lParam) - self->scrollingThumbOffset_));
            const int target = std::max(0, std::min(metrics.maxTopIndex,
                (desiredTop - trackTop) * metrics.maxTopIndex / travel));
            SendMessageW(window, LB_SETTOPINDEX, target, 0);
            InvalidateRect(window, nullptr, FALSE);
            UpdateWindow(window);
        }
        return 0;
    } else if (message == WM_LBUTTONUP && self->scrollingList_ == window) {
        self->scrollingList_ = nullptr;
        self->scrollingThumbOffset_ = 0;
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    } else if (message == WM_CAPTURECHANGED && self->scrollingList_ == window) {
        self->scrollingList_ = nullptr;
        self->scrollingThumbOffset_ = 0;
    }

    // Native listbox selection notifications occur inside this dispatch. Scope
    // input origin to it instead of remembering the last key across mouse input.
    const bool keyboardInput = window == self->connectionList_ &&
        (message == WM_KEYDOWN || message == WM_CHAR);
    const bool previousKeyboardInput = self->connectionListKeyboardInput_;
    if (keyboardInput) self->connectionListKeyboardInput_ = true;
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    self->connectionListKeyboardInput_ = previousKeyboardInput;
    if (message == WM_PAINT) {
        DrawOverlayScrollbar(window, self->darkMode_);
    } else if (message == WM_KEYDOWN || message == WM_SIZE || message == LB_SETTOPINDEX) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == LB_ADDSTRING || message == LB_DELETESTRING || message == LB_RESETCONTENT) {
        InvalidateRect(window, nullptr, TRUE);
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, OverlayListSubclassProc, subclassId);
    return result;
}

LRESULT CALLBACK MainWindow::SftpListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_MOUSEMOVE && window == self->sftpList_ && self->sftpTooltip_) {
        const DWORD hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, lParam));
        std::wstring text;
        if (!HIWORD(hit) && LOWORD(hit) < self->sftpEntries_.size()) text = self->sftpEntries_[LOWORD(hit)].name;
        if (text != self->sftpHoverText_) {
            self->sftpHoverText_ = std::move(text);
            TOOLINFOW tool{sizeof(tool)}; tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(window);
            tool.lpszText = const_cast<wchar_t*>(self->sftpHoverText_.c_str());
            SendMessageW(self->sftpTooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    if (message == WM_DROPFILES && window == self->sftpList_) {
        self->HandleSftpDrop(reinterpret_cast<HDROP>(wParam));
        return 0;
    }
    if (message == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            const LRESULT caret = SendMessageW(window, LB_GETCARETINDEX, 0, 0);
            RECT itemRect{};
            if (caret != LB_ERR && SendMessageW(window, LB_GETITEMRECT,
                    static_cast<WPARAM>(caret), reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
                point = {itemRect.left + Ui::Section,
                    (itemRect.top + itemRect.bottom) / 2};
                ClientToScreen(window, &point);
            } else {
                RECT rect{};
                GetWindowRect(window, &rect);
                point = {rect.left + Ui::Section, rect.top + Ui::Section};
            }
        }
        if (window == self->sftpList_) self->ShowSftpContextMenu(point);
        else if (window == self->sftpTransferList_) self->ShowSftpTransferContextMenu(point);
        return 0;
    }
    if (message == WM_KEYDOWN && wParam == 'L' && (GetKeyState(VK_CONTROL) & 0x8000)) { self->EditSftpPath(); return 0; }
    if (message == WM_KEYDOWN && wParam == VK_DELETE && window == self->sftpList_) {
        self->DeleteSelectedSftpEntries();
        return 0;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, SftpListSubclassProc, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SftpPathSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_LBUTTONDBLCLK) { self->EditSftpPath(); return 0; }
    if (message == WM_CONTEXTMENU) { self->ShowSftpContextMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}); return 0; }
    if (message == WM_LBUTTONUP) {
        self->NavigateSftpBreadcrumb({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    }
    if (message == WM_MOUSEMOVE) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hovered = -1;
        for (size_t index = 0; index < self->sftpBreadcrumbHits_.size(); ++index) {
            if (PointInRect(self->sftpBreadcrumbHits_[index].rect, point)) {
                hovered = static_cast<int>(index);
                break;
            }
        }
        if (hovered != self->hoveredSftpBreadcrumb_) {
            self->hoveredSftpBreadcrumb_ = hovered;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        self->hoveredSftpBreadcrumb_ = -1;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_SETCURSOR && self->hoveredSftpBreadcrumb_ >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, SftpPathSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool MainWindow::PreTranslate(MSG& message) {
    if (message.message == WM_KEYDOWN && message.wParam == VK_F6 && (GetKeyState(VK_CONTROL) & 0x8000)) {
        std::vector<HWND> panes{connectionList_, aiVisible_ ? aiPane_.Handle() : powerVisible_ ? powerPane_.Handle() : terminal_,
                                LiveMonitorPage() ? terminal_ : sftpPanelVisible_ ? sftpList_ : commandList_};
        panes.erase(std::remove_if(panes.begin(), panes.end(), [](HWND h) {
            return !h || !IsWindowVisible(h) || !IsWindowEnabled(h);
        }), panes.end());
        if (!panes.empty()) {
            const auto current = std::find_if(panes.begin(), panes.end(), [&](HWND h) {
                return h == message.hwnd || IsChild(h, message.hwnd);
            });
            const int step = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            const int index = current == panes.end() ? 0 :
                (static_cast<int>(current - panes.begin()) + step + static_cast<int>(panes.size())) % panes.size();
            HWND next = panes[static_cast<size_t>(index)];
            if (next == aiPane_.Handle() || next == powerPane_.Handle()) {
                if (HWND child = GetNextDlgTabItem(next, nullptr, FALSE)) next = child;
            }
            SetFocus(next);
        }
        return true;
    }
    if (aiVisible_ && aiPane_.PreTranslate(message)) return true;
    if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN ||
        message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
        message.message == WM_MBUTTONDOWN || message.message == WM_MOUSEWHEEL)
        startupUpdateDeferred_ = true;
    if (message.hwnd != terminal_) { return message.message == WM_KEYDOWN && message.wParam == VK_TAB && IsDialogMessageW(window_, &message); }
    if (message.message != WM_KEYDOWN) return false;
    const WPARAM key = message.wParam;
    if (key == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && HasTerminalSelection() && activeSession_) {
        auto selection = TerminalSelectionText(false);
        const auto id = "session-" + std::to_string(activeSession_->id);
        ShowAiPage(false); aiPane_.AttachSelection(selection, id); return true;
    }
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool claimed = (ctrl && shift && (key == 'C' || key == 'V')) ||
        (ctrl && key == 'C' && HasTerminalSelection()) || (shift && key == VK_INSERT) ||
        key == VK_ESCAPE || (key == VK_TAB && shift) ||
        key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT ||
        key == VK_HOME || key == VK_END || key == VK_INSERT || key == VK_DELETE ||
        key == VK_PRIOR || key == VK_NEXT || (key >= VK_F1 && key <= VK_F12);
    if (claimed) { DispatchMessageW(&message); return true; }
    return false;
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    // Includes direct control activation and window gestures, not just keyboard
    // messages. Once work has begun, never surprise the user with a later restart.
    if (message == WM_COMMAND || message == WM_ENTERSIZEMOVE) startupUpdateDeferred_ = true;
    switch (message) {
    case WM_NOTIFY: {
        const auto header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->hwndFrom == navigationTooltip_ && header->code == TTN_GETDISPINFOW) {
            if (reinterpret_cast<HWND>(header->idFrom) == addCommandButton_) {
                reinterpret_cast<NMTTDISPINFOW*>(header)->lpszText = const_cast<wchar_t*>(L"添加命令");
                return 0;
            }
            navigationTipText_ = ControlText(reinterpret_cast<HWND>(header->idFrom));
            reinterpret_cast<NMTTDISPINFOW*>(lParam)->lpszText = navigationTipText_.data();
            return 0;
        }
        break;
    }
    case AiPetActivityChanged:
        aiPet_.Activity(aiPane_.CompanionActivity());
        return 0;
    case AiPetPreferencesChanged: {
        petSettings_ = aiPet_.Preferences();
        SaveUiState();
        RECT client{}; GetClientRect(window_, &client);
        LayoutControls(client.right, client.bottom);
        return 0;
    }
    case AiPetMenuRequested: {
        RECT anchor{};
        GetWindowRect(reinterpret_cast<HWND>(lParam), &anchor);
        aiPet_.ShowMenu({anchor.left, anchor.bottom + Ui::Space});
        return 0;
    }
    case AiStateChanged:
        RefreshConnectionList();
        EnableWindow(disconnectButton_, aiVisible_ ? aiPane_.Busy() : activeSession_ != nullptr);
        return 0;
    case MessageUpdatePrompt: {
        std::unique_ptr<UpdatePrompt> prompt(reinterpret_cast<UpdatePrompt *>(lParam));
        if (!prompt)
            return 0;
        if (!CanInstallStartupUpdate(true)) {
            AppendStatus(L"已发现新版；当前任务不受影响，请空闲时重新启动以确认更新。", false);
            prompt->answer.set_value(false);
            return 0;
        }
        updateRequired_ = true;
        updateConfirming_ = true;
        updatePromptVersion_ = prompt->version;
        struct Dialog {
            DLGTEMPLATE d{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 270, 150};
            WORD menu = 0, cls = 0, title = 0;
        } data;
        const auto answer =
            UiDialogBoxIndirectOwned(instance_, &data.d, window_, UpdateDialogProc, reinterpret_cast<LPARAM>(this));
        if (answer != IDOK) {
            updateRequired_ = false;
            prompt->answer.set_value(false);
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        updateApprovedVersion_ = prompt->version;
        updateConfirming_ = false;
        updatePhase_ = 0;
        updateReceived_ = 0;
        updateTotal_ = 0;
        prompt->answer.set_value(true);
        UiDialogBoxIndirectOwned(instance_, &data.d, window_, UpdateDialogProc, reinterpret_cast<LPARAM>(this));
        return 0;
    }
    case MessageUpdate: {
        std::unique_ptr<update::UpdateResult> result(reinterpret_cast<update::UpdateResult*>(lParam));
        if (!result) return 0;
        if (updateThread_.joinable()) updateThread_.join();
        if (closing_.load()) { DiscardUpdate(*result); return 0; }
        if (result->status == update::UpdateStatus::Ready) {
            if (!CanInstallStartupUpdate(true)) {
                AppendStatus(L"已发现 " + std::wstring(result->version.begin(), result->version.end()) +
                    L" 新版；当前工作不受影响，请空闲时重新启动以更新。", false);
                DiscardUpdate(*result);
                if (updateDialog_)
                    UiEndOwnedDialog(updateDialog_, IDCANCEL);
                updateRequired_ = false;
                updateApprovedVersion_.clear();
                return 0;
            }
            if (updateDialog_)
                UiEndOwnedDialog(updateDialog_, IDOK);
            std::wstring error;
            if (update::LaunchUpdateWorker(result->stagingDirectory, SERIALCTL_APP_VERSION, error)) {
                // The ready worker now owns this stage. No UI message can start
                // another task between the idle check and synchronous close.
                result->stagingDirectory.clear();
                SendMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            DiscardUpdate(*result);
            MessageBoxW(window_, (L"更新安装失败：" + error + L"\n请重新启动程序重试。").c_str(), L"软件更新",
                        MB_OK | MB_ICONERROR);
            SendMessageW(window_, WM_CLOSE, 0, 0);
        } else if (updateRequired_) {
            const auto choice = MessageBoxW(updateDialog_ ? updateDialog_ : window_,
                                            (result->message + L"\n选择重试继续更新；取消将退出程序。").c_str(),
                                            L"更新失败", MB_RETRYCANCEL | MB_ICONERROR);
            if (choice == IDRETRY) {
                updatePhase_ = 0;
                updateReceived_ = 0;
                StartStartupUpdate();
            } else {
                if (updateDialog_)
                    UiEndOwnedDialog(updateDialog_, IDCANCEL);
                SendMessageW(window_, WM_CLOSE, 0, 0);
            }
        } else if (result->status == update::UpdateStatus::Failed && CanInstallStartupUpdate()) {
            AppendStatus(result->message, true);
        }
        return 0;
    }
    case PowerStateChanged:
        if (disconnectAllPending_ && !powerService_.State().value("connected", false)) {
            disconnectAllPending_ = false;
            powerPageOpened_ = false;
            powerVisible_ = false;
            RECT r{};
            GetClientRect(window_, &r);
            LayoutControls(r.right, r.bottom);
        }
        RefreshConnectionList();
        return 0;
    case MessageApi: {
        std::unique_ptr<ApiRequestMessage> pointer(reinterpret_cast<ApiRequestMessage*>(lParam));
        auto request=*pointer; int expected=0;
        if (request->state.compare_exchange_strong(expected,1)) {
            try { request->result.set_value(ExecuteApiRequest(request->method,request->path,request->body)); }
            catch (...) { request->result.set_value({{"error","API operation failed"}}); }
        }
        return 0;
    }
    case WM_CREATE: {
        wchar_t measure[2]{};
        measurePaint_ = GetEnvironmentVariableW(L"SERIALCTL_UI_METRICS", measure, 2) > 0;
    }
        ScaleUiMetrics(window_);
        LoadUiState();
        CreateControls();
        LoadCommands();
        RefreshCommandList();
        RefreshConnectionList();
        ApplyTheme();
        sessionService_.ObserveInput([this](const std::string &id, const Bytes &bytes, const std::string &source) {
            const auto state = sessionService_.State(id);
            apiServer_.Publish(id, bytes, source.c_str(), "input", state.value("inputCodePage", UINT(0)));
            QueueReceived({std::stoull(id.substr(8)), bytes, state.value("inputCodePage", UINT(0)), source, 0});
        });
        { std::wstring error;
          if (!apiServer_.Start([this](const auto& method, const auto& path, const auto& body) {
                  return ApiRequest(method, path, body);
              }, error)) AppendStatus(error, true);
        }
        return 0;
    case AiTargetChanged:
        SyncAiTerminal();
        return 0;
    case WM_ENTERSIZEMOVE:
        windowResizing_ = true;
        return 0;
    case WM_EXITSIZEMOVE: {
        // Commit the final client geometry while terminal reflow is still
        // deferred. The last sizing notification can trail the native border.
        RECT client{};
        GetClientRect(window_, &client);
        windowResizing_ = true;
        LayoutControls(client.right, client.bottom);
        windowResizing_ = false;
        UpdateTerminalDimensions();
        return 0;
    }
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) return 0;
        KillTimer(window_,SideAnimationTimerId);rightPanelAnimating_=false;rightPanelAnimatedWidth_=-1;
        RECT client{};
        GetClientRect(window_, &client);
        LayoutControls(client.right, client.bottom);
        InvalidateRect(window_, nullptr, TRUE);
        UpdateWindow(window_);
        return 0;
    }
    case WM_SETCURSOR:
        if (!SidebarCollapsed() && LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_, &point);
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, SidebarWidth(), SidebarCollapsed(), rightPanelAnimatedWidth_, LiveMonitorPage());
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (!SidebarCollapsed()) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, SidebarWidth(), SidebarCollapsed(), rightPanelAnimatedWidth_, LiveMonitorPage());
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                rightPanelDragging_ = true;
                SetCapture(window_);
                return 0;
            }
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (!SidebarCollapsed()) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, SidebarWidth(), SidebarCollapsed(), rightPanelAnimatedWidth_, LiveMonitorPage());
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                if (LiveMonitorPage())
                    (powerVisible_ ? powerMonitorRatio_ : aiMonitorRatio_) = 500;
                else
                    rightPanelWidth_ = Ui::Scale(360);
                LayoutControls(client.right, client.bottom);
                InvalidateRect(window_, nullptr, TRUE);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (rightPanelDragging_ && GetCapture() == window_) {
            RECT client{};
            GetClientRect(window_, &client);
            const int newWidth =
                std::max(Ui::MinRightWidth, std::min(LiveMonitorPage() ? static_cast<int>(client.right) : Ui::MaxRightWidth,
                                                     static_cast<int>(client.right) - GET_X_LPARAM(lParam)));
            if (newWidth != SidebarWidth()) {
                if (LiveMonitorPage()) {
                    const int left =
                        std::clamp(static_cast<int>(client.right) * 18 / 100, Ui::MinLeftWidth, Ui::MaxLeftWidth);
                    (powerVisible_ ? powerMonitorRatio_ : aiMonitorRatio_) =
                        std::clamp(MulDiv(newWidth - Ui::Gap, 1000,
                                          std::max(1, static_cast<int>(client.right) - left - Ui::Gap * 3)),
                                   200, 800);
                } else
                    rightPanelWidth_ = newWidth;
                LayoutControls(client.right, client.bottom);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (rightPanelDragging_) {
            rightPanelDragging_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            UpdateTerminalDimensions();
            SaveUiState();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        rightPanelDragging_ = false;
        UpdateTerminalDimensions();
        break;
    case MessageLogExport:
        PollLogExport();
        return 0;
    case WM_TIMER:
        if (wParam == ReceivePumpTimerId) {
            KillTimer(window_, ReceivePumpTimerId);
            return HandleMessage(MessageData, 0, 0);
        }
        if (wParam == BootPulseTimerId) {
            bool active = false;
            for (const auto &session : sessions_) {
                const auto state = session->boot->Status(GetTickCount64());
                active = active || state.state == "armed" || state.state == "interrupting";
                const auto bytes = session->boot->Poll(GetTickCount64());
                if (!bytes.empty()) {
                    std::wstring error;
                    const bool accepted = session->connection && session->connection->IsConnected() &&
                        session->connection->Send(Bytes(bytes.begin(), bytes.end()), error);
                    session->boot->Delivery(accepted, WideToMultiByte(error, CP_UTF8));
                }
            }
            if (!active) KillTimer(window_, BootPulseTimerId);
            return 0;
        }
        if (wParam == LogExportTimerId) { PollLogExport(); return 0; }
        if(wParam==SideAnimationTimerId) {
            if (!rightPanelAnimating_) return 0; // A canceled timer may already have a queued message.
            const double t=std::min(1.0,static_cast<double>(GetTickCount64()-rightPanelAnimationAt_)/200.0);
            const double ease=1-(1-t)*(1-t)*(1-t);
            rightPanelAnimatedWidth_=rightPanelAnimationFrom_+static_cast<int>((rightPanelAnimationTo_-rightPanelAnimationFrom_)*ease);
            if(t>=1){KillTimer(window_,SideAnimationTimerId);rightPanelAnimating_=false;rightPanelAnimatedWidth_=-1;}
            RECT rect{};GetClientRect(window_,&rect);LayoutControls(rect.right,rect.bottom);
            InvalidateRect(window_,nullptr,FALSE);return 0;
        }
        if (wParam == StatusTimerId) {
            KillTimer(window_, StatusTimerId);
            AppendStatus(L"", false);
            return 0;
        }
        if (wParam == CommandTimerId) {
            KillTimer(window_, CommandTimerId);
            SendNextCommandStep();
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),&monitor);
        info->ptMinTrackSize.x = std::min(Ui::Scale(980),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
        info->ptMinTrackSize.y = std::min(Ui::Scale(620),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT rect{};GetClientRect(window_,&rect);
        HDC memory=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,std::max(1L,rect.right),std::max(1L,rect.bottom));
        if(memory&&bitmap){auto previous=SelectObject(memory,bitmap);PaintWindow(memory);BitBlt(dc,0,0,rect.right,rect.bottom,memory,0,0,SRCCOPY);SelectObject(memory,previous);}else PaintWindow(dc);
        if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_DRAWITEM:
        DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlType == ODT_MENU) {
            HDC dc = GetDC(window_);
            HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, uiFont_));
            SIZE extent{};
            const auto* label = reinterpret_cast<const wchar_t*>(measure->itemData);
            if (label) GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &extent);
            SelectObject(dc, old); ReleaseDC(window_, dc);
            measure->itemWidth = std::max(208L, extent.cx + 60);
            measure->itemHeight = measure->itemData == 0 ? Ui::Scale(9) : Ui::Scale(32);
            return TRUE;
        }
        measure->itemHeight =
            measure->CtlID == IdCommandList
                ? Ui::Scale(66)
                : (measure->CtlID == IdSftpList
                       ? Ui::SftpTableRowHeight
                       : (measure->CtlID == IdSftpTransferList ? Ui::SftpTransferRowHeight : Ui::Scale(60)));
        return TRUE;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == terminal_) {
            if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1)
                ShowSettings();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IdPowerMonitorTarget && HIWORD(wParam) == CBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(powerMonitorTarget_, CB_GETCURSEL, 0, 0));
            powerMonitorSession_ = selected > 0 && static_cast<size_t>(selected) <= powerMonitorIds_.size() ?
                powerMonitorIds_[static_cast<size_t>(selected - 1)] : std::string();
            SyncPowerTerminal();
            RefreshPowerTargets();
            return 0;
        }
        if (reinterpret_cast<HWND>(lParam) == aiTerminalHeader_) return 0;
        if (LOWORD(wParam) == IdAi && HIWORD(wParam) == BN_CLICKED) { ShowAiPage(); return 0; }
        if (LOWORD(wParam) == IdCmd && HIWORD(wParam) == BN_CLICKED) {
            if (pendingSession_) { AppendStatus(L"请等待当前连接完成", true); return 0; }
            if (connectionThread_.joinable()) connectionThread_.join();
            pendingMode_ = 4; powerVisible_ = false; ConnectFromDialog(); return 0;
        }
        if (LOWORD(wParam) == IdPower && HIWORD(wParam) == BN_CLICKED) {
            ShowPowerPage(); return 0;
        } {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (notification == BN_CLICKED) {
            if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdAi) {
                OpenConnectionDialog(id - IdSsh);
            }
            else if (id == IdTheme) { darkMode_ = !darkMode_; ApplyTheme(); }
            else if (id == IdSettings) ShowSettings();
            else if (id == IdDisconnect) { if (aiVisible_) aiPane_.Disconnect(); else Disconnect(); }
            else if (id == IdDisconnectAll)
                DisconnectAll();
            else if (id == IdSaveCommands) SaveCommands();
            else if (id == IdImport) ImportCommands();
            else if (id == IdExport) ExportCommands();
            else if (id == IdAddCommand) AddCommand();
            else if (id == IdDeleteCommand) DeleteSelectedCommand();
            else if (id == IdCommandTab) ShowSftpPanel(false);
            else if (id == IdSftpTab) {
                ShowSftpPanel(true);
                RefreshSftp();
            } else if (id == IdAiWrap) {
                auto &wrap = powerVisible_ ? powerMonitorWrap_ : aiMonitorWrap_;
                wrap = !wrap;
                UpdateTerminalDimensions();
                SaveUiState();
                RECT r{};
                GetClientRect(window_, &r);
                LayoutControls(r.right, r.bottom);
            } else if (id == IdRightPanelToggle)
                SetRightPanelCollapsed(!(powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_));
            else if (id == IdSftpNameHeader) SetSftpSort(SftpSortColumn::Name);
            else if (id == IdSftpSizeHeader) SetSftpSort(SftpSortColumn::Size);
            else if (id == IdSftpModifiedHeader) SetSftpSort(SftpSortColumn::Modified);
            else if (id == IdSftpTransferToggle) {
                sftpTransferExpanded_ = !sftpTransferExpanded_;
                RECT client{}; GetClientRect(window_, &client);
                LayoutControls(client.right, client.bottom);
                InvalidateRect(window_, nullptr, TRUE);
            }
            else if (id == IdSftpClearTransfers) ClearFinishedSftpTransfers();
            else if (id == IdSftpUp) RefreshSftp(ParentSftpRemotePath(sftpDirectory_));
            else if (id == IdSftpRefresh) RefreshSftp(sftpDirectory_);
            else if (id == IdSftpUpload) UploadSftp();
            else if (id == IdSftpDownload) DownloadSftp();
            return 0;
        }
        if (id == IdCommandList && notification == LBN_DBLCLK) {
            const LRESULT selected = SendMessageW(commandList_, LB_GETCURSEL, 0, 0);
            if (connection_ && selected != LB_ERR && runningCommandIndex_ < 0)
                SendCommand(static_cast<size_t>(selected));
            return 0;
        }
        if (id == IdCommandList && notification == LBN_SELCHANGE) {
            UpdateCommandActions();
            return 0;
        }
        if (id == IdConnectionList && notification == LBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(connectionList_, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR) {
                if (aiPageOpened_ && static_cast<size_t>(selected) == sessions_.size() + (powerPageOpened_ ? 1 : 0))
                    ShowAiPage(false);
                else if (static_cast<size_t>(selected) == sessions_.size() && powerPageOpened_)
                    ShowPowerPage();
                else SwitchSession(static_cast<size_t>(selected), connectionListKeyboardInput_
                    ? SessionFocus::Preserve : SessionFocus::Terminal);
            }
            return 0;
        }
        if (id == IdSftpList && notification == LBN_DBLCLK) {
            const LRESULT selected = SendMessageW(sftpList_, LB_GETCARETINDEX, 0, 0);
            if (selected != LB_ERR) NavigateSftp(static_cast<size_t>(selected));
            return 0;
        }
        if (id == IdSftpList && notification == LBN_SELCHANGE) {
            SetSftpBusy(sftpBusy_);
            return 0;
        }
        break;
    }
    case MessageData: {
        std::set<std::uint64_t> overflow;
        {
            std::lock_guard<std::mutex> lock(receivedMutex_);
            overflow.swap(receiveOverflow_);
        }
        const auto started = GetTickCount64();
        for (unsigned count = 0; count < 32 && (count == 0 || GetTickCount64() - started < 6); ++count) {
            ReceivedItem item;
            {
                std::lock_guard<std::mutex> lock(receivedMutex_);
                if (received_.empty()) break;
                item = std::move(received_.front());
                received_.pop_front();
                auto found = receivedBytes_.find(item.sessionId);
                if (found != receivedBytes_.end()) {
                    found->second -= std::min(found->second, item.data.size());
                    if (!found->second) receivedBytes_.erase(found);
                }
            }
            if (pendingSession_ && item.sessionId == pendingSession_->id) {
                if (pendingConnectionBytes_ + item.data.size() <= 4 * 1024 * 1024) {
                    pendingConnectionBytes_ += item.data.size();
                    pendingConnectionData_.push_back(std::move(item));
                } else {
                    connectionCancel_ = true;
                    pendingSession_->connection->CancelStart();
                }
            } else if (item.source.empty())
                AppendData(item.sessionId, item.data, item.codePage, item.arrival);
            else
                AppendInput(item);
        }
        {
            std::lock_guard<std::mutex> lock(receivedMutex_);
            receivePosted_ = false;
            if (!received_.empty() || !receiveOverflow_.empty())
                receivePosted_ = SetTimer(window_, ReceivePumpTimerId, 10, nullptr) != 0;
        }
        for (auto id : overflow) {
            SessionState* session = FindSession(id);
            if (session && session->connection) session->connection->Stop();
            PostStatus(id, L"接收数据超过缓冲上限，已停止连接以避免丢失后继续显示。", true);
        }
        return 0;
    }
    case MessageStatus: {
        std::unique_ptr<StatusMessage> status(reinterpret_cast<StatusMessage*>(lParam));
        if (status) {
            SessionState* session = FindSession(status->sessionId);
            auto* aiSession = session ? session : pendingSession_ && pendingSession_->id == status->sessionId ? pendingSession_.get() : nullptr;
            if (aiSession && *ai::TerminalKind(aiSession->mode)) {
                if (status->isError || status->text.find(L"HISTORY_GAP") != std::wstring::npos)
                    aiSession->aiTransportNotice = WideToMultiByte(status->text.substr(0, 512), CP_UTF8);
                if (status->text.find(L"HISTORY_GAP") != std::wstring::npos) aiSession->aiTruncated = true;
            }
            if (session) {
                if (!session->connection || !session->connection->IsConnected())
                    AiDeviceRequest("release", {{"sessionId", "session-" + std::to_string(session->id)}});
                session->lastStatus = status->text;
                session->lastStatusIsError = status->isError;
                session->unreadStatus = session != activeSession_;
                GetLocalTime(&session->statusTime);
            }
            if (session && session == activeSession_) {
                SetConnectedUi(connection_ && connection_->IsConnected());
                if (runningCommandIndex_ >= 0 && (!connection_ || !connection_->IsConnected()))
                    StopCommandSequence(CommandSequenceEnd::Interrupted, status->text);
                else
                    AppendStatus(status->text, status->isError);
            } else if (session && session->logger) {
                session->logger->WriteStatus(status->text);
            }
            RefreshConnectionList();
        }
        return 0;
    }
    case MessageDiscovery: {
        std::unique_ptr<DiscoveryMessage> result(reinterpret_cast<DiscoveryMessage*>(lParam));
        if (result->dialog != discoveryDialog_ || result->generation != discoveryGeneration_ || !IsWindow(result->dialog)) return 0;
        HWND ports = GetDlgItem(result->dialog, IDC_SERIAL);
        SendMessageW(ports, CB_RESETCONTENT, 0, 0);
        for (const auto& name : result->names) SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (!result->success) SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未发现可连接的串口"));
        SendMessageW(ports, CB_SETCURSEL, 0, 0);
        pendingRemoteInstance_ = result->success ? result->instance : std::string();
        if (result->success) SetDlgItemTextW(result->dialog, IDC_PORT, std::to_wstring(result->port).c_str());
        SetDlgItemTextW(result->dialog, IDC_DIALOG_ERROR,
                        result->success ? (L"服务端口 " + std::to_wstring(result->port) + L" · 选择串口连接").c_str()
                                          : result->error.c_str());
        EnableWindow(GetDlgItem(result->dialog, IDOK), result->success);
        InvalidateRect(result->dialog, nullptr, TRUE); return 0;
    }
    case MessageConnection: {
        std::unique_ptr<ConnectionMessage> result(reinterpret_cast<ConnectionMessage*>(lParam));
        if (result) CompleteConnection(result->success, result->error);
        return 0;
    }
    case MessageHostKey: {
        std::unique_ptr<HostKeyMessage> hostMessage(reinterpret_cast<HostKeyMessage*>(lParam));
        if (hostMessage) {
            const auto& request = **hostMessage;
            bool accepted = false;
            if (!closing_ && !connectionCancel_.load()) {
                accepted = HostKeyDialog::Show(window_, request.challenge, uiFont_, darkMode_, Ui::Dpi,
                    [this] { return closing_ || connectionCancel_.load(); });
            }
            (*hostMessage)->decision.set_value(accepted);
        }
        return 0;
    }
    case MessageSftp:
        HandleSftpMessage(lParam);
        return 0;
    case MessageSftpProgress:
        HandleSftpProgress(lParam);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        const Palette colors = Colors(darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, colors.text);
        if (control == themeButton_) {
            SetBkColor(dc, colors.window);
            return reinterpret_cast<LRESULT>(windowBrush_);
        }
        if (control == terminal_ || control == aiTerminalHeader_) {
            SetBkColor(dc, colors.terminal);
            return reinterpret_cast<LRESULT>(terminalBrush_);
        }
        if (control == connectionHeader_ || control == sftpPath_) {
            SetBkColor(dc, colors.panel);
            return reinterpret_cast<LRESULT>(panelBrush_);
        }
        SetBkColor(dc, colors.panel);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_CLOSE:
        if (commandsDirty_) {
            const int choice = MessageBoxW(window_, L"命令或顺序尚未保存。是否保存后退出？", L"保存命令", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL || (choice == IDYES && !SaveCommands())) return 0;
        }
        discoveryCancel_ = true;
        if (discoveryThread_.joinable()) discoveryThread_.join();
        closing_ = true;
        aiPane_.Shutdown();
        if (logExportJob_) logExportJob_->cancel.store(true);
        if (logExportThread_.joinable())
            CancelSynchronousIo(reinterpret_cast<HANDLE>(logExportThread_.native_handle()));
        StopStartupUpdate();
        apiServer_.Stop();
        powerService_.Shutdown();
        connectionCancel_.store(true);
        if (pendingSession_) pendingSession_->connection->CancelStart();
        if (connectionThread_.joinable()) connectionThread_.join();
        pendingSession_.reset();
        StopCommandSequence(CommandSequenceEnd::Silent);
        sftpOperationCancel_.store(true);
        sftpTransferCancel_.store(true);
        while (!sessions_.empty()) {
            activeSession_ = sessions_.back().get();
            connection_ = activeSession_->connection.get();
            logger_ = activeSession_->logger.get();
            Disconnect();
        }
        SaveUiState();
        if (sftpThread_.joinable()) sftpThread_.join();
        if (sftpTransferThread_.joinable()) sftpTransferThread_.join();
        // Device shutdown and transport cleanup take precedence over waiting
        // for a canceled export to finish destination filesystem I/O.
        StopLogExport();
        {
            MSG pending{};
            while (PeekMessageW(&pending, window_, MessageApi, MessageApi, PM_REMOVE)) delete reinterpret_cast<ApiRequestMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageDiscovery, MessageDiscovery, PM_REMOVE)) delete reinterpret_cast<DiscoveryMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageConnection, MessageConnection, PM_REMOVE))
                delete reinterpret_cast<ConnectionMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageHostKey, MessageHostKey, PM_REMOVE))
                delete reinterpret_cast<HostKeyMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageData, MessageData, PM_REMOVE))
                delete reinterpret_cast<DataMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageStatus, MessageStatus, PM_REMOVE))
                delete reinterpret_cast<StatusMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageSftp, MessageSftp, PM_REMOVE))
                delete reinterpret_cast<SftpMessage*>(pending.lParam);
            while (PeekMessageW(&pending, window_, MessageSftpProgress, MessageSftpProgress, PM_REMOVE))
                delete reinterpret_cast<SftpProgressMessage*>(pending.lParam);
        }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        aiPet_.Shutdown();
        if (uiFont_) DeleteObject(uiFont_);
        if (smallFont_) DeleteObject(smallFont_);
        if (titleFont_) DeleteObject(titleFont_);
        if (terminalFont_) DeleteObject(terminalFont_);
        if (windowBrush_) DeleteObject(windowBrush_);
        if (panelBrush_) DeleteObject(panelBrush_);
        if (terminalBrush_) DeleteObject(terminalBrush_);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

HWND MainWindow::CreateChild(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    if (_wcsicmp(type, L"BUTTON") == 0) style |= WS_TABSTOP;
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 24, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    if (_wcsicmp(type, L"BUTTON") == 0)
        SetWindowSubclass(control, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return control;
}

void MainWindow::CreateControls() {
    ScaleUiMetrics(window_);
    terminalFontHeight_=Ui::Scale(terminalFontHeight_);
    const wchar_t* uiFace = UiFontFamily();
    uiFont_ = CreateFontW(-Ui::Scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, uiFace);
    smallFont_ = CreateFontW(-Ui::Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, uiFace);
    dialogFieldStyle_.font = uiFont_;
    dialogFieldStyle_.dpi = Ui::Dpi;
    titleFont_ = CreateFontW(-Ui::Scale(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, uiFace);
    terminalFont_ = CreateFontW(-terminalFontHeight_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    const wchar_t* toolbarText[] = {L"SSH", L"串口", L"Telnet", L"远程串口"};
    for (int index = 0; index < 4; ++index) {
        toolbarButtons_.push_back(CreateChild(L"BUTTON", toolbarText[index], BS_OWNERDRAW, IdSsh + index));
    }
    cmdButton_ = CreateChild(L"BUTTON", L"CMD", BS_OWNERDRAW, IdCmd);
    powerButton_ = CreateChild(L"BUTTON", L"电源", BS_OWNERDRAW, IdPower);
    toolbarButtons_.push_back(cmdButton_); toolbarButtons_.push_back(powerButton_);
    aiButton_ = CreateChild(L"BUTTON", L"AI", BS_OWNERDRAW, IdAi);
    toolbarButtons_.push_back(aiButton_);
    settingsButton_ = CreateChild(L"BUTTON", L"设置", BS_OWNERDRAW, IdSettings);
    themeButton_ = CreateChild(L"BUTTON", L"浅色", BS_OWNERDRAW, IdTheme);
    navigationTooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window_, nullptr, instance_, nullptr);
    auto addTip = [this](HWND control) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(navigationTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    };
    for (auto button : toolbarButtons_) addTip(button);
    addTip(settingsButton_);
    addTip(themeButton_);
    disconnectButton_ = CreateChild(L"BUTTON", L"断开", BS_OWNERDRAW, IdDisconnect);

    connectionHeader_ = CreateChild(L"STATIC", L"连接", SS_LEFT, -1);
    SendMessageW(connectionHeader_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    disconnectAllButton_ = CreateChild(L"BUTTON", L"全部断开", BS_OWNERDRAW, IdDisconnectAll);
    connectionList_ = CreateChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IdConnectionList);

    SetWindowSubclass(connectionList_, OverlayListSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    aiTerminalHeader_ = CreateChild(L"STATIC", L"终端实时输出", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOTIFY, 305);
    addTip(aiTerminalHeader_);
    powerMonitorTarget_ = CreateChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        IdPowerMonitorTarget);
    UiStyleField(powerMonitorTarget_, &dialogFieldStyle_);
    addTip(powerMonitorTarget_);
    terminal_ = CreateChild(L"STATIC", L"终端", SS_NOTIFY | WS_TABSTOP, IdTerminal);
    SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), TRUE);
    SetWindowSubclass(terminal_, TerminalSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    aiPane_.Create(window_, uiFont_, darkMode_, [this](const std::string& method, const Json& params) { return AiDeviceRequest(method, params); });
    aiPet_.Create(window_, uiFont_, darkMode_, petSettings_);
    powerPane_.Create(window_, uiFont_, darkMode_, &powerService_);
    powerPane_.SetLogExportHandler([this](bool csv, const std::wstring &path, SessionLogger::ExportPoint point) {
        const auto slash = point.root.find_last_of(L"\\/");
        const auto name = point.root.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const auto source = (csv ? std::wstring(L"电源测量 CSV · ") : std::wstring(L"电源日志 · ")) + name;
        StartLogExport(source, path, point.bytes,
            [service = &powerService_, csv, path, point = std::move(point)](
                std::wstring &error, const std::atomic<bool> *cancel, const SessionLogger::ExportProgress &progress) {
                return service->ExportLog(csv, path, point, error, cancel, progress);
            });
    });
    status_ = CreateChild(L"STATIC", L"", SS_LEFT, IdStatus);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    ShowWindow(status_, SW_HIDE);
    statusTip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    TOOLINFOW statusTool{sizeof(statusTool)};
    statusTool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    statusTool.hwnd = window_; statusTool.uId = reinterpret_cast<UINT_PTR>(status_);
    statusTool.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(statusTip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&statusTool));
    SendMessageW(statusTip_, TTM_SETMAXTIPWIDTH, 0, Ui::Scale(400));

    commandHeader_ = CreateChild(L"BUTTON", L"命令", BS_OWNERDRAW, IdCommandTab);
    sftpTabButton_ = CreateChild(L"BUTTON", L"SFTP", BS_OWNERDRAW, IdSftpTab);
    rightPanelToggleButton_ = CreateChild(L"BUTTON",
        rightPanelCollapsed_ ? L"展开侧栏" : L"折叠侧栏", BS_OWNERDRAW, IdRightPanelToggle);
    commandList_ = CreateChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IdCommandList);
    SetWindowSubclass(commandList_, CommandListSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(commandList_, OverlayListSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    saveCommandsButton_ = CreateChild(L"BUTTON", L"保存", BS_OWNERDRAW, IdSaveCommands);
    importButton_ = CreateChild(L"BUTTON", L"导入", BS_OWNERDRAW, IdImport);
    exportButton_ = CreateChild(L"BUTTON", L"导出", BS_OWNERDRAW, IdExport);
    addCommandButton_ = CreateChild(L"BUTTON", L"添加命令", BS_OWNERDRAW, IdAddCommand);
    addTip(addCommandButton_);
    DragAcceptFiles(commandList_, TRUE);
    SetWindowLongPtrW(commandList_, GWL_STYLE, GetWindowLongPtrW(commandList_, GWL_STYLE) | WS_CLIPCHILDREN);
    deleteCommandButton_ = CreateChild(L"BUTTON", L"删除", BS_OWNERDRAW, IdDeleteCommand);
    sftpPath_ = CreateChild(L"STATIC", L".", SS_OWNERDRAW | SS_NOTIFY, IdSftpPath);
    SendMessageW(sftpPath_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
    SetWindowSubclass(sftpPath_, SftpPathSubclassProc, 3, reinterpret_cast<DWORD_PTR>(this));
    sftpNameHeader_ = CreateChild(L"BUTTON", L"名称", BS_OWNERDRAW, IdSftpNameHeader);
    sftpSizeHeader_ = CreateChild(L"BUTTON", L"大小", BS_OWNERDRAW, IdSftpSizeHeader);
    sftpModifiedHeader_ = CreateChild(L"BUTTON", L"修改时间", BS_OWNERDRAW, IdSftpModifiedHeader);
    for (HWND header : {sftpNameHeader_, sftpSizeHeader_}) SetWindowSubclass(header, SftpHeaderSubclassProc, 4, reinterpret_cast<DWORD_PTR>(this));
    sftpList_ = CreateChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT |
        LBS_NOTIFY | LBS_EXTENDEDSEL, IdSftpList);
    SetWindowSubclass(sftpList_, OverlayListSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(sftpList_, SftpListSubclassProc, 3, reinterpret_cast<DWORD_PTR>(this));
    DragAcceptFiles(sftpList_, TRUE);
    sftpTooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        window_, nullptr, instance_, nullptr);
    if (sftpTooltip_) {
        TOOLINFOW tool{sizeof(tool)}; tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tool.hwnd = sftpList_;
        tool.uId = reinterpret_cast<UINT_PTR>(sftpList_); tool.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(sftpTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(sftpTooltip_, TTM_SETMAXTIPWIDTH, 0, Ui::MaxRightWidth);
    }
    sftpTransferToggleButton_ = CreateChild(L"BUTTON", L"传输", BS_OWNERDRAW, IdSftpTransferToggle);
    sftpTransferList_ = CreateChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED |
        LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IdSftpTransferList);
    SetWindowSubclass(sftpTransferList_, OverlayListSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(sftpTransferList_, SftpListSubclassProc, 3, reinterpret_cast<DWORD_PTR>(this));
    sftpClearTransfersButton_ = CreateChild(L"BUTTON", L"清理", BS_OWNERDRAW, IdSftpClearTransfers);
    sftpUpButton_ = CreateChild(L"BUTTON", L"上级", BS_OWNERDRAW, IdSftpUp);
    sftpRefreshButton_ = CreateChild(L"BUTTON", L"刷新", BS_OWNERDRAW, IdSftpRefresh);
    sftpUploadButton_ = CreateChild(L"BUTTON", L"上传", BS_OWNERDRAW, IdSftpUpload);
    sftpDownloadButton_ = CreateChild(L"BUTTON", L"下载", BS_OWNERDRAW, IdSftpDownload);
    RefreshSftpTransferList();
    ShowSftpPanel(false);
    SetConnectedUi(false);
    // ShowWindow makes children hit-testable before its initial WM_SIZE has
    // finished. Commit the real geometry while the parent is still hidden so
    // an immediate first click cannot target the 100x24 creation rectangles.
    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
}

void MainWindow::LayoutControls(int width, int height) {
    UiLayoutBatch batch;
    if (width <= 0 || height <= 0) return;
    rightPanelAutoCollapsed_ = width < Ui::MinLeftWidth + Ui::MinRightWidth +
                                           (LiveMonitorPage() ? Ui::MinRightWidth : Ui::MinCenterWidth) + Ui::Gap * 3;
    const int toolbarButtonTop = (Ui::ToolbarHeight - Ui::StandardHeight) / 2;
    compactNavigation_ = CompactNavigation(width);
    int x = Ui::Gap + Ui::Space;
    for (size_t index = 0; index < toolbarButtons_.size(); ++index) {
        const int buttonWidth = Ui::Scale(compactNavigation_ ? 48 : Ui::ToolbarButtonWidths[index]);
        batch.Move(toolbarButtons_[index], x, toolbarButtonTop, buttonWidth, Ui::StandardHeight,
                   TRUE);
        x += buttonWidth + Ui::Space;
    }
    batch.Move(settingsButton_, x, toolbarButtonTop, Ui::Scale(compactNavigation_ ? 48 : Ui::SettingsButtonWidth), Ui::StandardHeight, TRUE);
    batch.Move(themeButton_, width - Ui::Gap - Ui::StandardHeight, toolbarButtonTop,
               Ui::StandardHeight, Ui::StandardHeight, TRUE);

    const MainLayoutMetrics layout =
        GetMainLayoutMetrics(width, height, SidebarWidth(), SidebarCollapsed(), rightPanelAnimatedWidth_, LiveMonitorPage());
    const int cardTop = MainCardTop();
    const int centerCardBottom = layout.contentBottom;
    const int sideCardBottom = height - Ui::Gap;
    const int leftInnerLeft = Ui::Gap + Ui::PanelPadding;
    const int leftInnerRight = layout.leftWidth - Ui::PanelPadding;
    const int headerTop = cardTop + Ui::PanelPadding;
    const int leftListTop = headerTop + Ui::StandardHeight + 4;
    const int rightListTop = headerTop + Ui::StandardHeight + 4;

    batch.Move(connectionHeader_, leftInnerLeft, headerTop + 4, leftInnerRight - leftInnerLeft - Ui::Scale(64),
               Ui::Scale(24), TRUE);
    batch.Move(disconnectButton_, leftInnerRight - Ui::Scale(56), headerTop, Ui::Scale(56), Ui::CompactHeight, TRUE);
    const int connectionHeight = std::max(Ui::Scale(20), sideCardBottom - Ui::PanelPadding -
        Ui::CompactHeight - Ui::Space - leftListTop - (statusHeight_ ? statusHeight_ + Ui::Space : 0));
    const int desiredPetHeight = aiPet_.DesiredHeight();
    const int petHeight = !aiPet_.Desktop() && aiVisible_ && desiredPetHeight &&
        connectionHeight >= Ui::Scale(120) + desiredPetHeight + Ui::Space ? desiredPetHeight : 0;
    batch.Move(connectionList_, leftInnerLeft, leftListTop, leftInnerRight - leftInnerLeft,
               connectionHeight - (petHeight ? petHeight + Ui::Space : 0), TRUE);
    if (aiPet_.Handle() && !aiPet_.Desktop()) {
        batch.Move(aiPet_.Handle(), leftInnerLeft, leftListTop + connectionHeight - petHeight,
                   leftInnerRight - leftInnerLeft, std::max(1, petHeight), TRUE);
        batch.Show(aiPet_.Handle(), petHeight ? SW_SHOW : SW_HIDE);
    }

    batch.Move(disconnectAllButton_, leftInnerLeft, sideCardBottom - Ui::PanelPadding - Ui::CompactHeight,
               leftInnerRight - leftInnerLeft, Ui::CompactHeight, FALSE);
    const bool monitor = LiveMonitorPage() && !SidebarCollapsed();
    if (monitor)
        batch.Move(terminal_, layout.rightLeft + Ui::PanelPadding, headerTop + Ui::StandardHeight + Ui::Space,
            std::max(Ui::Scale(80), width - Ui::Gap - layout.rightLeft - Ui::PanelPadding * 2),
            std::max(Ui::Scale(48), sideCardBottom - Ui::PanelPadding - headerTop - Ui::StandardHeight - Ui::Space), TRUE);
    else if (!LiveMonitorPage())
        batch.Move(terminal_, layout.centerLeft + Ui::PanelPadding, cardTop + Ui::PanelPadding,
            layout.centerRight - layout.centerLeft - Ui::PanelPadding * 2,
            centerCardBottom - cardTop - Ui::PanelPadding * 2, TRUE);
    const int monitorInnerWidth = width - Ui::Gap - layout.rightLeft - Ui::PanelPadding * 2;
    const int wrapWidth = monitorInnerWidth < Ui::Scale(80 + 12 + 96 + 48) ? Ui::CompactHeight : Ui::Scale(96);
    const int wrapLeft = width - Ui::Gap - Ui::PanelPadding - Ui::CollapsedRightWidth - wrapWidth;
    batch.Move(aiTerminalHeader_, layout.rightLeft + Ui::PanelPadding, headerTop + 2,
               std::max(0, wrapLeft - Ui::Gap - layout.rightLeft - Ui::PanelPadding),
               Ui::CompactHeight, TRUE);
    if (!aiWrapButton_) {
        aiWrapButton_ = CreateWindowExW(0, L"BUTTON", L"长行查看", WS_CHILD | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 0, 0,
                                        window_, reinterpret_cast<HMENU>(IdAiWrap), instance_, nullptr);
        SendMessageW(aiWrapButton_, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), FALSE);
        SetWindowSubclass(aiWrapButton_, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(aiWrapButton_);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(navigationTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    SetWindowTextW(aiWrapButton_, (powerVisible_ ? powerMonitorWrap_ : aiMonitorWrap_) ? L"自动折行" : L"长行查看");
    batch.Move(aiWrapButton_, wrapLeft, headerTop + 2, wrapWidth, Ui::CompactHeight, TRUE);
    batch.Show(aiWrapButton_, monitor ? SW_SHOW : SW_HIDE);
    const bool powerHeading = monitorInnerWidth >= Ui::Scale(460);
    if (powerVisible_) {
        const int start = layout.rightLeft + Ui::PanelPadding;
        if (powerHeading) batch.Move(aiTerminalHeader_, start, headerTop + 2, Ui::Scale(84), Ui::CompactHeight, TRUE);
        const int left = start + (powerHeading ? Ui::Scale(96) : 0);
        batch.Move(powerMonitorTarget_, left, headerTop + 2, std::max(Ui::Scale(48), wrapLeft - Ui::Gap - left), Ui::CompactHeight, TRUE);
        batch.ComboVisibleHeight(powerMonitorTarget_, Ui::CompactHeight);
    }
    batch.Show(powerMonitorTarget_, monitor && powerVisible_ ? SW_SHOW : SW_HIDE);
    batch.Show(aiTerminalHeader_, monitor && (!powerVisible_ || powerHeading) ? SW_SHOW : SW_HIDE);

    batch.Move(powerPane_.Handle(), layout.centerLeft + Ui::PanelPadding, cardTop + Ui::PanelPadding,
               layout.centerRight - layout.centerLeft - Ui::PanelPadding * 2,
               centerCardBottom - cardTop - Ui::PanelPadding * 2, TRUE);
    batch.Move(aiPane_.Handle(), layout.centerLeft + Ui::PanelPadding, cardTop + Ui::PanelPadding,
               layout.centerRight - layout.centerLeft - Ui::PanelPadding * 2,
               centerCardBottom - cardTop - Ui::PanelPadding * 2, TRUE);
    batch.Show(aiPane_.Handle(), aiVisible_ ? SW_SHOW : SW_HIDE);
    const std::string monitorTarget = powerVisible_ ? powerMonitorSession_ : aiPane_.AssociatedTarget();
    batch.Show(terminal_, LiveMonitorPage() ? (monitor && activeSession_ && monitorTarget == "session-" +
        std::to_string(activeSession_->id) ? SW_SHOW : SW_HIDE) : SW_SHOW);
    batch.Show(powerPane_.Handle(), powerVisible_ ? SW_SHOW : SW_HIDE);
    batch.Show(disconnectButton_, (aiVisible_ || (activeSession_ && !powerVisible_)) ? SW_SHOW : SW_HIDE);
    if (aiVisible_) EnableWindow(disconnectButton_, aiPane_.Busy());
    EnableWindow(rightPanelToggleButton_, TRUE);
    const int rightLeft = layout.rightLeft;
    const int rightInnerLeft = rightLeft + Ui::PanelPadding;
    const int actualRightInnerRight = width - Ui::Gap - Ui::PanelPadding;
    const int rightInnerRight = SidebarCollapsed()
        ? rightInnerLeft + Ui::MinRightWidth - Ui::PanelPadding * 2
        : actualRightInnerRight;
    // The final rail is fixed at the right edge. Keep its button stationary even
    // while the rest of the sidebar moves, avoiding missed releases and trails.
    const int toggleLeft = width - Ui::Gap - Ui::CollapsedRightWidth +
        (Ui::CollapsedRightWidth - Ui::IconButtonSize) / 2;
    batch.Move(rightPanelToggleButton_, toggleLeft, headerTop + 2, Ui::IconButtonSize, Ui::IconButtonSize, TRUE);
    batch.Move(commandHeader_, rightInnerLeft, headerTop + Ui::Scale(2), Ui::SegmentWidth, Ui::SegmentHeight, TRUE);
    batch.Move(sftpTabButton_, rightInnerLeft + Ui::SegmentWidth + Ui::Scale(4), headerTop + Ui::Scale(2), Ui::SegmentWidth,
               Ui::SegmentHeight, TRUE);
    const int addCommandRight = toggleLeft - Ui::Space;
    const int addCommandLeftLimit = rightInnerLeft + Ui::Scale(4) + Ui::SegmentWidth * 2 + Ui::Space;
    HDC measure = GetDC(addCommandButton_);
    const auto previousFont = SelectObject(measure, uiFont_);
    SIZE addLabel{};
    GetTextExtentPoint32W(measure, L"添加命令", 4, &addLabel);
    SelectObject(measure, previousFont);
    ReleaseDC(addCommandButton_, measure);
    const int completeWidth = addLabel.cx + Ui::Scale(32) + Ui::Space;
    const int addCommandWidth = std::max(Ui::IconButtonSize,
        std::min(completeWidth, addCommandRight - addCommandLeftLimit));
    const wchar_t *addText = addCommandWidth >= completeWidth ? L"添加命令" : L"";
    addCommandCompact_ = !*addText;
    batch.Move(addCommandButton_, addCommandRight - addCommandWidth, headerTop + Ui::Scale(2), addCommandWidth, Ui::SegmentHeight,
               TRUE);
    const int footerTop = sideCardBottom - Ui::PanelPadding - Ui::CompactHeight;
    const int listBottom = footerTop - Ui::Gap;
    batch.Move(commandList_, rightInnerLeft, rightListTop, rightInnerRight - rightInnerLeft,
               std::max(20, listBottom - rightListTop), TRUE);
    const int commandAvailable = rightInnerRight - rightInnerLeft - Ui::Gap * 3;
    int commandIndex = 0;
    for (HWND button : {importButton_, exportButton_, deleteCommandButton_, saveCommandsButton_}) {
        const int footerX = rightInnerLeft + commandAvailable * commandIndex / 4 + Ui::Gap * commandIndex;
        const int end = rightInnerLeft + commandAvailable * (commandIndex + 1) / 4 + Ui::Gap * commandIndex;
        batch.Move(button, footerX, footerTop, end - footerX, Ui::CompactHeight, TRUE);
        ++commandIndex;
    }
    const int sftpContentWidth = std::max(1, rightInnerRight - rightInnerLeft);
    batch.Move(sftpPath_, rightInnerLeft, rightListTop, sftpContentWidth, Ui::CompactHeight, TRUE);
    const int sftpHeaderTop = rightListTop + Ui::CompactHeight + Ui::Space;
    int nameWidth, sizeWidth, modifiedWidth;
    SftpColumnWidths(sftpContentWidth - Ui::OverlayScrollLaneWidth, nameWidth, sizeWidth, modifiedWidth);
    batch.Move(sftpNameHeader_, rightInnerLeft, sftpHeaderTop, nameWidth, Ui::CompactHeight, TRUE);
    batch.Move(sftpSizeHeader_, rightInnerLeft + nameWidth, sftpHeaderTop, sizeWidth, Ui::CompactHeight, TRUE);
    batch.Move(sftpModifiedHeader_, rightInnerLeft + nameWidth + sizeWidth, sftpHeaderTop, modifiedWidth,
               Ui::CompactHeight, TRUE);
    const int sftpListTop = sftpHeaderTop + Ui::CompactHeight;
    const int transferListHeight = sftpTransferExpanded_ ? Ui::SftpTransferDrawerHeight : 0;
    const int transferHeaderTop = footerTop - Ui::Gap - Ui::CompactHeight -
        (sftpTransferExpanded_ ? Ui::Gap + transferListHeight : 0);
    const int sftpListBottom = transferHeaderTop - Ui::Gap;
    batch.Move(sftpList_, rightInnerLeft, sftpListTop, sftpContentWidth,
               std::max(Ui::SftpTableRowHeight, sftpListBottom - sftpListTop), TRUE);
    const int clearWidth = 64;
    batch.Move(sftpTransferToggleButton_, rightInnerLeft, transferHeaderTop,
               std::max(80, sftpContentWidth - clearWidth - Ui::Space), Ui::CompactHeight, TRUE);
    batch.Move(sftpClearTransfersButton_, rightInnerRight - clearWidth, transferHeaderTop, clearWidth,
               Ui::CompactHeight, TRUE);
    batch.Move(sftpTransferList_, rightInnerLeft, transferHeaderTop + Ui::CompactHeight + Ui::Gap, sftpContentWidth,
               transferListHeight, TRUE);
    const int sftpAvailable = rightInnerRight - rightInnerLeft - Ui::Gap * 3;
    int sftpIndex = 0;
    for (HWND button : {sftpUpButton_, sftpRefreshButton_, sftpUploadButton_, sftpDownloadButton_}) {
        const int footerX = rightInnerLeft + sftpAvailable * sftpIndex / 4 + Ui::Gap * sftpIndex;
        const int end = rightInnerLeft + sftpAvailable * (sftpIndex + 1) / 4 + Ui::Gap * sftpIndex;
        batch.Move(button, footerX, footerTop, end - footerX, Ui::CompactHeight, TRUE);
        ++sftpIndex;
    }

    batch.Move(status_, leftInnerLeft,
               sideCardBottom - Ui::PanelPadding - Ui::CompactHeight - Ui::Space - statusHeight_,
               leftInnerRight - leftInnerLeft, statusHeight_, TRUE);
    // Visibility and final geometry must be committed together. Showing a child
    // here with ShowWindow would expose its old bounds before the batch applies.
    QueueSidebarVisibility(batch, sftpContentWidth);
    batch.Commit();
    aiPet_.Present(aiVisible_, petHeight != 0);
    UpdateTerminalDimensions();
}

void MainWindow::PaintWindow(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    const Palette colors = Colors(darkMode_);
    FillRect(dc, &client, windowBrush_);
    const RECT toolbar = NavigationFrame(client.right);
    DrawRoundedBox(dc, toolbar, Ui::CardRadius, colors.panel, colors.border);

    const MainLayoutMetrics layout = GetMainLayoutMetrics(client.right, client.bottom, SidebarWidth(),
                                                          SidebarCollapsed(), rightPanelAnimatedWidth_, LiveMonitorPage());
    const int cardTop = MainCardTop();
    RECT leftCard{Ui::Gap, cardTop, layout.leftWidth, client.bottom - Ui::Gap};
    RECT terminalFrame{layout.centerLeft, cardTop,
        layout.centerRight, layout.contentBottom};
    RECT rightCard{layout.rightLeft, cardTop,
        client.right - Ui::Gap, client.bottom - Ui::Gap};
    DrawRoundedBox(dc, leftCard, Ui::CardRadius, colors.panel, colors.border);
    DrawRoundedBox(dc, terminalFrame, Ui::CardRadius, LiveMonitorPage() ? UiAiTheme(darkMode_).surface : colors.terminal, colors.border);
    DrawRoundedBox(dc, rightCard, Ui::CardRadius, LiveMonitorPage() ? colors.terminal : colors.panel, colors.border);
    if (LiveMonitorPage() && !SidebarCollapsed()) {
        UiSeparator(dc, rightCard.left + Ui::PanelPadding, rightCard.right - Ui::PanelPadding,
                    cardTop + Ui::PanelPadding + Ui::StandardHeight + Ui::Scale(2), colors.border);
    }
}

void MainWindow::DrawOwnerItem(const DRAWITEMSTRUCT& source) {
    if (source.CtlID == IdCommandDragGhost && draggingCommandIndex_ >= 0) {
        DRAWITEMSTRUCT ghost = source;
        ghost.CtlType = ODT_LISTBOX;
        ghost.CtlID = IdCommandList;
        ghost.hwndItem = commandList_;
        ghost.itemID = static_cast<UINT>(draggingCommandIndex_);
        ghost.itemState = 0;
        ghost.itemData = ~ULONG_PTR(0);
        DrawOwnerItem(ghost);
        return;
    }
    if (source.CtlType == ODT_COMBOBOX) {
        UiDrawComboItem(source, dialogFieldStyle_);
        return;
    }
    auto item = source;
    if (item.itemID == static_cast<UINT>(-1) && item.CtlType != ODT_BUTTON &&
        item.CtlType != ODT_COMBOBOX && item.CtlType != ODT_MENU) return;
    UiBufferedPaint paint(item.hDC, item.rcItem);
    item.hDC = paint.Dc();
    const Palette colors = Colors(darkMode_);
    HDC dc = item.hDC;
    RECT rect = item.rcItem;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;

    if (item.CtlType == ODT_MENU) {
        HBRUSH background = CreateSolidBrush(colors.panel);
        FillRect(dc, &rect, background);
        DeleteObject(background);
        if (item.itemData == 0) {
            HPEN separator = CreatePen(PS_SOLID, 1, colors.border);
            HPEN oldSeparator = reinterpret_cast<HPEN>(SelectObject(dc, separator));
            const int y = (rect.top + rect.bottom) / 2;
            MoveToEx(dc, rect.left + 12, y, nullptr);
            LineTo(dc, rect.right - 12, y);
            SelectObject(dc, oldSeparator);
            DeleteObject(separator);
            return;
        }
        if (selected) {
            RECT highlight = rect;
            InflateRect(&highlight, -3, -2);
            DrawRoundedBox(dc, highlight, Ui::Radius, colors.accent, colors.accent);
        }
        const bool checked = (item.itemState & ODS_CHECKED) != 0 || (item.itemID == IdMenuLocalEcho && localEchoEnabled_) ||
            (item.itemID == IdMenuTimestamp && timestampEnabled_) ||
            (item.itemID == IdMenuEncodingUtf8 && selectedCodePage_ == CP_UTF8) ||
            (item.itemID == IdMenuEncodingGbk && selectedCodePage_ == 936) ||
            (item.itemID == IdMenuEncodingGb2312 && selectedCodePage_ == 20936) ||
            (item.itemID >= IdMenuEndingCr && item.itemID <= IdMenuEndingNone &&
                static_cast<int>(item.itemID - IdMenuEndingCr) == lineEndingIndex_);
        RECT check{rect.left + 10, rect.top, rect.left + 34, rect.bottom};
        if (checked)
            DrawTextSimple(dc, L"✓", check, colors.accent, uiFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT textRect{rect.left + 38, rect.top, rect.right - 12, rect.bottom};
        const auto* label = reinterpret_cast<const wchar_t*>(item.itemData);
        const COLORREF menuText = item.itemID == IdMenuSftpDelete ? colors.danger : colors.text;
        DrawTextSimple(dc, label ? label : L"", textRect,
                       (item.itemState & ODS_DISABLED) ? colors.muted
                       : selected                      ? RGB(255, 255, 255)
                                                       : menuText,
                       uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    if (item.CtlType == ODT_STATIC && item.CtlID == IdSftpPath) {
        HBRUSH outsideBrush = CreateSolidBrush(colors.panel);
        FillRect(dc, &rect, outsideBrush);
        DeleteObject(outsideBrush);
        DrawRoundedBox(dc, rect, Ui::Radius, colors.terminal, colors.border);
        sftpBreadcrumbHits_.clear();
        const std::wstring visibleText = ControlText(item.hwndItem);
        if (visibleText != sftpDirectory_ || sftpDirectory_.empty()) {
            RECT textRect = rect;
            textRect.left += 10;
            textRect.right -= 10;
            DrawTextSimple(dc, visibleText, textRect, colors.muted, smallFont_,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            return;
        }
        const std::vector<SftpBreadcrumb> crumbs = BuildSftpBreadcrumbs(sftpDirectory_);
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, smallFont_));
        std::vector<int> widths;
        int totalWidth = 0;
        for (const SftpBreadcrumb& crumb : crumbs) {
            SIZE size{};
            GetTextExtentPoint32W(dc, crumb.label.c_str(), static_cast<int>(crumb.label.size()), &size);
            widths.push_back(size.cx + Ui::Section);
            totalWidth += size.cx + Ui::Section;
        }
        SelectObject(dc, oldFont);
        const int available = std::max(1,
            static_cast<int>(rect.right - rect.left) - 20);
        std::vector<int> display;
        if (totalWidth <= available || crumbs.size() <= 2) {
            for (size_t index = 0; index < crumbs.size(); ++index)
                display.push_back(static_cast<int>(index));
        } else {
            display.push_back(0);
            display.push_back(-1);
            int used = widths.front() + 32;
            std::vector<int> suffix;
            for (int index = static_cast<int>(crumbs.size()) - 1; index > 0; --index) {
                if (used + widths[static_cast<size_t>(index)] > available && !suffix.empty()) break;
                suffix.push_back(index);
                used += widths[static_cast<size_t>(index)];
            }
            std::reverse(suffix.begin(), suffix.end());
            display.insert(display.end(), suffix.begin(), suffix.end());
        }
        int x = rect.left + 10;
        for (size_t displayIndex = 0; displayIndex < display.size() && x < rect.right - 10; ++displayIndex) {
            const int crumbIndex = display[displayIndex];
            const int width = crumbIndex < 0 ? 24 : widths[static_cast<size_t>(crumbIndex)];
            RECT labelRect{x, rect.top,
                std::min(static_cast<int>(rect.right) - 10, x + width), rect.bottom};
            if (crumbIndex < 0) {
                DrawTextSimple(dc, L"…", labelRect, colors.muted, smallFont_,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                SftpBreadcrumbHit hit{labelRect, crumbs[static_cast<size_t>(crumbIndex)].path};
                sftpBreadcrumbHits_.push_back(hit);
                const bool hovered = hoveredSftpBreadcrumb_ ==
                    static_cast<int>(sftpBreadcrumbHits_.size()) - 1;
                DrawTextSimple(dc, crumbs[static_cast<size_t>(crumbIndex)].label, labelRect,
                    hovered ? UiAccentInk(darkMode_) : colors.text, smallFont_,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            x = labelRect.right;
            if (displayIndex + 1 < display.size()) {
                RECT chevron{x, rect.top,
                    std::min(static_cast<int>(rect.right) - 10, x + Ui::Space), rect.bottom};
                DrawChevronHorizontal(dc, chevron, colors.muted, true);
                x = chevron.right;
            }
        }
        return;
    }

    if (item.CtlType == ODT_BUTTON) {
        const int id = static_cast<int>(item.CtlID);
        const bool enabled = IsWindowEnabled(item.hwndItem) != FALSE;
        const bool hovered = GetPropW(item.hwndItem, Ui::HoverProperty) != nullptr;
        COLORREF outside = id == IdTheme ? colors.window : colors.panel;
        if (LiveMonitorPage() && (id == IdAiWrap || id == IdRightPanelToggle))
            outside = colors.terminal;
        if (id == IdCommandTab || id == IdSftpTab) outside = colors.panel;
        if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdAi) outside = colors.panel;
        HBRUSH outsideBrush = CreateSolidBrush(outside);
        FillRect(dc, &rect, outsideBrush);
        DeleteObject(outsideBrush);
        if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdAi || id == IdSettings) {
            const bool down = pressed && enabled;
            const bool inverse = darkMode_ || down;
            const COLORREF ink = !enabled ? colors.muted : down ? RGB(255, 255, 255) : colors.text;
            const COLORREF fill = down ? colors.accent : enabled && hovered ? colors.panelAlt : colors.panel;
            if (fill != colors.panel)
                DrawRoundedBox(dc, rect, Ui::Radius, fill, fill);
            const ToolbarIcon icon = id == IdSettings ? ToolbarIcon::Settings :
                id == IdAi ? ToolbarIcon::Ai : id == IdCmd ? ToolbarIcon::Cmd : id == IdPower ? ToolbarIcon::Power :
                static_cast<ToolbarIcon>(id - IdSsh);
            const int size = Ui::Scale(24), top = (rect.top+rect.bottom-size)/2;
            const int iconLeft = compactNavigation_ ? (rect.left + rect.right - size) / 2 : rect.left + Ui::Scale(12);
            RECT iconRect{iconLeft, top, iconLeft+size, top+size};
            toolbarIcons_.Draw(dc, icon, iconRect, inverse);
            RECT textRect = rect;
            textRect.left += Ui::Scale(46);
            if (!compactNavigation_)
                DrawTextSimple(dc, ControlText(item.hwndItem), textRect, ink,
                    uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            return;
        }
        if (id == IdTheme) {
            const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered);
            DrawRoundedBox(dc, rect, Ui::Radius, visual.fill, visual.border);
            RECT iconRect=rect; InflateRect(&iconRect,-Ui::Scale(6),-Ui::Scale(6));
            toolbarIcons_.Draw(dc, darkMode_ ? ToolbarIcon::Sun : ToolbarIcon::Moon, iconRect,
                               darkMode_ || (pressed && enabled));
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            return;
        }
        if (id == IdRightPanelToggle) {
            const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered);
            DrawRoundedBox(dc, rect, Ui::Radius, visual.fill, visual.border);
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            DrawChevronHorizontal(dc, rect, visual.ink,
                                  (powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) ||
                                      rightPanelAutoCollapsed_);
            return;
        }
        if (id == IdDisconnect || id == IdDisconnectAll) {
            const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered);
            DrawRoundedBox(dc, rect, Ui::Radius, visual.fill, visual.border);
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            DrawTextSimple(dc, id == IdDisconnect ? L"断开" : L"全部断开", rect,
                           !enabled  ? colors.muted
                           : pressed ? visual.ink
                                     : colors.danger,
                           smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        if (id == IdCommandTab || id == IdSftpTab) {
            const bool active = (id == IdSftpTab) == sftpPanelVisible_;
            const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered);
            DrawRoundedBox(dc, rect, Ui::Radius, visual.fill, active && !pressed ? colors.muted : visual.border);
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            DrawTextSimple(dc, ControlText(item.hwndItem), rect, visual.ink, uiFont_,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        if (id == IdSftpNameHeader || id == IdSftpSizeHeader || id == IdSftpModifiedHeader) {
            const SftpSortColumn column = id == IdSftpNameHeader ? SftpSortColumn::Name :
                (id == IdSftpSizeHeader ? SftpSortColumn::Size : SftpSortColumn::Modified);
            const bool active = sftpSortColumn_ == column;
            HBRUSH background = CreateSolidBrush(hovered ? colors.accentSoft : colors.panel);
            FillRect(dc, &rect, background);
            DeleteObject(background);
            RECT textRect = rect;
            textRect.left += Ui::Space;
            textRect.right -= active ? 22 : Ui::Space;
            DrawTextSimple(dc, ControlText(item.hwndItem), textRect,
                active ? UiAccentInk(darkMode_) : colors.muted, smallFont_,
                id == IdSftpNameHeader ? DT_LEFT | DT_VCENTER | DT_SINGLELINE :
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            if (active) DrawSortChevron(dc, rect, UiAccentInk(darkMode_), sftpSortAscending_);
            HPEN line = CreatePen(PS_SOLID, 1, colors.border);
            HPEN oldLine = reinterpret_cast<HPEN>(SelectObject(dc, line));
            MoveToEx(dc, rect.left, rect.bottom - 1, nullptr);
            LineTo(dc, rect.right, rect.bottom - 1);
            SelectObject(dc, oldLine);
            DeleteObject(line);
            return;
        }
        if (id == IdSftpTransferToggle) {
            HBRUSH background = CreateSolidBrush(hovered ? colors.accentSoft : colors.panel);
            FillRect(dc, &rect, background);
            DeleteObject(background);
            RECT textRect = rect;
            textRect.left += Ui::Space;
            textRect.right -= 28;
            DrawTextSimple(dc, ControlText(item.hwndItem), textRect, colors.text, smallFont_,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT arrow = rect;
            arrow.left = arrow.right - 28;
            DrawChevronDown(dc, arrow, colors.muted);
            if (!sftpTransferExpanded_) {
                // Rotate the disclosure affordance into a right-facing chevron.
                HBRUSH erase = CreateSolidBrush(hovered ? colors.accentSoft : colors.panel);
                FillRect(dc, &arrow, erase);
                DeleteObject(erase);
                DrawChevronHorizontal(dc, arrow, colors.muted, true);
            }
            return;
        }
        if (id == IDC_HOST_REVERIFY) {
            DrawRoundedBox(dc, rect, Ui::Radius, pressed ? colors.accent : hovered ? colors.panelAlt : colors.panel,
                           pressed ? colors.accent : hovered ? colors.panelAlt : colors.panel);
            DrawTextSimple(dc, ControlText(item.hwndItem), rect,
                !enabled ? colors.muted : pressed ? RGB(255, 255, 255) : UiAccentInk(darkMode_),
                smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            UiButtonKeyboardFocus(dc, rect, item.itemState, Ui::Dpi, UiFocusInk(darkMode_, enabled && pressed));
            return;
        }
        if (id == IdAddCommand || id == IDC_COMMAND_ADD_STEP) {
            const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered);
            DrawRoundedBox(dc, rect, Ui::Radius,
                           pressed   ? visual.fill
                           : hovered ? colors.panelAlt
                                     : colors.panel,
                           pressed ? visual.fill : colors.panel);
            UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
            RECT icon = rect;
            auto label = id == IdAddCommand && addCommandCompact_ ? std::wstring() : ControlText(item.hwndItem);
            if (!label.empty())
                icon.right = icon.left + Ui::Scale(32);
            const COLORREF ink = !enabled ? colors.muted : pressed ? visual.ink : UiAccentInk(darkMode_);
            DrawPlusIcon(dc, icon, ink);
            RECT textRect = rect;
            textRect.left += Ui::Scale(32);
            textRect.right -= Ui::Space;
            DrawTextSimple(dc, label, textRect, ink, uiFont_,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }

        const auto visual = UiButtonVisual(UiTheme(darkMode_), enabled, pressed, hovered, id == IDOK);
        DrawRoundedBox(dc, rect, Ui::Radius, visual.fill, visual.border);
        UiButtonKeyboardFocus(dc, rect, item.itemState, GetDeviceCaps(dc, LOGPIXELSX),
                UiFocusInk(darkMode_, enabled && (pressed || id == IDOK || id == IdSend)));
        if (id == IdAiWrap && rect.right - rect.left <= Ui::CompactHeight) {
            RECT glyph = rect;
            InflateRect(&glyph, -Ui::Scale(7), -Ui::Scale(7));
            UiDrawGlyph(dc, glyph, visual.ink, UiGlyph::Wrap);
            return;
        }
        const bool utility = id == IdMenuSaveLog || id == IdMenuClear || id == IdMenuCopy || id == IdMenuPaste ||
                             id == IdMenuSelectAll || id == IdSendSelectionToAi || id == IdMenuNetworkDiagnostics ||
                             id == IdMenuCancelLogExport;
        if (utility) {
            RECT text = rect;
            text.left += Ui::Scale(36);
            text.right -= Ui::Scale(8);
            const int cy = (rect.top + rect.bottom) / 2;
            RECT icon{rect.left + Ui::Scale(10), cy - Ui::Scale(9), rect.left + Ui::Scale(28), cy + Ui::Scale(9)};
            const auto color = visual.ink;
            if (id == IdSendSelectionToAi)
                toolbarIcons_.Draw(dc, ToolbarIcon::Ai, icon, darkMode_ || (pressed && enabled));
            else
                UiDrawGlyph(dc, icon, color,
                            id == IdMenuClear             ? UiGlyph::Trash
                            : id == IdMenuCopy            ? UiGlyph::Copy
                            : id == IdMenuPaste           ? UiGlyph::Paste
                            : id == IdMenuSelectAll       ? UiGlyph::SelectAll
                            : id == IdMenuCancelLogExport ? UiGlyph::Stop
                                                          : UiGlyph::Document);
            DrawTextSimple(dc, ControlText(item.hwndItem), text, color, uiFont_,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            DrawTextSimple(dc, ControlText(item.hwndItem), rect, visual.ink, uiFont_,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }

    HBRUSH background = CreateSolidBrush(colors.panel);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    if (item.CtlID == IdConnectionList) {
        const bool valid = item.itemID < sessions_.size();
        const SessionState* session = valid ? sessions_[item.itemID].get() : nullptr;
        const bool power=item.itemID==sessions_.size()&&powerPageOpened_;
        const bool ai=item.itemID==sessions_.size()+(powerPageOpened_?1:0)&&aiPageOpened_;
        const bool connected=ai?aiPane_.Connected():power?powerService_.State().value("connected",false):
            session&&session->connection&&session->connection->IsConnected();
        const bool active=ai?aiVisible_:power?powerVisible_:!powerVisible_&&!aiVisible_&&session==activeSession_;
        OverlayScrollMetrics scrollMetrics;
        RECT card{rect.left, rect.top + Ui::Gap / 2,
                  rect.right - (GetOverlayScrollMetrics(item.hwndItem, scrollMetrics) ? Ui::OverlayScrollLaneWidth : 0),
                  rect.bottom - (Ui::Gap - Ui::Gap / 2)};
        DrawRoundedBox(dc, card, Ui::CardRadius, active ? colors.accent : colors.panelAlt,
                       active ? colors.accent : colors.panelAlt);
        const int iconSize = Ui::Scale(18);
        RECT icon{card.left + Ui::Scale(12), card.top + ((card.bottom - card.top) - iconSize) / 2,
                  card.left + Ui::Scale(12) + iconSize,
                  card.top + ((card.bottom - card.top) - iconSize) / 2 + iconSize};
        const auto glyph = ai                              ? ToolbarIcon::Ai
                           : power                         ? ToolbarIcon::Power
                           : session && session->mode == 0 ? ToolbarIcon::Ssh
                           : session && session->mode == 1 ? ToolbarIcon::Serial
                           : session && session->mode == 2 ? ToolbarIcon::Telnet
                           : session && session->mode == 3 ? ToolbarIcon::RemoteSerial
                                                           : ToolbarIcon::Cmd;
        toolbarIcons_.Draw(dc, glyph, icon, darkMode_ || active);
        const int textLeft = icon.right + Ui::Space;
        RECT title{textLeft, card.top + Ui::Scale(3), card.right - Ui::Scale(8), card.top + Ui::Scale(25)};
        RECT subtitle{textLeft, card.top + Ui::Scale(25), card.right - Ui::Scale(8), card.bottom - Ui::Scale(3)};
        DrawTextSimple(dc,
                       ai        ? L"AI 助手"
                       : power   ? L"IT6332A"
                       : session ? session->name
                                 : L"暂无连接",
                       title, active ? RGB(255, 255, 255) : colors.text, smallFont_,
                       DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring state = connected ? L"已连接" : L"已断开";
        if (session && session->udpRaw && connected)
            state = session->udpReceived ? L"已接收数据" : L"等待 UDP 数据";
        if (connected && session && session->mode == 1)
            state = session->port ? L"共享 · TCP " + std::to_wstring(session->port) : L"本地可用";
        if (session && !session->host.empty())
            state = session->host + L" · " + state;
        else if (session && session->mode == 4)
            state = L"本机 · " + state;
        if (session && session->unreadStatus)
            state = (session->lastStatusIsError ? L"新错误 · " : L"新状态 · ") + state;
        RECT dot{textLeft, subtitle.top + Ui::Scale(6), textLeft + Ui::Scale(5), subtitle.top + Ui::Scale(11)};
        const bool unreadError = session && session->unreadStatus && session->lastStatusIsError;
        DrawStatusDot(dc, dot, active ? RGB(255, 255, 255) : unreadError ? colors.danger : colors.muted);
        subtitle.left += Ui::Scale(12);
        DrawTextSimple(dc, state, subtitle, active ? RGB(255, 255, 255) : unreadError ? colors.danger : colors.muted, smallFont_,
                       DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (item.CtlID == IdCommandList && item.itemID < commands_.size()) {
        const bool ghost = source.itemData == ~ULONG_PTR(0);
        const bool sourceDragging = !ghost && draggingCommandIndex_ == static_cast<int>(item.itemID);
        HBRUSH background = CreateSolidBrush(colors.panel);
        FillRect(dc, &rect, background);
        DeleteObject(background);
        const CommandItem& command = commands_[item.itemID];
        OverlayScrollMetrics scrollMetrics;
        const int rightInset = GetOverlayScrollMetrics(item.hwndItem, scrollMetrics) ?
            Ui::OverlayScrollLaneWidth : 0;
        RECT card{rect.left, rect.top + Ui::Gap / 2, rect.right - rightInset, rect.bottom - (Ui::Gap - Ui::Gap / 2)};
        const COLORREF cardFill = ghost ? (commandDragInside_ ? colors.accent : colors.muted)
                                         : sourceDragging ? colors.panel : colors.panelAlt;
        const COLORREF mainInk = ghost ? RGB(255, 255, 255) : sourceDragging ? colors.muted : colors.text;
        const COLORREF secondaryInk = ghost ? RGB(255, 255, 255) : colors.muted;
        DrawRoundedBox(dc, card, Ui::CardRadius, cardFill,
                       ghost ? cardFill : sourceDragging ? colors.border : selected ? colors.muted : colors.border);
        const int cy = (card.top + card.bottom) / 2;
        RECT marker{card.left + Ui::Scale(8), cy - Ui::Scale(8), card.left + Ui::Scale(24), cy + Ui::Scale(8)};
        UiDrawGlyph(dc, marker, secondaryInk, UiGlyph::Command);
        const CommandActionRects actions = GetCommandActionRects(card);
        RECT title{card.left + Ui::Scale(32), card.top + Ui::Scale(9), actions.run.left - Ui::Space, card.top + Ui::Scale(30)};
        RECT value{card.left + Ui::Scale(32), card.top + Ui::Scale(31), actions.run.left - Ui::Space, card.bottom - Ui::Scale(6)};
        DrawTextSimple(dc, command.name, title, mainInk, uiFont_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        const bool macro = command.commands.size() > 1;
        const std::wstring summary = ghost ?
            (commandDragInside_ && commandDropIndex_ >= 0 ? L"移至第 " + std::to_wstring(commandDropIndex_ + 1) + L" 项"
                                                          : L"松开取消") : macro ?
            L"宏 · " + std::to_wstring(command.commands.size()) + L" 条指令 · " +
                std::to_wstring(command.intervalMs) + L" ms" :
            (command.commands.empty() ? std::wstring() : command.commands.front());
        DrawTextSimple(dc, summary, value, secondaryInk, smallFont_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        const bool connected = connection_ && connection_->IsConnected();
        const bool sequenceRunning = runningCommandIndex_ >= 0;
        const bool isRunning = runningCommandIndex_ == static_cast<int>(item.itemID);
        const bool runnable = !sourceDragging && connected && (!sequenceRunning || isRunning);
        const bool editable = !sourceDragging && !sequenceRunning;
        const bool hoverRun = hoveredCommandIndex_ == static_cast<int>(item.itemID) && hoveredCommandAction_ == 1;
        const bool hoverEdit = hoveredCommandIndex_ == static_cast<int>(item.itemID) && hoveredCommandAction_ == 2;
        const bool runDown =
            runnable && pressedCommandIndex_ == static_cast<int>(item.itemID) && pressedCommandAction_ == 1;
        const bool editDown =
            editable && pressedCommandIndex_ == static_cast<int>(item.itemID) && pressedCommandAction_ == 2;
        DrawRoundedBox(dc, actions.run, Ui::Radius,
                       ghost                  ? cardFill
                       : runDown              ? colors.accent
                       : hoverRun && runnable ? colors.panel
                                              : cardFill,
                       ghost ? cardFill : runDown ? colors.accent : cardFill);
        DrawRoundedBox(dc, actions.edit, Ui::Radius,
                       ghost                   ? cardFill
                       : editDown              ? colors.accent
                       : hoverEdit && editable ? colors.panel
                                               : cardFill,
                       ghost ? cardFill : editDown ? colors.accent : cardFill);

        RECT runIcon = actions.run;
        InflateRect(&runIcon, -Ui::Scale(6), -Ui::Scale(6));
        UiDrawGlyph(dc, runIcon,
                    ghost || runDown ? RGB(255, 255, 255)
                    : runnable ? colors.text
                               : colors.muted,
                    isRunning ? UiGlyph::Stop : UiGlyph::Run);
        if (isRunning)
            DrawCommandProgress(dc, actions.run, submittedCommandSteps_, command.commands.size(),
                colors.border, colors.accent);
        DrawEditIcon(dc, actions.edit, ghost || editDown ? RGB(255, 255, 255) : editable ? colors.muted : colors.border);
        if (!ghost && draggingCommandIndex_ >= 0 && commandDragInside_ && commandInsertIndex_ >= 0) {
            const bool above = commandInsertIndex_ == static_cast<int>(item.itemID);
            const bool belowLast = commandInsertIndex_ == static_cast<int>(commands_.size()) &&
                item.itemID + 1 == commands_.size();
            if (above || belowLast) {
                const int y = above ? rect.top + Ui::Gap / 4 : rect.bottom - Ui::Gap / 4;
                HPEN pen = CreatePen(PS_SOLID, std::max(2, Ui::Scale(2)), colors.accent);
                HGDIOBJ previous = SelectObject(dc, pen);
                MoveToEx(dc, card.left + Ui::Scale(4), y, nullptr);
                LineTo(dc, card.right - Ui::Scale(4), y);
                SelectObject(dc, previous);
                DeleteObject(pen);
            }
        }
    } else if (item.CtlID == IdSftpList && item.itemID < sftpEntries_.size()) {
        const SftpEntry& entry = sftpEntries_[item.itemID];
        OverlayScrollMetrics scrollMetrics;
        const int rightInset = Ui::OverlayScrollLaneWidth;
        RECT row{rect.left, rect.top, rect.right - rightInset, rect.bottom};
        HBRUSH rowBrush = CreateSolidBrush(selected ? colors.accent : colors.panel);
        FillRect(dc, &row, rowBrush);
        DeleteObject(rowBrush);
        RECT tableClient{}; GetClientRect(sftpList_, &tableClient);
        const int contentWidth = std::max(1, static_cast<int>(tableClient.right) - rightInset);
        const bool showModified = contentWidth >= 300;
        int nameWidth, sizeWidth, modifiedWidth;
        SftpColumnWidths(contentWidth, nameWidth, sizeWidth, modifiedWidth);
        RECT icon{row.left + Ui::Space, row.top, row.left + Ui::Scale(32), row.bottom};
        DrawSftpEntryIcon(dc, icon,
                          selected          ? RGB(255, 255, 255)
                          : entry.directory ? colors.accent
                                            : colors.muted,
                          entry.directory, entry.symlink);
        RECT name{row.left + Ui::Scale(36), row.top, row.left + nameWidth - Ui::Space, row.bottom};
        DrawTextSimple(dc, entry.name, name, selected ? RGB(255, 255, 255) : colors.text, uiFont_,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!entry.directory) {
            RECT size{row.left + nameWidth, row.top,
                row.left + nameWidth + sizeWidth - Ui::Space, row.bottom};
            DrawTextSimple(dc, FileSizeText(entry.size), size, selected ? RGB(235, 240, 255) : colors.muted, smallFont_,
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (showModified) {
            RECT modified{row.left + nameWidth + sizeWidth, row.top,
                row.right - Ui::Space, row.bottom};
            DrawTextSimple(dc, entry.modifiedText, modified, selected ? RGB(235, 240, 255) : colors.muted, smallFont_,
                           DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        HPEN divider = CreatePen(PS_SOLID, 1, colors.border);
        HPEN oldDivider = reinterpret_cast<HPEN>(SelectObject(dc, divider));
        MoveToEx(dc, row.left, row.bottom - 1, nullptr);
        LineTo(dc, row.right, row.bottom - 1);
        SelectObject(dc, oldDivider);
        DeleteObject(divider);
    } else if (item.CtlID == IdSftpTransferList && item.itemID < sftpTransfers_.size()) {
        const SftpTransferItem& transfer = sftpTransfers_[item.itemID];
        OverlayScrollMetrics scrollMetrics;
        const int rightInset = GetOverlayScrollMetrics(item.hwndItem, scrollMetrics) ?
            Ui::OverlayScrollLaneWidth : 0;
        RECT row{rect.left, rect.top, rect.right - rightInset, rect.bottom};
        HBRUSH rowBrush = CreateSolidBrush(selected ? colors.accentSoft : colors.panel);
        FillRect(dc, &row, rowBrush);
        DeleteObject(rowBrush);

        RECT title{row.left + Ui::Space, row.top + 3, row.right - Ui::Space, row.top + 23};
        const std::wstring direction = transfer.direction == SftpTransferDirection::Upload ?
            L"上传 · " : L"下载 · ";
        DrawTextSimple(dc, direction + transfer.name, title, colors.text, smallFont_,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        std::wstring status;
        COLORREF statusColor = colors.muted;
        switch (transfer.state) {
        case SftpTransferState::Queued:
            status = L"等待中";
            break;
        case SftpTransferState::Running:
            if (!transfer.error.empty()) {
                status = transfer.error;
            } else {
                status = transfer.percent >= 0 ? std::to_wstring(transfer.percent) + L"%" : L"传输中";
                if (const std::wstring rate = TransferRateText(
                        transfer.bytesPerSecond, transfer.remainingSeconds); !rate.empty())
                    status += L" · " + rate;
                statusColor = colors.accent;
            }
            break;
        case SftpTransferState::Completed:
            status = L"已完成";
            statusColor = colors.accent;
            break;
        case SftpTransferState::Failed:
            status = transfer.error.empty() ? L"失败" : transfer.error;
            statusColor = colors.danger;
            break;
        case SftpTransferState::Canceled:
            status = L"已取消";
            break;
        }
        RECT subtitle{row.left + Ui::Space, row.top + 23, row.right - Ui::Space, row.bottom - 5};
        DrawTextSimple(dc, status, subtitle, statusColor, smallFont_,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (transfer.state == SftpTransferState::Running) {
            RECT track{row.left + Ui::Space, row.bottom - 4, row.right - Ui::Space, row.bottom};
            HBRUSH trackBrush = CreateSolidBrush(colors.border);
            FillRect(dc, &track, trackBrush);
            DeleteObject(trackBrush);
            RECT fill = track;
            const int trackWidth = std::max(1,
                static_cast<int>(track.right - track.left));
            if (transfer.percent >= 0) {
                fill.right = fill.left + trackWidth * std::max(0, std::min(100, transfer.percent)) / 100;
            } else {
                fill.right = fill.left + trackWidth / 3;
            }
            HBRUSH fillBrush = CreateSolidBrush(colors.accent);
            FillRect(dc, &fill, fillBrush);
            DeleteObject(fillBrush);
        }
    }
}

void MainWindow::ApplyTheme() {
    powerPane_.Theme(darkMode_);
    aiPane_.Theme(darkMode_);
    aiPet_.Theme(darkMode_);
    const Palette colors = Colors(darkMode_);
    if (windowBrush_) DeleteObject(windowBrush_);
    if (panelBrush_) DeleteObject(panelBrush_);
    if (terminalBrush_) DeleteObject(terminalBrush_);
    windowBrush_ = CreateSolidBrush(colors.window);
    panelBrush_ = CreateSolidBrush(colors.panel);
    terminalBrush_ = CreateSolidBrush(colors.terminal);
    ApplyDarkTitleBar(window_, darkMode_);
    const int iconId = darkMode_ ? IDI_APP_ICON : IDI_APP_ICON_LIGHT;
    HICON largeIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    HICON smallIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(iconId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    SetWindowTextW(themeButton_, darkMode_ ? L"浅色" : L"深色");
    // Theme changes can occur inside the theme button's native notification.
    // Commit parent and children together; do not leave the pressed/focused
    // button or an inset edit frame using the previous palette until focus loss.
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
}

void MainWindow::OpenConnectionDialog(int mode) {
    if (pendingSession_) {
        AppendStatus(L"正在连接，请等待当前连接完成。", false);
        return;
    }
    pendingMode_ = mode;
    pendingHostKeyReverify_ = false;
    if (UiDialogBoxOwned(instance_, MAKEINTRESOURCEW(IDD_CONNECTION), window_, ConnectionDialogProc,
                         reinterpret_cast<LPARAM>(this)) == IDOK) {
        ConnectFromDialog();
    }
    discoveryCancel_ = true;
    if (discoveryThread_.joinable()) discoveryThread_.join();
    discoveryDialog_ = nullptr;
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

INT_PTR CALLBACK MainWindow::ConnectionDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        ApplyDarkTitleBar(dialog, self->darkMode_);
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont_), TRUE);
            wchar_t className[32]{};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (_wcsicmp(className, L"Edit") == 0 || _wcsicmp(className, WC_COMBOBOXW) == 0) {
                if (_wcsicmp(className, L"Edit") == 0) {
                    PrepareDialogEditField(dialog, child, self);
                    SetWindowSubclass(child, DialogControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
                } else
                    UiStyleField(child, &self->dialogFieldStyle_);
                SetWindowPos(child, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            } else if (_wcsicmp(className, L"Button") == 0) {
                SetWindowSubclass(child, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
            }
        }
        self->ConfigureConnectionDialog(dialog);
        CenterOnOwner(dialog, self->window_);
        return TRUE;
    }
    if (!self) return FALSE;
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(dialog, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, self->panelBrush_);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX) {
        const Palette colors = Colors(self->darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetTextColor(dc, GetDlgCtrlID(control) == IDC_DIALOG_ERROR ? colors.danger :
                        GetDlgCtrlID(control) == IDC_HOST_IDENTITY ? colors.muted : colors.text);
        if (GetDlgCtrlID(control) == IDC_DIALOG_ERROR || GetDlgCtrlID(control) == IDC_HOST_IDENTITY)
            SelectObject(dc, self->smallFont_);
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
            SetBkColor(dc, colors.terminal);
            return reinterpret_cast<INT_PTR>(self->terminalBrush_);
        }
        SetBkColor(dc, colors.panel);
        return reinterpret_cast<INT_PTR>(self->panelBrush_);
    }
    if (message == WM_DRAWITEM) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_MEASUREITEM) {
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = Ui::Scale(28);
        return TRUE;
    }
    if (message == WM_CONTEXTMENU && self->pendingMode_ == 3 && reinterpret_cast<HWND>(wParam) == GetDlgItem(dialog, IDC_SERIAL_REFRESH)) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IdSharedAdvancedPort, L"高级：指定服务端口…");
        AppendMenuW(menu, MF_STRING, IdSharedAutoPort, L"恢复自动发现 7000–7015");
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1) { RECT rect{}; GetWindowRect(GetDlgItem(dialog, IDC_SERIAL_REFRESH), &rect); point = {rect.left, rect.bottom}; }
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, dialog, nullptr);
        DestroyMenu(menu);
        if (choice) PostMessageW(dialog, WM_COMMAND, choice, 0);
        return TRUE;
    }
    if (message == WM_TIMER && wParam == DiscoveryTimerId) {
        KillTimer(dialog, DiscoveryTimerId); self->DiscoverSharedSerialPorts(dialog); return TRUE;
    }
    if (message == WM_TIMER && wParam == SerialScanTimerId) {
        self->CompleteSerialPortScan(dialog); return TRUE;
    }
    if (message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (self->pendingMode_ == 0 &&
            (LOWORD(wParam) == IDC_HOST || LOWORD(wParam) == IDC_PORT) && HIWORD(wParam) == EN_CHANGE) {
            const auto host = Trim(ControlText(GetDlgItem(dialog, IDC_HOST)));
            const auto port = ParsePositive(ControlText(GetDlgItem(dialog, IDC_PORT)));
            const bool saved = port > 0 && port <= 65535 && !TrustedPuttyHostKey(host, static_cast<std::uint16_t>(port)).empty();
            SetDlgItemTextW(dialog, IDC_HOST_IDENTITY, saved ? L"主机身份已保存" : L"连接时核验主机身份");
        }
        if (self->pendingMode_ == 3 && (LOWORD(wParam) == IdSharedAdvancedPort || LOWORD(wParam) == IdSharedAutoPort)) {
            if (LOWORD(wParam) == IdSharedAdvancedPort) {
                std::wstring value;
                if (!self->PromptSftpValue(L"高级共享端口", L"服务端口（通常使用自动发现）", std::to_wstring(self->discoveryExplicitPort_ ? self->discoveryExplicitPort_ : DefaultSharePort), SftpInputPurpose::Port, value, dialog)) return TRUE;
                self->discoveryExplicitPort_ = static_cast<std::uint16_t>(ParsePositive(value));
            } else self->discoveryExplicitPort_ = 0;
            ++self->discoveryGeneration_; self->discoveryCancel_ = true;
            EnableWindow(GetDlgItem(dialog, IDOK), FALSE); SetTimer(dialog, DiscoveryTimerId, 100, nullptr); return TRUE;
        }
        if (self->pendingMode_ == 3 && LOWORD(wParam) == IDC_HOST && HIWORD(wParam) == EN_CHANGE) {
            ++self->discoveryGeneration_; self->discoveryCancel_ = true;
            EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
            SetTimer(dialog, DiscoveryTimerId, 500, nullptr); return TRUE;
        }
        if (LOWORD(wParam) == IDC_SERIAL_REFRESH && HIWORD(wParam) == BN_CLICKED) {
            if (self->pendingMode_ == 3)
                self->DiscoverSharedSerialPorts(dialog);
            else
                self->RefreshSerialPorts(dialog);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDC_HOST_REVERIFY) {
            std::wstring error;
            if (!self->ReadConnectionDialog(dialog, error)) {
                SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, error.c_str());
                InvalidateRect(GetDlgItem(dialog, IDC_DIALOG_ERROR), nullptr, TRUE);
                return TRUE;
            }
            self->pendingHostKeyReverify_ = LOWORD(wParam) == IDC_HOST_REVERIFY;
            UiEndOwnedDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            UiEndOwnedDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::ConfigureConnectionDialog(HWND dialog) {
    const bool serial = pendingMode_ == 1;
    const bool ssh = pendingMode_ == 0;
    const bool share = pendingMode_ == 3;
    ShowDialogItem(dialog, IDC_HOST_IDENTITY, ssh);
    ShowDialogItem(dialog, IDC_HOST_REVERIFY, ssh);
    discoveryExplicitPort_ = 0;
    pendingRemoteInstance_.clear();
    const int serialSelectorIds[] = {IDC_SERIAL_LABEL, IDC_SERIAL};
    const int serialSettingIds[] = {IDC_BAUD_LABEL, IDC_BAUD, IDC_DATABITS_LABEL, IDC_DATABITS,
        IDC_PARITY_LABEL, IDC_PARITY, IDC_STOPBITS_LABEL, IDC_STOPBITS, IDC_FLOW_LABEL, IDC_FLOW};
    const int networkIds[] = {IDC_HOST_LABEL, IDC_HOST, IDC_PORT_LABEL, IDC_PORT};
    for (int id : serialSelectorIds) ShowDialogItem(dialog, id, serial || share);
    for (int id : serialSettingIds) ShowDialogItem(dialog, id, serial);
    for (int id : networkIds) ShowDialogItem(dialog, id, !serial);
    for (int id : {IDC_USER_LABEL, IDC_USER, IDC_PASSWORD_LABEL, IDC_PASSWORD})
        ShowDialogItem(dialog, id, ssh);
    ShowDialogItem(dialog, IDC_SHARE_LABEL, false);
    ShowDialogItem(dialog, IDC_SHARE_PORT, false);
    ShowDialogItem(dialog, IDC_SERIAL_REFRESH, serial || share);
    ShowDialogItem(dialog, IDC_DIALOG_NOTE, false);
    if (share) { ShowDialogItem(dialog, IDC_PORT_LABEL, false); ShowDialogItem(dialog, IDC_PORT, false); }

    constexpr int dialogWidth = 238;
    constexpr int margin = 12;
    constexpr int columnWidth = 101;
    constexpr int rightColumn = 125;
    constexpr int fullWidth = dialogWidth - margin * 2;
    if (serial) {
        MoveDialogItemDlu(dialog, IDC_SERIAL_LABEL, margin, 10, 64, 10);
        MoveDialogItemDlu(dialog, IDC_SERIAL, margin, 22, 174, 154);
        MoveDialogItemDlu(dialog, IDC_SERIAL_REFRESH, 194, 22, 32, 14);
        MoveDialogItemDlu(dialog, IDC_BAUD_LABEL, margin, 44, 64, 10);
        MoveDialogItemDlu(dialog, IDC_BAUD, margin, 56, columnWidth, 140);
        MoveDialogItemDlu(dialog, IDC_DATABITS_LABEL, rightColumn, 44, 64, 10);
        MoveDialogItemDlu(dialog, IDC_DATABITS, rightColumn, 56, columnWidth, 84);
        MoveDialogItemDlu(dialog, IDC_PARITY_LABEL, margin, 78, 64, 10);
        MoveDialogItemDlu(dialog, IDC_PARITY, margin, 90, columnWidth, 72);
        MoveDialogItemDlu(dialog, IDC_STOPBITS_LABEL, rightColumn, 78, 64, 10);
        MoveDialogItemDlu(dialog, IDC_STOPBITS, rightColumn, 90, columnWidth, 72);
        MoveDialogItemDlu(dialog, IDC_FLOW_LABEL, margin, 112, 64, 10);
        MoveDialogItemDlu(dialog, IDC_FLOW, margin, 124, columnWidth, 72);
    } else {
        MoveDialogItemDlu(dialog, IDC_HOST_LABEL, margin, 10, columnWidth, 10);
        MoveDialogItemDlu(dialog, IDC_HOST, margin, 22, columnWidth, 14);
        MoveDialogItemDlu(dialog, IDC_PORT_LABEL, rightColumn, 10, columnWidth, 10);
        MoveDialogItemDlu(dialog, IDC_PORT, rightColumn, 22, columnWidth, 14);
        if (ssh) {
            MoveDialogItemDlu(dialog, IDC_USER_LABEL, margin, 44, columnWidth, 10);
            MoveDialogItemDlu(dialog, IDC_USER, margin, 56, columnWidth, 14);
            MoveDialogItemDlu(dialog, IDC_PASSWORD_LABEL, rightColumn, 44, columnWidth, 10);
            MoveDialogItemDlu(dialog, IDC_PASSWORD, rightColumn, 56, columnWidth, 14);
        } else if (share) {
            SetDlgItemTextW(dialog, IDC_HOST_LABEL, L"远端 IP");
            MoveDialogItemDlu(dialog, IDC_HOST, margin, 22, 174, 14);
            SetDlgItemTextW(dialog, IDC_SERIAL_LABEL, L"远端串口");
            MoveDialogItemDlu(dialog, IDC_SERIAL_LABEL, margin, 44, columnWidth, 10);
            MoveDialogItemDlu(dialog, IDC_SERIAL, margin, 56, fullWidth, 154);
            MoveDialogItemDlu(dialog, IDC_SERIAL_REFRESH, 194, 22, 32, 14);
        }
    }

    int errorY = 106;
    int buttonY = 118;
    int dialogHeight = 144;
    if (serial) {
        errorY = 140;
        buttonY = 152;
        dialogHeight = 178;
    } else if (ssh) {
        MoveDialogItemDlu(dialog, IDC_HOST_IDENTITY, margin, 76, 120, 14);
        MoveDialogItemDlu(dialog, IDC_HOST_REVERIFY, 144, 72, 82, 18);
        errorY = 94;
        buttonY = 106;
        dialogHeight = 132;
    } else if (share) {
        errorY = 72;
        buttonY = 84;
        dialogHeight = 110;
    } else if (!serial && !share) {
        errorY = 38;
        buttonY = 50;
        dialogHeight = 76;
    }
    MoveDialogItemDlu(dialog, IDC_DIALOG_ERROR, margin, errorY, fullWidth, 12);
    MoveDialogItemDlu(dialog, IDCANCEL, dialogWidth - 156, buttonY, 68, 18);
    MoveDialogItemDlu(dialog, IDOK, dialogWidth - 80, buttonY, 68, 18);
    ResizeDialogClientDlu(dialog, dialogWidth, dialogHeight);
    FitDialogLabels(dialog);

    const wchar_t* titles[] = {L"新建 SSH 连接", L"新建串口连接", L"新建 Telnet 连接", L"连接共享串口"};
    SetWindowTextW(dialog, titles[pendingMode_]);
    ShowWindow(GetDlgItem(dialog, IDC_DIALOG_NOTE), SW_HIDE);
    SetDlgItemTextW(dialog, IDC_HOST, aiRemoteConnect_ ? L"" : L"127.0.0.1");
    SetDlgItemTextW(dialog, IDC_PORT, pendingMode_ == 0 ? L"22" : pendingMode_ == 2 ? L"23" : L"7000");
    SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, L"");

    for (int id : {IDC_HOST, IDC_PORT, IDC_USER, IDC_PASSWORD, IDC_SHARE_PORT})
        SendDlgItemMessageW(dialog, id, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));

    if (serial) {
        RefreshSerialPorts(dialog);
    } else if (share) {
        HWND ports = GetDlgItem(dialog, IDC_SERIAL);
        SendMessageW(ports, CB_RESETCONTENT, 0, 0);
        SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"请先查询远端串口"));
        SendMessageW(ports, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
        discoveryDialog_ = dialog;
        SetTimer(dialog, DiscoveryTimerId, 500, nullptr);
    }
    AddComboItems(GetDlgItem(dialog, IDC_BAUD), {L"9600", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600", L"1500000"}, 3);
    AddComboItems(GetDlgItem(dialog, IDC_DATABITS), {L"5", L"6", L"7", L"8"}, 3);
    AddComboItems(GetDlgItem(dialog, IDC_PARITY), {L"无", L"奇", L"偶"}, 0);
    AddComboItems(GetDlgItem(dialog, IDC_STOPBITS), {L"1", L"1.5", L"2"}, 0);
    AddComboItems(GetDlgItem(dialog, IDC_FLOW), {L"无", L"CTS/RTS", L"XON/XOFF"}, 0);
    UiLayoutBatch fields;
    for (int id : {IDC_SERIAL, IDC_BAUD, IDC_DATABITS, IDC_PARITY, IDC_STOPBITS, IDC_FLOW})
        fields.ComboVisibleHeight(GetDlgItem(dialog, id), Ui::Scale(28));
    fields.Commit();
    if (serial || share)
        MatchDialogControlHeight(dialog, share ? IDC_HOST : IDC_SERIAL, IDC_SERIAL_REFRESH);
}

void MainWindow::RefreshSerialPorts(HWND dialog) {
    if (!serialPortScan_ || serialPortScan_->done) {
        serialPortScan_ = std::make_shared<SerialPortScan>();
        // The job owns no HWND or MainWindow reference. Closing a dialog cannot
        // block on a slow SetupAPI driver or deliver to a recycled dialog handle.
        std::thread([job = serialPortScan_] {
            try { job->ports = EnumerateSerialPorts(); }
            catch (...) { job->error = L"串口枚举失败，请检查设备后刷新。"; }
            job->done = true;
        }).detach();
    }
    HWND ports = GetDlgItem(dialog, IDC_SERIAL);
    SendMessageW(ports, CB_RESETCONTENT, 0, 0);
    SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"正在查找串口…"));
    SendMessageW(ports, CB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_REFRESH), FALSE);
    SetTimer(dialog, SerialScanTimerId, 40, nullptr);
}
void MainWindow::CompleteSerialPortScan(HWND dialog) {
    if (!serialPortScan_ || !serialPortScan_->done) return;
    KillTimer(dialog, SerialScanTimerId);
    HWND ports = GetDlgItem(dialog, IDC_SERIAL);
    SendMessageW(ports, CB_RESETCONTENT, 0, 0);
    serialPorts_ = serialPortScan_->ports;
    int count = 0;
    for (const auto &port : serialPorts_) {
        auto label = port.friendlyName;
        if (label.find(port.port) == std::wstring::npos)
            label += L" (" + port.port + L")";
        SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        ++count;
    }
    if (count == 0) {
        SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未检测到串口"));
    }
    SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, serialPortScan_->error.c_str());
    SendMessageW(ports, CB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), !((pendingMode_ == 1 || pendingMode_ == 3) && count == 0));
    EnableWindow(GetDlgItem(dialog, IDC_SERIAL_REFRESH), TRUE);
    InvalidateRect(ports, nullptr, TRUE);
    InvalidateRect(GetDlgItem(dialog, IDC_DIALOG_ERROR), nullptr, TRUE);
}

void MainWindow::DiscoverSharedSerialPorts(HWND dialog) {
    if (!discoveryFinished_) { SetTimer(dialog, DiscoveryTimerId, 100, nullptr); return; }
    if (discoveryThread_.joinable()) discoveryThread_.join();
    const std::wstring host = Trim(ControlText(GetDlgItem(dialog, IDC_HOST)));
    if (host.empty()) return;
    IN_ADDR ipv4{}; IN6_ADDR ipv6{};
    if (InetPtonW(AF_INET, host.c_str(), &ipv4) != 1 && InetPtonW(AF_INET6, host.c_str(), &ipv6) != 1) {
        SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, L"请输入完整的 IP 地址"); return;
    }
    discoveryDialog_ = dialog;
    discoveryCancel_ = false;
    discoveryFinished_ = false;
    const unsigned generation = ++discoveryGeneration_;
    const auto explicitPort = discoveryExplicitPort_;
    EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, L"正在自动发现串口…");
    discoveryThread_ = std::thread([this, host, dialog, generation, explicitPort] {
        auto result = std::make_unique<DiscoveryMessage>();
        result->dialog = dialog;
        result->generation = generation;
        try {
            std::vector<std::wstring> descriptions;
            result->success = SharedSerialConnection::DiscoverAuto(
                                                    host, result->port, result->names, result->error, &discoveryCancel_,
                                                    explicitPort, &descriptions, &result->instance);
            if (result->success)
                result->names = std::move(descriptions);
        } catch (...) {
            result->error = L"串口查询失败";
        }
        discoveryFinished_ = true;
        if (PostMessageW(window_, MessageDiscovery, 0, reinterpret_cast<LPARAM>(result.get())))
            result.release();
    });
}

bool MainWindow::ReadConnectionDialog(HWND dialog, std::wstring& error) {
    auto text = [dialog](int id) { return Trim(ControlText(GetDlgItem(dialog, id))); };
    if (pendingMode_ == 1) {
        const auto selected = SendMessageW(GetDlgItem(dialog, IDC_SERIAL), CB_GETCURSEL, 0, 0);
        if (selected < 0 || static_cast<size_t>(selected) >= serialPorts_.size()) {
            error = L"请选择有效串口";
            return false;
        }
        pendingSerial_.portName = serialPorts_[selected].port;
        pendingSerial_.friendlyName = serialPorts_[selected].friendlyName;
        pendingSerial_.baudRate = ParsePositive(text(IDC_BAUD));
        if (pendingSerial_.portName.empty() || pendingSerial_.baudRate == 0) {
            error = L"请选择串口并填写正确的波特率。";
            return false;
        }
        pendingSerial_.dataBits = static_cast<BYTE>(5 + SendDlgItemMessageW(dialog, IDC_DATABITS, CB_GETCURSEL, 0, 0));
        const int parity = static_cast<int>(SendDlgItemMessageW(dialog, IDC_PARITY, CB_GETCURSEL, 0, 0));
        pendingSerial_.parity = parity == 1 ? ODDPARITY : parity == 2 ? EVENPARITY : NOPARITY;
        const int stop = static_cast<int>(SendDlgItemMessageW(dialog, IDC_STOPBITS, CB_GETCURSEL, 0, 0));
        pendingSerial_.stopBits = stop == 1 ? ONE5STOPBITS : stop == 2 ? TWOSTOPBITS : ONESTOPBIT;
        pendingSerial_.flowControl = static_cast<DWORD>(SendDlgItemMessageW(dialog, IDC_FLOW, CB_GETCURSEL, 0, 0));
        return true;
    }
    pendingHost_ = text(IDC_HOST);
    pendingUsername_ = text(IDC_USER);
    pendingPassword_ = ControlText(GetDlgItem(dialog, IDC_PASSWORD));
    const DWORD port = ParsePositive(text(IDC_PORT));
    if (pendingHost_.empty() || port == 0 || port > 65535) {
        error = L"主机或端口无效。";
        return false;
    }
    if (pendingMode_ == 0 && pendingUsername_.empty()) {
        error = L"SSH 用户名不能为空。";
        return false;
    }
    if (pendingMode_ == 3) {
        pendingRemoteSerial_ = text(IDC_SERIAL);
        pendingRemoteLabel_ = pendingRemoteSerial_;
        const auto description = pendingRemoteSerial_.find(L" · ");
        if (description != std::wstring::npos) pendingRemoteSerial_.erase(description);
        if (pendingRemoteSerial_.empty() || pendingRemoteSerial_ == L"请先查询远端串口" ||
            pendingRemoteSerial_ == L"未发现可连接的串口" ||
            pendingRemoteSerial_ == L"请先填写正确的 IP 和端口") {
            error = L"请先查询并选择远端串口。";
            return false;
        }
    }
    pendingPort_ = static_cast<std::uint16_t>(port);
    return true;
}

void MainWindow::ConnectFromDialog() {
    std::unique_ptr<IConnection> connection;
    std::wstring sessionName;
    if (pendingMode_ == 0) {
        connection = std::make_unique<PlinkConnection>(pendingHost_, pendingPort_, pendingUsername_,
            pendingPassword_, terminalVisibleColumns_, terminalVisibleRows_,
            [this](const HostKeyChallenge& challenge) {
                if (connectionCancel_.load()) return false;
                auto request = std::make_shared<HostKeyRequest>();
                request->challenge = challenge;
                auto decision = request->decision.get_future();
                auto* message = new HostKeyMessage(request);
                if (!PostMessageW(window_, MessageHostKey, 0, reinterpret_cast<LPARAM>(message))) {
                    delete message;
                    return false;
                }
                while (decision.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
                    if (connectionCancel_.load()) return false;
                }
                return !connectionCancel_.load() && decision.get();
            }, pendingHostKeyReverify_);
        sessionName = pendingUsername_ + L"@" + pendingHost_;
    } else if (pendingMode_ == 1) {
        connection = std::make_unique<SerialConnection>(pendingSerial_);
        sessionName = pendingSerial_.friendlyName.empty() ? pendingSerial_.portName : pendingSerial_.friendlyName;
    } else if (pendingMode_ == 2) {
        connection = std::make_unique<TcpConnection>(pendingHost_, pendingPort_, true);
        sessionName = pendingHost_ + L":" + std::to_wstring(pendingPort_);
    } else if (pendingMode_ == 4) {
        connection = std::make_unique<CmdConnection>(); sessionName = L"本地 CMD";
    } else {
        connection = std::make_unique<SharedSerialConnection>(pendingHost_, pendingPort_, pendingRemoteSerial_,
                                                              pendingRemoteInstance_);
        sessionName = pendingRemoteLabel_ + L" @ " + pendingHost_;
    }

    auto session = std::make_unique<SessionState>();
    session->terminal.Resize(terminalVisibleColumns_, terminalVisibleRows_);
    session->id = nextSessionId_++;
    session->mode = pendingMode_;
    // Only the desktop that owns the local COM answers terminal queries.
    // A shared viewer receives live output and history through the same Feed
    // path, so neither may synthesize additional device input. SSH/Telnet keep
    // their own replies; CMD retains its existing unsupported-query warning.
    session->terminal.SetResponseRole(session->mode == 3 ? TerminalModel::ResponseRole::Viewer
                                                       : TerminalModel::ResponseRole::Owner);
    if (pendingMode_ == 4) { session->codePage = GetOEMCP(); session->lineEndingIndex = 2; }
    session->lineEndingIndex = pendingMode_ == 0 ? 1 : (pendingMode_ == 4 ? 2 : 0);
    session->name = sessionName + (pendingMode_ == 4 ? L" #" + std::to_wstring(session->id) : L"");
    session->host = pendingHost_;
    if (session->mode == 3 && !session->udpRaw)
        session->remoteCom = pendingRemoteSerial_;
    if (session->mode == 1)
        session->remoteCom = pendingSerial_.portName;
    session->username = pendingUsername_;
    session->password = pendingPassword_;
    session->port = pendingPort_;
    session->logger = std::make_shared<SessionLogger>();
    const std::uint64_t sessionId = session->id;

    session->connection = std::make_shared<QueuedConnection>(std::move(connection));
    if (session->mode == 1 || session->mode == 4) {
        Json resource = {{"id", "session-" + std::to_string(session->id)},
                         {"name", WideToMultiByte(session->name, CP_UTF8)},
                         {"kind", session->mode == 1 ? "serial" : "cmd"},
                         {"codePage", session->codePage}};
        if (session->mode == 1) {
            resource["com"] = WideToMultiByte(pendingSerial_.portName, CP_UTF8);
            resource["baudRate"] = pendingSerial_.baudRate;
            resource["friendlyName"] = WideToMultiByte(pendingSerial_.friendlyName, CP_UTF8);
        }
        sessionService_.Register(resource, session->connection);
        apiServer_.SetSessions(sessionService_.Ids());
    }
    pendingSession_ = std::move(session);
    pendingConnectionData_.clear();
    pendingConnectionBytes_ = 0;
    connectionCancel_.store(false);
    AppendStatus(L"正在连接：" + pendingSession_->name, false);
    SessionState* pending = pendingSession_.get();
    try {
        connectionThread_ = std::thread([this, pending, sessionId] {
            auto result = std::make_unique<ConnectionMessage>();
            try {
                if (!connectionCancel_.load() && pending->logger->Start(pending->name, result->error)) {
                    result->success = pending->connection->Start(
                        [this, sessionId, boot = pending->boot, transport = std::weak_ptr<IConnection>(pending->connection)](const Bytes& data) {
                            const auto bytes = boot->Feed(reinterpret_cast<const char*>(data.data()), data.size(), GetTickCount64());
                            if (!bytes.empty()) {
                                std::wstring error;
                                const auto connection = transport.lock();
                                const bool accepted = connection && connection->IsConnected() &&
                                    connection->Send(Bytes(bytes.begin(), bytes.end()), error);
                                boot->Delivery(accepted, WideToMultiByte(error, CP_UTF8));
                            }
                            PostData(sessionId, data);
                        },
                        [this, sessionId](const std::wstring& text, bool isError) {
                            PostStatus(sessionId, text, isError);
                        }, result->error);
                }
                if (connectionCancel_.load()) result->success = false;
                if (!result->success) {
                    pending->connection->Stop();
                    pending->logger->Stop();
                }
            } catch (...) {
                result->success = false;
                result->error = L"连接初始化失败。";
                pending->connection->Stop();
                pending->logger->Stop();
            }
            if (PostMessageW(window_, MessageConnection, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    } catch (...) {
        sessionService_.Unregister("session-" + std::to_string(sessionId));
        apiServer_.SetSessions(sessionService_.Ids());
        pendingSession_.reset();
        AppendStatus(L"无法启动连接工作线程。", true);
    }
}

void MainWindow::CompleteConnection(bool success, const std::wstring& error) {
    if (connectionThread_.joinable()) connectionThread_.join();
    if (!pendingSession_) return;
    if (!success || closing_ || connectionCancel_.load()) {
        aiRemoteConnect_ = false;
        if (pendingSession_->mode == 0 && ai::IsAuthenticationFailure(error)) {
            SecureZeroMemory(pendingPassword_.data(), pendingPassword_.size() * sizeof(wchar_t));
            pendingPassword_.clear();
            SecureZeroMemory(pendingSession_->password.data(), pendingSession_->password.size() * sizeof(wchar_t));
            pendingSession_->password.clear();
        }
        sessionService_.Unregister("session-" + std::to_string(pendingSession_->id));
        apiServer_.SetSessions(sessionService_.Ids());
        pendingSession_.reset();
        pendingConnectionData_.clear();
        pendingConnectionBytes_ = 0;
        if (!closing_) AppendStatus(error.empty() ? L"连接已取消。" : error, true);
        return;
    }
    if (pendingSession_->mode == 1)
        pendingSession_->port = static_cast<std::uint16_t>(apiServer_.Port());
    const std::uint64_t id = pendingSession_->id;
    sessions_.push_back(std::move(pendingSession_));
    RefreshConnectionList();
    const bool interruptedMacro = runningCommandIndex_ >= 0;
    SwitchSession(sessions_.size() - 1);
    const std::wstring macroStatus = interruptedMacro ? statusText_ : L"";
    for (const auto &item : pendingConnectionData_) {
        if (item.source.empty())
            AppendData(id, item.data, item.codePage, item.arrival);
        else
            AppendInput(item);
    }
    pendingConnectionData_.clear();
    pendingConnectionBytes_ = 0;
    // A new connection switches the active session. Do not replace a macro's
    // unknown execution outcome with the ordinary connection success message.
    if (interruptedMacro) {
        AppendStatus(macroStatus, true);
    } else if (activeSession_->mode == 1) {
        AppendStatus(activeSession_->port
                         ? L"已连接：" + activeSession_->name + L" · WebSocket " + std::to_wstring(activeSession_->port)
                         : L"本地串口已连接；7000–7015 均不可用，网络服务未启用",
                     activeSession_->port == 0);
    } else AppendStatus(L"已连接：" + activeSession_->name, false);
    if (aiRemoteConnect_) {
        aiRemoteConnect_ = false;
        aiPane_.SelectTarget("session-" + std::to_string(id));
        ShowAiPage(false);
    } else SetFocus(terminal_);
}

void MainWindow::Disconnect() {
    if (!activeSession_) return;
    const bool interruptedMacro = runningCommandIndex_ >= 0 && !closing_;
    StopCommandSequence(interruptedMacro ? CommandSequenceEnd::Interrupted : CommandSequenceEnd::Silent,
                        L"用户断开当前连接");
    const std::wstring macroStatus = interruptedMacro ? statusText_ : L"";
    const std::uint64_t disconnectedSessionId = activeSession_->id;
    if (sftpOperationSessionId_ == disconnectedSessionId)
        sftpOperationCancel_.store(true);
    for (SftpTransferItem& item : sftpTransfers_) {
        if (item.sessionId != disconnectedSessionId) continue;
        if (item.state == SftpTransferState::Queued) {
            item.state = SftpTransferState::Canceled;
            item.error = L"连接已断开";
        } else if (item.state == SftpTransferState::Running) {
            sftpTransferCancel_.store(true);
        }
    }
    RefreshSftpTransferList();
    const auto found = std::find_if(sessions_.begin(), sessions_.end(),
        [this](const std::unique_ptr<SessionState>& item) { return item.get() == activeSession_; });
    if (found == sessions_.end()) return;
    const size_t index = static_cast<size_t>(found - sessions_.begin());
    AiDeviceRequest("release", {{"sessionId", "session-" + std::to_string((*found)->id)}});
    sessionService_.Unregister("session-" + std::to_string((*found)->id));
    apiServer_.SetSessions(sessionService_.Ids());
    (*found)->connection->Stop();
    (*found)->logger->Stop();
    sessions_.erase(found);
    activeSession_ = nullptr;
    connection_ = nullptr;
    logger_ = nullptr;
    activeConnectionName_.clear();
    selectedMode_ = -1;
    RefreshConnectionList();
    if (!sessions_.empty() && !closing_ && !disconnectAllInProgress_)
        SwitchSession(std::min(index, sessions_.size() - 1));
    else {
        ClearTerminalSelection();
        InvalidateRect(terminal_, nullptr, TRUE);
        ShowSftpPanel(false);
        sftpEntries_.clear();
        sftpDirectory_ = L".";
        SetConnectedUi(false);
    }
    AppendStatus(interruptedMacro ? macroStatus : L"已断开", interruptedMacro);
}

void MainWindow::DisconnectAll() {
    aiPane_.Disconnect();
    aiVisible_ = false; aiPageOpened_ = false;
    disconnectAllInProgress_ = true;
    const bool interruptedMacro = runningCommandIndex_ >= 0 && !closing_;
    StopCommandSequence(interruptedMacro ? CommandSequenceEnd::Interrupted : CommandSequenceEnd::Silent,
                        L"用户断开全部连接");
    const std::wstring macroStatus = interruptedMacro ? statusText_ : L"";
    connectionCancel_ = true;
    if (pendingSession_) {
        pendingSession_->connection->CancelStart();
        sessionService_.Unregister("session-" + std::to_string(pendingSession_->id));
        apiServer_.SetSessions(sessionService_.Ids());
    }
    sftpOperationCancel_ = true;
    sftpTransferCancel_ = true;
    while (!sessions_.empty()) {
        activeSession_ = sessions_.back().get();
        connection_ = activeSession_->connection.get();
        logger_ = activeSession_->logger.get();
        Disconnect();
    }
    if (connectionThread_.joinable())
        connectionThread_.join();
    pendingSession_.reset();
    pendingConnectionData_.clear();
    pendingConnectionBytes_ = 0;
    {
        std::lock_guard<std::mutex> lock(receivedMutex_);
        received_.clear();
        receivedBytes_.clear();
        receiveOverflow_.clear();
    }
    disconnectAllInProgress_ = false;
    if (sftpThread_.joinable())
        sftpThread_.join();
    if (sftpTransferThread_.joinable())
        sftpTransferThread_.join();
    if (powerService_.State().value("connected", false)) {
        auto action = powerService_.Submit({{"type", "disconnect"}, {"source", "local"}});
        if (action.contains("error")) {
            AppendStatus(L"电源断开请求失败；输出状态未知" +
                         (interruptedMacro ? L"；" + macroStatus : L""), true);
            return;
        }
        disconnectAllPending_ = true;
    } else {
        powerPageOpened_ = false;
        powerVisible_ = false;
    }
    RefreshConnectionList();
    RECT r{};
    GetClientRect(window_, &r);
    LayoutControls(r.right, r.bottom);
    const std::wstring disconnected = disconnectAllPending_ ?
        L"会话已断开；电源任务收尾与掉电确认处理中" : L"全部连接已断开并清空";
    AppendStatus(disconnected + (interruptedMacro ? L"；" + macroStatus : L""), interruptedMacro);
}

MainWindow::SessionState* MainWindow::FindSession(std::uint64_t id) {
    const auto found = std::find_if(sessions_.begin(), sessions_.end(),
        [id](const std::unique_ptr<SessionState>& item) { return item->id == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

void MainWindow::ShowAiPage(bool connect) {
    KillTimer(window_, SideAnimationTimerId);
    rightPanelAnimating_ = false; rightPanelAnimatedWidth_ = -1;
    powerVisible_ = false; aiVisible_ = true; aiPageOpened_ = true;
    RefreshConnectionList();
    RECT client{}; GetClientRect(window_, &client); LayoutControls(client.right, client.bottom);
    InvalidateRect(window_, nullptr, TRUE);
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);
    SyncAiTerminal();
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (connect && !aiPane_.Connected()) aiPane_.ShowConnection();
}

Json MainWindow::SendAiBytes(SessionState &session, const Bytes &bytes) {
    if (bytes.empty() || bytes.size() > 65536 || !session.connection || !session.connection->IsConnected())
        return {{"error", "Terminal is disconnected or input exceeds its bound"}};
    const auto id = "session-" + std::to_string(session.id);
    if (session.mode == 1 || session.mode == 4)
        return sessionService_.Input(id, bytes, "ai:desktop");
    std::wstring error;
    if (!session.connection->Send(bytes, error)) return {{"error", WideToMultiByte(error, CP_UTF8)}};
    return {{"accepted", bytes.size()}, {"state", "queued"}, {"execution", "unknown"},
        {"scope", "existing-selected-terminal-connection"}, {"automaticRetry", false}};
}

Json MainWindow::AiDeviceRequest(const std::string& method, const Json& params) {
    auto error = [](const std::string& text) -> Json { return {{"error", text}}; };
    if (method == "connectRemote") {
        if (pendingSession_) return error("Another connection is being established");
        aiRemoteConnect_ = true;
        OpenConnectionDialog(3);
        if (!pendingSession_) aiRemoteConnect_ = false;
        return {{"pending", pendingSession_ != nullptr}};
    }
    if (method == "sessions") {
        Json result = Json::array();
        for (const auto& session : sessions_)
            if (*ai::TerminalKind(session->mode) && session->connection && session->connection->IsConnected())
                result.push_back(
                    {{"id", "session-" + std::to_string(session->id)},
                     {"instance", apiServer_.Instance()},
                     {"name", std::string("[") + ai::TerminalKind(session->mode) + "] " +
                                  WideToMultiByte(session->name, CP_UTF8)},
                     {"kind", ai::TerminalKind(session->mode)},
                     {"username", session->mode == 0 ? WideToMultiByte(session->username,CP_UTF8) : ""},
                     {"codePage", session->connection->OutputCodePage() ? session->connection->OutputCodePage()
                                                                        : session->codePage},
                     {"inputCodePage",
                      session->connection->InputCodePage() ? session->connection->InputCodePage() : session->codePage},
                     {"remoteIp", session->mode == 0 || session->mode == 2 || session->mode == 3
                                      ? WideToMultiByte(session->host, CP_UTF8)
                                      : ""},
                     {"remotePort", session->mode == 0 || session->mode == 2 || session->mode == 3 ? session->port : 0},
                     {"remoteCom", WideToMultiByte(session->remoteCom, CP_UTF8)},
                     {"com", WideToMultiByte(session->remoteCom, CP_UTF8)},
                     {"transport", session->udpRaw ? "udpRaw" : "session"},
                     {"peerVerified", !session->udpRaw},
                     {"dataReceived", session->udpReceived},
                     {"commandSupported", true}});
        return result;
    }
    if (method == "power.get") {
        const auto state = powerService_.State();
        Json result = {{"id", "power-1"}, {"selectedChannels", powerPane_.SelectedChannels()}};
        result["recoveryBinding"] = powerPane_.RecoveryBinding();
        for (const char* key : {"connected", "model", "identity", "simulation", "mode", "channels", "task", "error"})
            if (state.contains(key)) result[key] = state[key];
        return result;
    }
    if (method == "power.output") {
        if (!params.contains("enabled") || !params["enabled"].is_boolean() || !params.contains("channels") ||
            params["channels"] != powerPane_.SelectedChannels())
            return error("Power channel selection changed; query power state again. Nothing submitted");
        const auto state = powerService_.State();
        if (!state.value("connected", false)) return error("Power supply is not connected");
        if (params.value("identity", std::string()) != state.value("identity", std::string()) ||
            params.value("mode", std::string()) != state.value("mode", std::string()))
            return error("Power identity/mode changed; query state again. Nothing submitted");
        return powerService_.Submit({{"type", "output"}, {"source", "ai:desktop"},
            {"channels", params["channels"]}, {"enabled", params["enabled"]},
            {"requestId", params.at("requestId")}});
    }
    if (method == "power.action") return powerService_.Action(params.at("id").get<std::string>());
    const auto id = params.value("sessionId", std::string());
    SessionState* session = nullptr;
    for (const auto& candidate : sessions_) if (id == "session-" + std::to_string(candidate->id)) { session = candidate.get(); break; }
    if (method == "release") {
        if (session) {
            session->boot->Cancel();
            const auto task = powerService_.State().value("task", Json::object());
            if (!session->recoveryAction.empty() && task.value("running", false) &&
                task.value("actionId", std::string()) == session->recoveryAction)
                powerService_.Submit({{"type", "stop"}, {"source", "ai-turn-ended"}});
            session->recoveryAction.clear();
        }
        return {{"released", true}};
    }
    if (!session || !*ai::TerminalKind(session->mode) || !session->connection || !session->connection->IsConnected())
        return error("Associated terminal session is not open; no connection was opened automatically");
    if (method == "boot_arm" || method == "boot_status") {
        if (session->mode != 1 && session->mode != 3) return error("U-Boot capture only supports the selected board serial terminal");
        if (method == "boot_arm") {
            if (!session->boot->Arm(params.value("pattern", std::string("Hit any key")),
                params.value("interrupt", std::string(" ")), params.value("prompt", std::string("=>")),
                params.value("timeoutSeconds", 90u), GetTickCount64()))
                return error("Use bounded printable ASCII boot patterns and a 1..120 second capture window");
            SetTimer(window_, BootPulseTimerId, 40, nullptr);
        }
        const auto state = session->boot->Status(GetTickCount64());
        return {{"sessionId", id}, {"state", state.state}, {"matched", state.matched}, {"entered", state.entered},
            {"submittedBursts", state.submitted}, {"remainingMs", state.remainingMs}, {"error", state.error},
            {"pattern", state.pattern}, {"prompt", state.prompt}, {"execution", state.entered ? "prompt-observed" : "unconfirmed"},
            {"source", "local-receive-path"}, {"maximumBursts", 3}};
    }
    if (method == "recover") {
        const auto binding = powerPane_.RecoveryBinding();
        if ((session->mode != 1 && session->mode != 3) || !binding.value("enabled", false) ||
            binding.value("sessionId", std::string()) != id)
            return error("Enable recovery for this exact board terminal and checked power channels in the power page first");
        const auto purpose = params.value("purpose", std::string("unresponsive"));
        const auto now = GetTickCount64();
        if (purpose == "enterUboot") {
            if (session->boot->Status(now).state != "armed") return error("Arm local U-Boot capture before a boot recovery cycle");
        } else if (purpose == "unresponsive") {
            if (!session->aiCommandAt || now - session->aiCommandAt < 5000 || session->aiObservedAt < session->aiCommandAt ||
                (session->aiLastReceivedAt && now - session->aiLastReceivedAt < 5000))
                return error("No current inactivity evidence: read/observe the command and distinguish long work or interaction first");
            const auto line = Trim(session->terminal.CurrentLineText());
            if (session->aiCursor > session->aiCommandCursor && !line.empty() &&
                (line.back() == L'#' || line.back() == L'$' || line.back() == L'>'))
                return error("A prompt is visible; inactivity alone is not a hang");
            auto command = session->aiCommandText;
            std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char *operation : {"flash", "erase", "mkfs", "fsck", "dd ", "nand write", "mmc write"})
                if (command.find(operation) != std::string::npos)
                    return error("Potentially non-interruptible operation; automatic power recovery refused");
        } else return error("Unknown recovery purpose");
        if (!session->recoveryAction.empty()) return error("A recovery was already submitted in this task; query its original action");
        GUID guid{}; wchar_t unique[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, unique, 40)) return error("Cannot allocate recovery identity");
        const auto result = powerService_.Submit({{"type", "recover"}, {"source", "ai:desktop"},
            {"sessionId", id}, {"channels", binding.at("channels")}, {"identity", binding.at("identity")},
            {"mode", binding.at("mode")}, {"requestId", "ai:recover:" + WideToMultiByte(unique, CP_UTF8)}});
        if (!result.contains("error")) session->recoveryAction = result.value("id", std::string());
        return result;
    }
    if (method == "screen") {
        session->aiObservedAt = GetTickCount64();
        auto text = WideToMultiByte(session->terminal.ScreenText(), CP_UTF8);
        bool truncated = text.size() > 65536 || session->terminal.Rows() > 200;
        if (text.size() > 65536) {
            size_t end = 65536;
            while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
            text.resize(end);
        }
        return {{"sessionId", id}, {"text", text}, {"source", "current-vt-screen"}, {"truncated", truncated},
            {"columns", session->terminal.Columns()}, {"rows", session->terminal.Rows()},
            {"cursorRow", session->terminal.CursorRow()}, {"cursorColumn", session->terminal.CursorColumn()},
            {"alternateScreen", session->terminal.AlternateScreen()}, {"keyboardFlags", session->terminal.KeyboardFlags()},
            {"mouseCaptured", session->terminal.MouseCaptured()}, {"cursor", session->aiCursor}};
    }
    if (method == "key") {
        if (runningCommandIndex_ >= 0 || session->mode == 4)
            return error("Named keys require an idle interactive serial, Telnet or SSH terminal; CMD uses complete lines");
        const auto bytes = session->terminal.KeySequence(params.value("key", std::string()));
        if (bytes.empty()) return error("Unsupported key; use Enter, Ctrl+C, Escape, Tab, arrows or function keys");
        return SendAiBytes(*session, Bytes(bytes.begin(), bytes.end()));
    }
    if (method == "read") {
        session->aiObservedAt = GetTickCount64();
        const auto after = params.value("after", std::uint64_t(0));
        const int lines = std::clamp(params.value("lines", 200), 1, 500);
        std::string text, first, last;
        for (const auto& chunk : session->aiOutput) if (!after || chunk.seq > after) {
            if (first.empty()) first = WideToMultiByte(chunk.timestamp, CP_UTF8);
            last = WideToMultiByte(chunk.timestamp, CP_UTF8); text += chunk.text;
        }
        bool limited = false;
        int count = 0;
        for (size_t i = text.size(); i > 0; --i) if (text[i - 1] == '\n' && i < text.size() && ++count >= lines) {
            text.erase(0, i); limited = true; break;
        }
        const bool gap = after && (after > session->aiCursor ||
            (!session->aiOutput.empty() && after + 1 < session->aiOutput.front().seq));
        return {{"sessionId", id}, {"name", WideToMultiByte(session->name, CP_UTF8)},
            {"text", text}, {"source", "device-output"}, {"kind", ai::TerminalKind(session->mode)},
            {"encoding", "UTF-8"}, {"sourceCodePage", session->connection->OutputCodePage() ? session->connection->OutputCodePage() : session->codePage},
            {"firstReceivedAt", first}, {"lastReceivedAt", last}, {"timestampBasis", "local-decoded-receive-time"},
            {"cursor", session->aiCursor}, {"gap", gap}, {"truncated", limited || session->aiTruncated},
            {"transportNotice", session->aiTransportNotice},
            {"maximumBytes", 65536}, {"maximumLines", lines}, {"bytes", text.size()},
            {"lastOutputAgeMs", session->aiLastReceivedAt ? Json(GetTickCount64() - session->aiLastReceivedAt) : Json(nullptr)},
            {"commandId", session->aiCommandNumber},
            {"commandAgeMs", session->aiCommandAt ? Json(GetTickCount64() - session->aiCommandAt) : Json(nullptr)},
            {"outputSinceCommand", session->aiCursor > session->aiCommandCursor},
            {"execution", "unknown"}, {"noOutputIsNotHangProof", true}};
    }
    if (method == "command" || method == "text") {
        if (runningCommandIndex_ >= 0) return error("A command macro is in progress; stop it before sending an AI command");
        const auto command = params.value(method == "command" ? "command" : "text", std::string());
        if (method == "command" ? !ai::ValidCommand(command) :
            command.empty() || command.size() > 4096 || std::any_of(command.begin(),command.end(),[](unsigned char c) {
                return (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127;
            }))
            return error("Invalid bounded terminal input");
        if (method == "text" && session->mode == 4) return error("CMD accepts complete command lines only");
        // Never use the current visible session: the explicit ID was pinned at
        // turn start and displayed in the tool message before this callback.
        const auto wide = MultiByteToWide(reinterpret_cast<const uint8_t*>(command.data()), command.size(), CP_UTF8);
        const wchar_t* endings[] = {L"\r", L"\n", L"\r\n", L""};
        auto outgoing = wide;
        if (method == "text" && session->terminal.BracketedPaste()) outgoing = L"\x1b[200~" + outgoing + L"\x1b[201~";
        if (method == "command" && session->mode != 0)
            outgoing += endings[session->mode == 4 ? 2 : std::clamp(session->lineEndingIndex, 0, 3)];
        const UINT selectedPage = session->connection->InputCodePage() ? session->connection->InputCodePage() : session->codePage;
        const UINT page = selectedPage == 20936 && !IsValidCodePage(20936) ? 936 : selectedPage;
        BOOL replacement = FALSE;
        auto used = page == CP_UTF8 ? nullptr : &replacement;
        const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        const int bytes = WideCharToMultiByte(page, flags, outgoing.data(), static_cast<int>(outgoing.size()), nullptr, 0, nullptr, used);
        if (bytes <= 0 || replacement) return error("The selected terminal encoding cannot represent this command exactly");
        std::string encoded(bytes, '\0');
        WideCharToMultiByte(page, flags, outgoing.data(), static_cast<int>(outgoing.size()), encoded.data(), bytes, nullptr, used);
        if (replacement) return error("Terminal command encoding would replace characters; nothing was submitted");
        if (method == "command" && session->mode == 0) encoded += session->terminal.KeySequence("Enter");
        auto result = SendAiBytes(*session, Bytes(encoded.begin(), encoded.end()));
        if (!result.contains("error") && method == "command") {
            session->aiCommandAt = GetTickCount64();
            session->aiCommandCursor = session->aiCursor;
            session->aiCommandText = command;
            result["commandId"] = ++session->aiCommandNumber;
            result["cursorBeforeSend"] = session->aiCursor;
        }
        return result;
    }

    return error("Unknown bounded SerialCtl operation");
}

void MainWindow::ShowPowerPage() {
    UiRedrawScope redraw(window_);
    aiVisible_ = false;
    KillTimer(window_, SideAnimationTimerId);
    rightPanelAnimating_ = false;
    rightPanelAnimatedWidth_ = -1;
    powerVisible_ = true;
    powerPageOpened_ = true;
    RefreshConnectionList();
    SyncPowerTerminal();
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);
}

void MainWindow::SwitchSession(size_t index, SessionFocus focus) {
    if (index >= sessions_.size()) return;
    const bool sameVisibleSession = activeSession_ == sessions_[index].get() && !aiVisible_ && !powerVisible_;
    if (focus != SessionFocus::AiPreview && focus != SessionFocus::PowerPreview) {
        KillTimer(window_, SideAnimationTimerId);
        rightPanelAnimating_ = false;
        rightPanelAnimatedWidth_ = -1;
        aiVisible_ = false;
        ShowWindow(aiPane_.Handle(), SW_HIDE);
    }
    if (focus != SessionFocus::PowerPreview) { powerVisible_ = false; ShowWindow(powerPane_.Handle(), SW_HIDE); }
    if (!aiVisible_) ShowWindow(terminal_, SW_SHOW);
    const bool interruptedMacro = runningCommandIndex_ >= 0 &&
        activeSession_ && activeSession_ != sessions_[index].get();
    if (interruptedMacro)
        StopCommandSequence(CommandSequenceEnd::Interrupted, L"用户切换当前会话");
    const std::wstring macroStatus = interruptedMacro ? statusText_ : L"";
    activeSession_ = sessions_[index].get();
    activeSession_->unreadStatus = false;
    connection_ = activeSession_->connection.get();
    logger_ = activeSession_->logger.get();
    activeConnectionName_ = activeSession_->name;
    selectedMode_ = activeSession_->mode;
    selectedCodePage_ = activeSession_->codePage;
    lineEndingIndex_ = activeSession_->lineEndingIndex;

    pendingHost_ = activeSession_->host;
    pendingUsername_ = activeSession_->username;
    pendingPassword_ = activeSession_->password;
    pendingPort_ = activeSession_->port;
    sftpDirectory_ = activeSession_->sftpDirectory;
    sftpEntries_ = activeSession_->sftpEntries;
    ClearTerminalSelection();
    UpdateTerminalDimensions();
    InvalidateRect(terminal_, nullptr, TRUE);
    const auto listTop = SendMessageW(connectionList_, LB_GETTOPINDEX, 0, 0);
    SendMessageW(connectionList_, LB_SETCURSEL, index, 0);
    SendMessageW(connectionList_, LB_SETTOPINDEX, listTop, 0);
    SetConnectedUi(connection_ && connection_->IsConnected());
    if (focus != SessionFocus::AiPreview && focus != SessionFocus::PowerPreview && selectedMode_ == 0 && SftpClient::IsAvailable()) {
        ShowSftpPanel(true);
        RefreshSftpList();
        if (!sameVisibleSession && connection_ && connection_->IsConnected() && !sftpBusy_) RefreshSftp();
    } else {
        ShowSftpPanel(false);
    }
    RefreshConnectionList();
    RECT client{}; GetClientRect(window_, &client); LayoutControls(client.right, client.bottom);
    InvalidateRect(window_, nullptr, TRUE);
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);
    if (focus == SessionFocus::Terminal) SetFocus(terminal_);
    if (interruptedMacro) AppendStatus(macroStatus, true);
    else if (!activeSession_->lastStatus.empty()) {
        const auto &t = activeSession_->statusTime;
        wchar_t timestamp[32]{};
        swprintf_s(timestamp, L"%02u:%02u:%02u · ", t.wHour, t.wMinute, t.wSecond);
        AppendStatus(timestamp + activeSession_->lastStatus, activeSession_->lastStatusIsError);
    } else AppendStatus(L"", false);
}

bool MainWindow::SendBytesToActive(const Bytes& data, bool localEcho, const std::wstring& echoedText) {
    if (!activeSession_ || !connection_ || !connection_->IsConnected()) {
        AppendStatus(L"请先建立连接。", true);
        return false;
    }
    if (data.empty())
        return false;
    ScrollTerminalToBottom();
    std::wstring error;
    if (activeSession_->mode == 1 || activeSession_->mode == 4) {
        auto result = sessionService_.Input("session-" + std::to_string(activeSession_->id), data, "gui");
        if (result.contains("error")) {
            auto text = result["error"].dump();
            AppendStatus(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(text.data()), text.size(), CP_UTF8),
                         true);
            return false;
        }
    } else if (!connection_->Send(data, error)) {
        AppendStatus(error, true);
        return false;
    }
    if (activeSession_->mode == 4)
        localEcho = false;
    if (localEcho && !echoedText.empty()) {
        const std::wstring timestamp = timestampEnabled_ ? CurrentTerminalTimestamp() : std::wstring();
        const TerminalFeedResult result = activeSession_->terminal.Feed(echoedText, timestamp);
        if (logger_ && !result.logText.empty()) logger_->WriteText(result.logText);
        if (activeSession_->terminalScrollOffset > 0) {
            activeSession_->terminalScrollOffset += static_cast<int>(result.scrollbackAdded);
            activeSession_->terminalHasNewOutput = true;
        }
        InvalidateRect(terminal_, nullptr, FALSE);
    }
    return true;
}

void MainWindow::SendTerminalCharacter(wchar_t character) {
    if (runningCommandIndex_ >= 0) return;
    if (!activeSession_ || !connection_ || !connection_->IsConnected()) return;
    ScrollTerminalToBottom();
    if (selectedMode_ == 4) {
        auto &editor = activeSession_->cmdEditor;
        if (character == L'\r') {
            auto text = editor.Text() + L"\r\n";
            auto encoded = EncodeTerminalText(text);
            if (SendBytesToActive(Bytes(encoded.begin(), encoded.end()), false))
                editor.Submitted();
        } else if (character == L'\b')
            editor.Edit(CmdLineEditor::Key::Backspace);
        else if (character == 3) {
            AppendStatus(L"CMD 行提交模式不支持 Ctrl+C；可使用全部断开终止进程树", true);
        } else if (character >= 32 || character == L'\t') {
            if (!editor.Insert(std::wstring(1, character)))
                AppendStatus(L"CMD 输入超过 8191 字符", true);
        }
        InvalidateRect(terminal_, nullptr, FALSE);
        return;
    }
    if (activeSession_->terminal.KeyboardFlags() && (character < 32 || character == 127)) {
        std::string key = character == L'\r' ? "Enter" : character == L'\t' ? "Tab" :
            character == L'\b' || character == 127 ? "Backspace" : character == 27 ? "Escape" : "";
        unsigned modifiers = (GetKeyState(VK_SHIFT) & 0x8000 ? 1u : 0u) |
            (GetKeyState(VK_MENU) & 0x8000 ? 2u : 0u) | (GetKeyState(VK_CONTROL) & 0x8000 ? 4u : 0u);
        if (key.empty() && character >= 1 && character <= 26) {
            key.assign(1, static_cast<char>('a' + character - 1));
            modifiers |= 4;
        }
        const auto sequence = activeSession_->terminal.KeySequence(key, modifiers);
        if (!sequence.empty()) SendBytesToActive(Bytes(sequence.begin(), sequence.end()), false);
        return;
    }
    std::wstring text;
    if (character == L'\r') text = selectedMode_ == 0 ? L"\r" : SelectedLineEnding();
    else if (character == L'\b' && selectedMode_ == 0) text.assign(1, static_cast<wchar_t>(0x7f));
    else text.assign(1, character);
    const std::string encoded = EncodeTerminalText(text);
    if (encoded.empty() && character != 0) return;
    SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text);
}

void MainWindow::SendTerminalKey(WPARAM key, bool shift, bool control, bool alt) {
    if (runningCommandIndex_ >= 0) return;
    if (!activeSession_) return;
    ScrollTerminalToBottom();
    if (selectedMode_ == 4) {
        using Key = CmdLineEditor::Key;
        switch (key) {
        case VK_LEFT:
            activeSession_->cmdEditor.Edit(Key::Left);
            break;
        case VK_RIGHT:
            activeSession_->cmdEditor.Edit(Key::Right);
            break;
        case VK_HOME:
            activeSession_->cmdEditor.Edit(Key::Home);
            break;
        case VK_END:
            activeSession_->cmdEditor.Edit(Key::End);
            break;
        case VK_DELETE:
            activeSession_->cmdEditor.Edit(Key::Delete);
            break;
        case VK_UP:
            activeSession_->cmdEditor.Edit(Key::Up);
            break;
        case VK_DOWN:
            activeSession_->cmdEditor.Edit(Key::Down);
            break;
        case VK_ESCAPE:
            activeSession_->cmdEditor.Clear();
            break;
        default:
            AppendStatus(L"CMD 本地行编辑不支持该控制键", true);
            break;
        }
        InvalidateRect(terminal_, nullptr, FALSE);
        return;
    }
    std::string name;
    switch (key) {
    case VK_UP: name = "Up"; break;
    case VK_DOWN: name = "Down"; break;
    case VK_LEFT: name = "Left"; break;
    case VK_RIGHT: name = "Right"; break;
    case VK_HOME: name = "Home"; break;
    case VK_END: name = "End"; break;
    case VK_INSERT: name = "Insert"; break;
    case VK_DELETE: name = "Delete"; break;
    case VK_PRIOR: name = "PageUp"; break;
    case VK_NEXT: name = "PageDown"; break;
    case VK_ESCAPE: name = "Escape"; break;
    case VK_TAB: name = "Tab"; break;
    default:
        if (key >= VK_F1 && key <= VK_F12) name = "F" + std::to_string(key - VK_F1 + 1);
        else return;
    }
    const auto sequence = activeSession_->terminal.KeySequence(name, (shift ? 1u : 0u) | (alt ? 2u : 0u) | (control ? 4u : 0u));
    if (!sequence.empty()) SendBytesToActive(Bytes(sequence.begin(), sequence.end()), false);
}

void MainWindow::SendCommand(size_t index) {
    if (index >= commands_.size() || commands_[index].commands.empty() || runningCommandIndex_ >= 0) return;
    if (!connection_ || !connection_->IsConnected() || !activeSession_) {
        AppendStatus(L"请先建立连接，再执行常用命令。", true);
        return;
    }
    ULONGLONG now = GetTickCount64();
#ifdef SERIALCTL_AI_TESTING
    if (commandClock_) now = commandClock_();
#endif
    if (lastCommandIndex_ == static_cast<int>(index) && lastCommandSession_ == activeSession_->id &&
        now - lastCommandActivation_ < std::max<DWORD>(350, GetDoubleClickTime()))
        return; // One activation across mouse double-clicks and held Enter.
    const CommandItem& command = commands_[index];
    if (command.commands.size() == 1) {
        const std::wstring text = command.commands.front() + SelectedLineEnding();
        const std::string encoded = EncodeTerminalText(text);
        if (SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text)) {
            lastCommandIndex_ = static_cast<int>(index);
            lastCommandSession_ = activeSession_->id;
            lastCommandActivation_ = now;
            AppendStatus(L"已提交命令；执行结果见输出：" + command.commands.front(), false);
        }
        return;
    }

    runningCommandIndex_ = static_cast<int>(index);
    lastCommandIndex_ = static_cast<int>(index);
    lastCommandSession_ = activeSession_->id;
    lastCommandActivation_ = now;
    submittedCommandSteps_ = 0;
    runningCommandSessionId_ = activeSession_->id;
    runningCommandCodePage_ = SelectedCodePage();
    runningCommandLineEnding_ = SelectedLineEnding();
    UpdateCommandActions();
    InvalidateRect(commandList_, nullptr, FALSE);
    SendNextCommandStep();
}

void MainWindow::SendNextCommandStep() {
    if (runningCommandIndex_ < 0 || runningCommandIndex_ >= static_cast<int>(commands_.size())) return;
    const CommandItem& command = commands_[static_cast<size_t>(runningCommandIndex_)];
    if (!activeSession_ || activeSession_->id != runningCommandSessionId_ || !connection_ ||
        !connection_->IsConnected() || submittedCommandSteps_ >= command.commands.size()) {
        StopCommandSequence(CommandSequenceEnd::Interrupted, L"连接或当前会话已变化");
        return;
    }

    const std::wstring& step = command.commands[submittedCommandSteps_];
    const std::wstring text = step + runningCommandLineEnding_;
    const std::string encoded =
        selectedMode_ == 4 ? EncodeTerminalText(text) : WideToMultiByte(text, runningCommandCodePage_);
    if (!SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text)) {
        StopCommandSequence(CommandSequenceEnd::Interrupted, statusText_);
        return;
    }

    ++submittedCommandSteps_;
    InvalidateRect(commandList_, nullptr, FALSE);
    if (submittedCommandSteps_ >= command.commands.size()) {
        StopCommandSequence(CommandSequenceEnd::Submitted);
        return;
    }

    AppendStatus(L"宏“" + command.name + L"”已提交 " +
        std::to_wstring(submittedCommandSteps_) + L"/" + std::to_wstring(command.commands.size()) +
        L" 条，等待下一次提交；执行结果见输出。", false);
    KillTimer(window_, StatusTimerId);
    if (!SetTimer(window_, CommandTimerId, command.intervalMs, nullptr)) {
        StopCommandSequence(CommandSequenceEnd::Interrupted, L"计时器创建失败");
    }
}

void MainWindow::StopCommandSequence(CommandSequenceEnd end, const std::wstring& detail) {
    if (runningCommandIndex_ < 0) return;
    std::wstring name;
    size_t total = submittedCommandSteps_;
    if (runningCommandIndex_ < static_cast<int>(commands_.size())) {
        name = commands_[static_cast<size_t>(runningCommandIndex_)].name;
        total = commands_[static_cast<size_t>(runningCommandIndex_)].commands.size();
    }
    const auto count = std::to_wstring(submittedCommandSteps_) + L"/" + std::to_wstring(total);
    // detail can alias statusText_; snapshot it before AppendStatus replaces it.
    const std::wstring reason = detail;
    auto* owner = FindSession(runningCommandSessionId_);
    KillTimer(window_, CommandTimerId);
    runningCommandIndex_ = -1;
    submittedCommandSteps_ = 0;
    runningCommandSessionId_ = 0;
    runningCommandLineEnding_.clear();
    UpdateCommandActions();
    InvalidateRect(commandList_, nullptr, FALSE);
    if (end == CommandSequenceEnd::Submitted)
        AppendStatus(L"宏“" + name + L"”已提交 " + count + L" 条；执行结果见输出。", false);
    else if (end == CommandSequenceEnd::UserStopped)
        AppendStatus(L"宏“" + name + L"”已停止后续提交（" + count +
                     L"）；已提交内容可能继续执行，结果见输出。", false);
    else if (end == CommandSequenceEnd::Interrupted)
        AppendStatus(L"宏“" + name + L"”后续提交已中断（" + count + L"）：" + reason +
                     L"；已提交内容执行状态未知，请检查连接和输出，不自动重发。", true);
    if (owner && end != CommandSequenceEnd::Silent) {
        owner->lastStatus = statusText_;
        owner->lastStatusIsError = end == CommandSequenceEnd::Interrupted;
        owner->unreadStatus = owner != activeSession_;
        GetLocalTime(&owner->statusTime);
    }
}

void MainWindow::PostData(std::uint64_t sessionId, const Bytes& data) {
    LARGE_INTEGER arrival{};
    if (measurePaint_)
        QueryPerformanceCounter(&arrival);
    const auto state = sessionService_.State("session-" + std::to_string(sessionId));
    const UINT codePage = state.value("outputCodePage", UINT(0));
    apiServer_.Publish("session-" + std::to_string(sessionId), data, "device", "output", codePage);
    QueueReceived({sessionId, data, codePage, {}, arrival.QuadPart});
}
void MainWindow::QueueReceived(ReceivedItem item) {
    if (closing_)
        return;
    if (item.source.empty() && item.data.size() > 16384) {
        for (size_t offset = 0; offset < item.data.size(); offset += 16384) {
            const auto end = std::min(offset + 16384, item.data.size());
            QueueReceived({item.sessionId, Bytes(item.data.begin() + offset, item.data.begin() + end),
                           item.codePage, {}, item.arrival});
        }
        return;
    }
    const auto id = item.sessionId;
    std::lock_guard<std::mutex> lock(receivedMutex_);
    auto &bytes = receivedBytes_[id];
    if (bytes + item.data.size() > 4 * 1024 * 1024 || receivedBytes_.size() > 64 || received_.size() >= 4096)
        receiveOverflow_.insert(id);
    else {
        bytes += item.data.size();
        if (!received_.empty() && item.source.empty() && received_.back().source.empty() &&
            received_.back().sessionId == id && received_.back().codePage == item.codePage &&
            received_.back().data.size() + item.data.size() <= 16384)
            received_.back().data.insert(received_.back().data.end(), item.data.begin(), item.data.end());
        else
            received_.push_back(std::move(item));
    }
    if (!receivePosted_) {
        receivePosted_ = PostMessageW(window_, MessageData, 0, 0) != FALSE;
        if (!receivePosted_) {
            received_.clear();
            receivedBytes_.clear();
            receiveOverflow_.insert(id);
        }
    }
}
void MainWindow::AppendInput(const ReceivedItem &item) {
    SessionState *session = FindSession(item.sessionId);
    if (!session)
        return;
    if (session->rawTrace && session->logger) {
        auto raw = Encode64(item.data);
        session->logger->WriteStatus(
            L"TX Base64 " + MultiByteToWide(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size(), CP_UTF8));
    }
    if (session->mode == 4) {
        const auto text =
            MultiByteToWide(item.data.data(), item.data.size(), item.codePage ? item.codePage : session->codePage);
        const auto result = session->terminal.Feed(text, timestampEnabled_ ? CurrentTerminalTimestamp() : L"");
        if (session->logger)
            session->logger->WriteText(result.logText);
        if (session->terminalScrollOffset > 0) {
            session->terminalScrollOffset += static_cast<int>(result.scrollbackAdded);
            session->terminalHasNewOutput = true;
        }
        if (session == activeSession_)
            InvalidateRect(terminal_, nullptr, FALSE);
    }
    if (session->logger)
        session->logger->WriteStatus(
            L"输入来源 " +
            MultiByteToWide(reinterpret_cast<const std::uint8_t *>(item.source.data()), item.source.size(), CP_UTF8) +
            L" · " + std::to_wstring(item.data.size()) + L" 字节");
    if (measurePaint_ && session == activeSession_ && session->logger)
        SetPropW(window_, L"SerialCtl.LogAcceptedBytes", reinterpret_cast<HANDLE>(
            static_cast<UINT_PTR>(session->logger->CaptureExportPoint().bytes)));
}

void MainWindow::PostStatus(std::uint64_t sessionId, const std::wstring& text, bool isError) {
    if (closing_) return;
    const auto utf8=WideToMultiByte(text,CP_UTF8);
    apiServer_.Publish("session-"+std::to_string(sessionId),Bytes(utf8.begin(),utf8.end()),"connection",isError?"error":"status");
    auto* status = new StatusMessage{sessionId, text, isError};
    if (!PostMessageW(window_, MessageStatus, 0, reinterpret_cast<LPARAM>(status))) delete status;
}

void MainWindow::AppendData(std::uint64_t sessionId, const Bytes &data, UINT codePage, LONGLONG arrival) {
    SessionState* session = FindSession(sessionId);
    if (!session) return;
    if (session->udpRaw && !data.empty())
        session->udpReceived = true;
    if (session->rawTrace && session->logger) {
        auto raw = Encode64(data);
        session->logger->WriteStatus(
            L"RX Base64 " + MultiByteToWide(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size(), CP_UTF8));
    }
    if (session->mode == 4 && codePage && codePage != session->codePage) {
        session->codePage = codePage;
        if (session == activeSession_)
            selectedCodePage_ = codePage;
    }
    const std::wstring decoded = DecodeTerminalData(*session, data);
    if (measurePaint_ && arrival) {
        session->paintProbeText += decoded;
        static const std::wstring prefix = L"SERIALCTL_PAINT_";
        const auto marker = session->paintProbeText.rfind(prefix);
        if (marker != std::wstring::npos && marker + prefix.size() + 3 <= session->paintProbeText.size()) {
            const auto digits = session->paintProbeText.substr(marker + prefix.size(), 3);
            if (std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
                const auto probe = static_cast<INT_PTR>(std::stoi(digits) + 1);
                if (probe != session->paintProbe) {
                    session->paintProbe = probe;
                    session->pendingProbeArrival = arrival;
                }
            }
        }
        if (session->paintProbeText.size() > 96)
            session->paintProbeText.erase(0, session->paintProbeText.size() - 96);
    }
    const std::wstring timestamp = timestampEnabled_ ? CurrentTerminalTimestamp() : std::wstring();
    const bool wasAlternateScreen = session->terminal.AlternateScreen();
    const TerminalFeedResult result = session->terminal.Feed(decoded, timestamp);
    if (*ai::TerminalKind(session->mode) && !decoded.empty()) {
        session->aiLastReceivedAt = GetTickCount64();
        // Bound the additional AI copy before conversion even if the terminal
        // received a large coalesced batch. Serial receive never waits on AI.
        size_t first = decoded.size() > 32768 ? decoded.size() - 32768 : 0;
        if (first && decoded[first] >= 0xdc00 && decoded[first] <= 0xdfff) ++first;
        if (first) session->aiTruncated = true;
        auto output = WideToMultiByte(decoded.substr(first), CP_UTF8);
        if (output.size() > 65536) {
            size_t start = output.size() - 65536;
            while (start < output.size() && (static_cast<unsigned char>(output[start]) & 0xc0) == 0x80) ++start;
            output.erase(0, start); session->aiTruncated = true;
        }
        session->aiBytes += output.size();
        session->aiOutput.push_back({++session->aiCursor, CurrentTerminalTimestamp(), std::move(output)});
        while (session->aiBytes > 65536 || session->aiOutput.size() > 256) {
            session->aiBytes -= session->aiOutput.front().text.size(); session->aiOutput.pop_front(); session->aiTruncated = true;
        }
    }
    if (session->logger && !result.logText.empty()) session->logger->WriteText(result.logText);
    if (!result.response.empty() && session->connection && session->connection->IsConnected()) {
        if (session->mode == 4) {
            if (!session->cmdInteractionWarning) {
                session->cmdInteractionWarning = true;
                PostStatus(sessionId, L"CMD 行提交模式不支持全屏终端查询；未向控制台发送控制序列", true);
            }
        } else if (session->mode == 1) {
            const auto reply =
                sessionService_.Input("session-" + std::to_string(sessionId),
                                      Bytes(result.response.begin(), result.response.end()), "terminal-response");
            if (reply.contains("error")) {
                const auto message = reply.at("error").at("message").get<std::string>();
                PostStatus(sessionId,
                           L"终端协议响应发送失败：" +
                               MultiByteToWide(reinterpret_cast<const std::uint8_t *>(message.data()), message.size(),
                                               CP_UTF8),
                           true);
            }
        } else {
            std::wstring ignored;
            session->connection->Send(Bytes(result.response.begin(), result.response.end()), ignored);
        }
    }
    if (result.titleChanged) session->terminalTitle = result.title;
    if (!result.workingDirectory.empty()) { session->terminalDirectory = result.workingDirectory; session->terminalDirectoryFromOsc = true; }
    if (session->logger && !session->logger->Error().empty() && loggerErrorsShown_.insert(sessionId).second)
        AppendStatus(session->logger->Error(), true);
    SyncSftpDirectoryFromTerminal(*session);
    if (session == activeSession_) {
        if (wasAlternateScreen != session->terminal.AlternateScreen())
            UpdateTerminalDimensions();
        if (session->terminalScrollOffset > 0) {
            session->terminalScrollOffset += static_cast<int>(result.scrollbackAdded);
            session->terminalHasNewOutput = true;
        }
        const int maximumOffset = std::max(0,
            static_cast<int>(session->terminal.DisplayLineCount()) - terminalVisibleRows_);
        session->terminalScrollOffset = std::min(session->terminalScrollOffset, maximumOffset);
        InvalidateRect(terminal_, nullptr, FALSE);
    }
}

std::wstring MainWindow::DecodeTerminalData(SessionState& session, const Bytes& data) {
    return session.decoder.Decode(data, session.codePage);
}

void MainWindow::SyncSftpDirectoryFromTerminal(SessionState& session) {
    if (session.mode != 0 || session.username.empty() || !session.sftpFollowTerminal) return;
    if (!session.terminalDirectory.empty()) {
        if (session.terminalDirectory != session.sftpDirectory && session.terminalDirectory != session.pendingSftpDirectory) {
            session.pendingSftpDirectory = session.terminalDirectory;
            if (&session == activeSession_ && !sftpBusy_) RefreshSftp(session.terminalDirectory);
        }
        if (session.terminalDirectoryFromOsc) return;
    }
    std::wstring line = Trim(session.terminal.CurrentLineText());
    if (line.empty()) return;

    const size_t prompt = line.find_last_of(L"$#");
    if (prompt == std::wstring::npos || !Trim(line.substr(prompt + 1)).empty()) return;
    const std::wstring identity = session.username + L"@";
    const size_t identityAt = line.rfind(identity, prompt);
    if (identityAt == std::wstring::npos) return;
    const size_t colon = line.find(L':', identityAt + identity.size());
    if (colon == std::wstring::npos || colon >= prompt) return;

    std::wstring directory = Trim(line.substr(colon + 1, prompt - colon - 1));
    if (!directory.empty() && directory.back() == L']') {
        directory.pop_back();
        directory = Trim(std::move(directory));
    }
    if (directory == L"~") {
        directory = session.sftpHomeDirectory;
    } else if (directory.rfind(L"~/", 0) == 0) {
        if (session.sftpHomeDirectory.empty()) return;
        directory = JoinSftpRemotePath(session.sftpHomeDirectory, directory.substr(2));
    } else if (directory.empty() || directory.front() != L'/') {
        return;
    }
    while (directory.size() > 1 && directory.back() == L'/') directory.pop_back();
    if (directory.empty() || directory == session.sftpDirectory ||
        directory == session.pendingSftpDirectory) return;

    session.pendingSftpDirectory = directory;
    session.terminalDirectory = directory;
    if (&session == activeSession_ && !sftpBusy_) RefreshSftp(directory);
}

void MainWindow::AppendStatus(const std::wstring& text, bool isError) {
    statusIsError_ = isError;
    statusText_ = text;
    TOOLINFOW tool{sizeof(tool)};tool.hwnd=window_;tool.uId=reinterpret_cast<UINT_PTR>(status_);
    tool.lpszText=const_cast<wchar_t*>(statusText_.c_str());
    SendMessageW(statusTip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
    SetWindowTextW(status_, text.c_str());
    if (status_) {
        RECT client{}; GetClientRect(window_, &client);
        const auto layout = GetMainLayoutMetrics(client.right, client.bottom, SidebarWidth(), SidebarCollapsed(),
                                                 rightPanelAnimatedWidth_, LiveMonitorPage());
        HDC dc=GetDC(status_); auto old=SelectObject(dc,smallFont_);
        RECT measured{0,0,layout.leftWidth-Ui::Gap-Ui::PanelPadding*2,0};
        DrawTextW(dc,text.c_str(),-1,&measured,DT_WORDBREAK|DT_CALCRECT);
        SelectObject(dc,old);ReleaseDC(status_,dc);
        int height = text.empty() ? 0 : std::clamp(static_cast<int>(measured.bottom),Ui::Scale(20),Ui::Scale(120));
        if (statusHeight_ != height) {
            statusHeight_ = height;
            LayoutControls(client.right, client.bottom);
        }
    }
    ShowWindow(status_, text.empty() ? SW_HIDE : SW_SHOW);
    InvalidateRect(status_, nullptr, TRUE);
    KillTimer(window_, StatusTimerId);
    if (!text.empty() && !isError) SetTimer(window_, StatusTimerId, 3500, nullptr);
    if (logger_ && !text.empty()) logger_->WriteStatus(text);
}

void MainWindow::StartLogExport(const std::wstring &source, const std::wstring &destination,
                                std::uint64_t total, LogExportTask task) {
    PollLogExport();
    if (logExportJob_) {
        if (!statusIsError_ && runningCommandIndex_ < 0)
            AppendStatus(L"已有日志正在导出，可在设置中取消", false);
        return;
    }
    auto job = std::make_shared<LogExportJob>();
    job->source = source;
    job->destination = destination;
    job->total.store(total);
    logExportJob_ = job;
    if (measurePaint_) {
        SetPropW(window_, L"SerialCtl.LogExportBusy", reinterpret_cast<HANDLE>(1));
        SetPropW(window_, L"SerialCtl.LogExportCutoff", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(total)));
    }
    logExportNotice_ = L"正在准备日志快照 · " + source;
    powerPane_.SetLogExportBusy(true);
    logExportMainStatus_.clear();
    if (!statusIsError_ && runningCommandIndex_ < 0) {
        AppendStatus(logExportNotice_, false);
        logExportMainStatus_ = logExportNotice_;
    }
    try {
        const HWND wakeWindow = window_;
        logExportThread_ = std::thread([job, task = std::move(task), wakeWindow]() mutable {
            try {
                job->success = task(job->error, &job->cancel,
                    [job](std::uint64_t copied, std::uint64_t length) {
                        job->total.store(length);
                        job->copied.store(copied);
                        job->copying.store(true);
                    });
            } catch (...) { job->error = L"日志导出失败，原目标文件未替换"; }
            // Release source ownership before done: UI completion must not join
            // a thread whose last logger destructor is still stopping a writer.
            task = {};
            job->done.store(true, std::memory_order_release);
            PostMessageW(wakeWindow, MessageLogExport, 0, 0); // no owning message payload
        });
        SetTimer(window_, LogExportTimerId, 200, nullptr);
    } catch (...) {
        logExportJob_.reset();
        if (measurePaint_) RemovePropW(window_, L"SerialCtl.LogExportBusy");
        powerPane_.SetLogExportBusy(false);
        logExportNotice_ = L"无法启动后台日志导出，原目标文件未替换";
        if (!statusIsError_ && runningCommandIndex_ < 0) AppendStatus(logExportNotice_, true);
    }
}

void MainWindow::PollLogExport() {
    auto job = logExportJob_;
    if (!job) return;
    std::wstring notice;
    bool failed = false;
    if (job->done.load(std::memory_order_acquire)) {
        if (logExportThread_.joinable()) logExportThread_.join();
        failed = !job->success && !job->cancel.load();
        notice = job->success ? L"日志已保存：" + job->destination + L" · " + job->source :
            job->error + L" · " + job->source;
        logExportJob_.reset();
        if (measurePaint_) RemovePropW(window_, L"SerialCtl.LogExportBusy");
        powerPane_.SetLogExportBusy(false);
        KillTimer(window_, LogExportTimerId);
    } else if (job->cancel.load()) {
        if (logExportThread_.joinable())
            CancelSynchronousIo(reinterpret_cast<HANDLE>(logExportThread_.native_handle()));
        notice = L"正在取消日志导出 · " + job->source;
    } else if (!job->copying.load()) {
        notice = L"正在准备日志快照 · " + job->source;
    } else {
        const auto total = job->total.load(), copied = std::min(job->copied.load(), total);
        const auto percent = total ? static_cast<unsigned>(static_cast<long double>(copied) * 100 / total) : 100;
        notice = L"日志复制 " + std::to_wstring(percent) + L"%（" +
            std::to_wstring(copied / (1024 * 1024)) + L"/" + std::to_wstring(total / (1024 * 1024)) +
            L" MiB）" + (copied == total ? L"，正在完成 · " : L" · ") + job->source;
    }
    if (notice != logExportNotice_) {
        logExportNotice_ = std::move(notice);
        // Export feedback must not hide a device fault or an unknown macro
        // outcome. The instance note in Settings always retains this feedback.
        if (!statusIsError_ && runningCommandIndex_ < 0 &&
            (statusText_.empty() || statusText_ == logExportMainStatus_)) {
            AppendStatus(logExportNotice_, failed);
            logExportMainStatus_ = logExportNotice_;
        }
    }
}

void MainWindow::CancelLogExport() {
    PollLogExport();
    if (!logExportJob_) return;
    logExportJob_->cancel.store(true);
    PollLogExport();
}

void MainWindow::StopLogExport() {
    if (logExportJob_) logExportJob_->cancel.store(true);
    if (logExportThread_.joinable()) {
        const HANDLE thread = reinterpret_cast<HANDLE>(logExportThread_.native_handle());
        while (WaitForSingleObject(thread, 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(thread);
        logExportThread_.join();
    }
    if (window_) KillTimer(window_, LogExportTimerId);
    logExportJob_.reset();
}

void MainWindow::SaveCurrentLog(HWND owner) {
    PollLogExport();
    if (logExportJob_) return;
    if (!logger_ || logger_->Path().empty()) {
        AppendStatus(L"当前没有可保存的会话日志。", true);
        return;
    }
    const auto sessionId = activeSession_ ? activeSession_->id : 0;
    const auto originalPath = logger_->Path();
    wchar_t path[MAX_PATH] = L"SerialCtl-会话日志.log";
    const size_t slash = logger_->Path().find_last_of(L"\\/");
    const std::wstring fileName = slash == std::wstring::npos ? logger_->Path() : logger_->Path().substr(slash + 1);
    if (!fileName.empty() && fileName.size() < std::size(path)) wcscpy_s(path, fileName.c_str());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner ? owner : window_;
    dialog.lpstrFilter = L"日志文件 (*.log)\0*.log\0文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"log";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;
    if (!activeSession_ || activeSession_->id != sessionId || !logger_ || logger_->Path() != originalPath) {
        AppendStatus(L"原会话已切换或关闭，日志未保存；请重新选择会话", true);
        return;
    }
    auto logger = activeSession_->logger;
    const auto point = logger->CaptureExportPoint();
    const std::wstring destination = path;
    StartLogExport(L"会话日志 · " + activeSession_->name, destination, point.bytes,
        [logger, point, destination](std::wstring &error, const std::atomic<bool> *cancel,
                                     const SessionLogger::ExportProgress &progress) {
            return logger->SaveCopy(destination, point, error, cancel, progress);
        });
}

void MainWindow::SaveNetworkDiagnostics(HWND owner) {
    wchar_t path[MAX_PATH] = L"SerialCtl-network-diagnostics.json";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner ? owner : window_;
    dialog.lpstrFilter = L"JSON (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog))
        return;
    const auto text = Json{{"protocol", "serialctl.v1"},
                           {"instance", apiServer_.Instance()},
                           {"port", apiServer_.Port()},
                           {"records", apiServer_.Diagnostics()}}
                          .dump(2);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        AppendStatus(L"无法保存网络诊断：" + Win32ErrorMessage(), true);
        return;
    }
    DWORD written = 0;
    bool ok =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    const auto error = ok ? std::wstring() : Win32ErrorMessage();
    CloseHandle(file);
    AppendStatus(ok ? L"网络连接诊断已保存" : L"网络连接诊断写入失败：" + error, !ok);
}

void MainWindow::ShowSettings() {
    SettingsDialogState state{this, activeSession_ ? activeSession_->id : 0};
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    } data{};
    data.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    data.dialog.cx = 238;
    data.dialog.cy = 280;
    UiDialogBoxIndirectOwned(instance_, &data.dialog, window_, SettingsDialogProc,
        reinterpret_cast<LPARAM>(&state));
}

INT_PTR CALLBACK MainWindow::SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<SettingsDialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->owner) return FALSE;
    auto* self = state->owner;
    const bool hasSession = state->sessionId && self->activeSession_ &&
        self->activeSession_->id == state->sessionId;
    auto refresh = [&] {
        self->PollLogExport();
        const bool connected = hasSession && self->connection_ && self->connection_->IsConnected();
        auto enable = [&](int id, bool enabled) {
            HWND control = GetDlgItem(dialog, id);
            if ((IsWindowEnabled(control) != FALSE) != enabled) EnableWindow(control, enabled);
        };
        enable(IdSettingsRawTrace, hasSession);
        enable(IdSettingsEncoding, hasSession);
        enable(IdSettingsEnding, hasSession && self->activeSession_->mode != 0 &&
            self->activeSession_->mode != 4);
        enable(IdMenuSaveLog, hasSession && self->logger_ && !self->logger_->Path().empty() &&
            !self->logExportJob_);
        enable(IdMenuCancelLogExport, self->logExportJob_ && !self->logExportJob_->cancel.load());
        const auto note = self->logExportNotice_.empty() ?
            std::wstring(L"保存/取消仅影响选项；操作立即执行。") : self->logExportNotice_;
        HWND help = GetDlgItem(dialog, IdSettingsExportNote);
        if (help && ControlText(help) != note) {
            SetWindowTextW(help, note.c_str());
            wcsncpy_s(state->exportTip, note.c_str(), _TRUNCATE);
            TOOLINFOW tool{sizeof(tool)};
            tool.hwnd = dialog;
            tool.uId = reinterpret_cast<UINT_PTR>(help);
            tool.lpszText = state->exportTip;
            SendMessageW(self->statusTip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        enable(IdMenuClear, hasSession);
        enable(IdMenuSelectAll, hasSession && self->activeSession_->terminal.DisplayLineCount() > 0);
        enable(IdMenuCopy, hasSession && self->HasTerminalSelection());
        enable(IdSendSelectionToAi, hasSession && self->HasTerminalSelection());
        enable(IdMenuPaste, connected && self->runningCommandIndex_ < 0 &&
            IsClipboardFormatAvailable(CF_UNICODETEXT));
    };
    if (message == WM_INITDIALOG) {
        SetWindowTextW(dialog, L"设置");
        ApplyDarkTitleBar(dialog, self->darkMode_);
        UiResizeDialog(dialog, Ui::Dpi, 436, 556);
        auto add = [&](const wchar_t* type, const wchar_t* text, int id, int x, int y,
                       int width, int height, DWORD style = 0) {
            HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                Ui::Scale(x), Ui::Scale(y), Ui::Scale(width), Ui::Scale(height), dialog,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self->instance_, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont_), TRUE);
            if (_wcsicmp(type, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
                SetWindowSubclass(control, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
            if (_wcsicmp(type, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX)
                UiStyleCheckBox(control, &self->dialogFieldStyle_, true);
            if (_wcsicmp(type, WC_COMBOBOXW) == 0)
                UiStyleField(control, &self->dialogFieldStyle_);
            return control;
        };
        const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
        const DWORD button = BS_OWNERDRAW | WS_TABSTOP;
        add(L"STATIC", L"全局 · 本窗口的所有终端", -1, 16, 12, 404, 24);
        add(L"BUTTON", L"本地回显", IdSettingsEcho, 16, 36, 196, 28, check);
        add(L"BUTTON", L"毫秒时间戳", IdSettingsTimestamp, 224, 36, 196, 28, check);
        const auto sessionLabel = hasSession ? L"当前会话 · " + self->activeSession_->name :
            std::wstring(L"当前会话 · 未选择终端");
        add(L"STATIC", sessionLabel.c_str(), -1, 16, 76, 404, 24, SS_ENDELLIPSIS);
        add(L"BUTTON", L"记录 RX/TX 原始字节（Base64）", IdSettingsRawTrace,
            16, 104, 404, 28, check);
        add(L"STATIC", L"收发编码", -1, 16, 144, 196, 24);
        const wchar_t* endingLabel = hasSession && self->activeSession_->mode == 0 ? L"Enter（SSH 固定 CR）" :
            hasSession && self->activeSession_->mode == 4 ? L"Enter（CMD 提交整行）" : L"Enter 换行方式";
        add(L"STATIC", endingLabel, -1, 224, 144, 196, 24);
        HWND encoding = add(WC_COMBOBOXW, L"", IdSettingsEncoding, 16, 168, 196, 180,
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL);
        for (auto value : {L"UTF-8", L"GBK", L"GB2312"})
            SendMessageW(encoding, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        int selectedEncoding = self->selectedCodePage_ == 936 ? 1 : self->selectedCodePage_ == 20936 ? 2 : 0;
        if (hasSession && self->activeSession_->mode == 4) {
            SendMessageW(encoding, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自动（CMD 代码页）"));
            if (self->connection_ && std::strcmp(self->connection_->CodePageSource(), "manual") != 0)
                selectedEncoding = 3;
        }
        state->initialEncoding = selectedEncoding;
        SendMessageW(encoding, CB_SETCURSEL, selectedEncoding, 0);
        HWND ending = add(WC_COMBOBOXW, L"", IdSettingsEnding, 224, 168, 196, 180,
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL);
        for (auto value : {L"CR", L"LF", L"CRLF", L"不追加换行"})
            SendMessageW(ending, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        SendMessageW(ending, CB_SETCURSEL, hasSession && self->activeSession_->mode == 4 ? 2 :
            hasSession && self->activeSession_->mode == 0 ? 0 : self->lineEndingIndex_, 0);
        CheckDlgButton(dialog, IdSettingsEcho, self->localEchoEnabled_ ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IdSettingsTimestamp, self->timestampEnabled_ ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IdSettingsRawTrace,
            hasSession && self->activeSession_->rawTrace ? BST_CHECKED : BST_UNCHECKED);
        add(L"STATIC", L"会话操作 · 点击立即执行", -1, 16, 208, 404, 24);
        add(L"BUTTON", L"保存当前日志…", IdMenuSaveLog, 16, 240, 196, 32, button);
        add(L"BUTTON", L"清空终端", IdMenuClear, 224, 240, 196, 32, button);
        add(L"BUTTON", L"复制", IdMenuCopy, 16, 280, 128, 32, button);
        add(L"BUTTON", L"粘贴", IdMenuPaste, 156, 280, 128, 32, button);
        add(L"BUTTON", L"全选", IdMenuSelectAll, 296, 280, 124, 32, button);
        add(L"BUTTON", L"发送选中内容到 AI", IdSendSelectionToAi, 16, 320, 404, 32, button);
        add(L"STATIC", L"本程序实例 · 诊断与日志导出", -1, 16, 364, 404, 24);
        add(L"BUTTON", L"保存网络连接诊断…", IdMenuNetworkDiagnostics, 16, 392, 196, 32, button);
        add(L"BUTTON", L"取消日志导出", IdMenuCancelLogExport, 224, 392, 196, 32, button);
        HWND help = add(L"STATIC", L"", IdSettingsExportNote, 16, 436, 404, 24, SS_ENDELLIPSIS);
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = dialog;
        tool.uId = reinterpret_cast<UINT_PTR>(help);
        tool.lpszText = state->exportTip;
        SendMessageW(self->statusTip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(help, WM_SETFONT, reinterpret_cast<WPARAM>(self->smallFont_), TRUE);
        add(L"BUTTON", L"取消", IDCANCEL, 216, 468, 96, 36, button);
        add(L"BUTTON", L"保存", IDOK, 324, 468, 96, 36, button);
        HWND version = add(L"STATIC", L"SerialCtl " SERIALCTL_VERSION, -1, 16, 520, 136, 20, SS_LEFT);
        HWND credit = add(L"STATIC", L"Application design by jiojio", -1, 152, 520, 268, 20, SS_RIGHT);
        SetPropW(version, L"SerialCtl.Secondary", reinterpret_cast<HANDLE>(1));
        SetPropW(credit, L"SerialCtl.Secondary", reinterpret_cast<HANDLE>(1));
        SendMessageW(version, WM_SETFONT, reinterpret_cast<WPARAM>(self->smallFont_), FALSE);
        SendMessageW(credit, WM_SETFONT, reinterpret_cast<WPARAM>(self->smallFont_), FALSE);
        UiLayoutBatch layout;
        layout.ComboVisibleHeight(encoding, Ui::Scale(28));
        layout.ComboVisibleHeight(ending, Ui::Scale(28));
        layout.Commit();
        refresh();
        CenterOnOwner(dialog, self->window_);
        UiDialogViewport::Fit(dialog, Ui::Dpi);
        SetTimer(dialog, 1, 250, nullptr);
        return TRUE;
    }
    if (message == WM_TIMER) {
        refresh();
        return TRUE;
    }
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(dialog, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, self->panelBrush_);
        UiSeparator(reinterpret_cast<HDC>(wParam), Ui::Scale(16), client.right - Ui::Scale(16), client.bottom - Ui::Scale(44),
                    Colors(self->darkMode_).border);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ||
        message == WM_CTLCOLORLISTBOX) {
        const auto colors = Colors(self->darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lParam)) &&
                                 !GetPropW(reinterpret_cast<HWND>(lParam), L"SerialCtl.Secondary")
                             ? colors.text
                             : colors.muted);
        SetBkColor(dc, colors.panel);
        return reinterpret_cast<INT_PTR>(self->panelBrush_);
    }
    if (message == WM_MEASUREITEM) {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_COMBOBOX) {
            item.itemHeight = Ui::Scale(28);
            return TRUE;
        }
    }
    if (message == WM_DRAWITEM) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_DESTROY) {
        KillTimer(dialog, 1);
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = dialog;
        tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, IdSettingsExportNote));
        SendMessageW(self->statusTip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const UINT id = LOWORD(wParam);
        if (id == IDOK) {
            const bool echo = IsDlgButtonChecked(dialog, IdSettingsEcho) == BST_CHECKED;
            const bool timestamps = IsDlgButtonChecked(dialog, IdSettingsTimestamp) == BST_CHECKED;
            if (self->localEchoEnabled_ != echo) self->ExecuteTerminalAction(IdMenuLocalEcho);
            if (self->timestampEnabled_ != timestamps) self->ExecuteTerminalAction(IdMenuTimestamp);
            if (hasSession) {
                self->activeSession_->rawTrace = IsDlgButtonChecked(dialog, IdSettingsRawTrace) == BST_CHECKED;
                const auto encoding = SendDlgItemMessageW(dialog, IdSettingsEncoding, CB_GETCURSEL, 0, 0);
                // Do not freeze automatic CMD code pages merely by saving unrelated settings.
                if (encoding != state->initialEncoding && encoding >= 0 && encoding <= 3)
                    self->ExecuteTerminalAction(encoding == 3 ? IdMenuEncodingCmdAuto :
                        IdMenuEncodingUtf8 + static_cast<UINT>(encoding));
                if (IsWindowEnabled(GetDlgItem(dialog, IdSettingsEnding))) {
                    const auto ending = SendDlgItemMessageW(dialog, IdSettingsEnding, CB_GETCURSEL, 0, 0);
                    if (ending >= 0 && ending <= 3)
                        self->ExecuteTerminalAction(IdMenuEndingCr + static_cast<UINT>(ending));
                }
            }
            self->AppendStatus(state->sessionId && !hasSession ?
                L"全局设置已保存；原会话已切换，请重新打开设置修改会话选项" : L"终端设置已保存", false);
            UiEndOwnedDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IdSendSelectionToAi) {
            if (hasSession && self->HasTerminalSelection()) {
                const auto text = self->TerminalSelectionText(false);
                const auto target = "session-" + std::to_string(self->activeSession_->id);
                UiEndOwnedDialog(dialog, IDCANCEL);
                self->ShowAiPage(false); self->aiPane_.AttachSelection(text, target);
            }
            return TRUE;
        }
        if (id == IdMenuNetworkDiagnostics || id == IdMenuCancelLogExport ||
            id == IdMenuSaveLog || id == IdMenuClear ||
            id == IdMenuCopy || id == IdMenuPaste || id == IdMenuSelectAll) {
            refresh();
            if (IsWindowEnabled(GetDlgItem(dialog, id))) self->ExecuteTerminalAction(id, dialog);
            refresh();
            return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::ExecuteTerminalAction(UINT command, HWND owner) {
    switch (command) {
    case IdMenuCancelLogExport:
        CancelLogExport();
        break;
    case IdMenuNetworkDiagnostics:
        SaveNetworkDiagnostics(owner);
        break;
    case IdMenuEncodingCmdAuto:
        if (activeSession_ && activeSession_->mode == 4 && connection_) {
            connection_->OverrideTextCodePage(0);
            activeSession_->codePage = selectedCodePage_ = connection_->OutputCodePage();
            activeSession_->decoder.Reset();
            AppendStatus(L"CMD 编码已恢复自动检测；无法访问控制台时使用 OEM 并标明来源", false);
        }
        break;
    case IdMenuRawTrace:
        if (activeSession_)
            activeSession_->rawTrace = !activeSession_->rawTrace;
        break;
    case IdMenuPaste: PasteToTerminal(); break;
    case IdMenuEndingCr: case IdMenuEndingLf: case IdMenuEndingCrLf: case IdMenuEndingNone:
        if (selectedMode_ != 0) { lineEndingIndex_ = static_cast<int>(command - IdMenuEndingCr); if (activeSession_) activeSession_->lineEndingIndex = lineEndingIndex_; }
        break;
    case IdMenuLocalEcho:
        localEchoEnabled_ = !localEchoEnabled_;
        AppendStatus(localEchoEnabled_ ? L"本地回显已开启" : L"本地回显已关闭", false);
        break;
    case IdMenuTimestamp:
        timestampEnabled_ = !timestampEnabled_;
        UpdateTerminalDimensions();
        InvalidateRect(terminal_, nullptr, TRUE);
        AppendStatus(timestampEnabled_ ? L"毫秒时间戳已开启" : L"毫秒时间戳已关闭", false);
        break;
    case IdMenuEncodingUtf8:
    case IdMenuEncodingGbk:
    case IdMenuEncodingGb2312: {
        selectedCodePage_ = command == IdMenuEncodingUtf8 ? CP_UTF8 :
            (command == IdMenuEncodingGbk ? 936 : 20936);
        if (activeSession_) {
            if (activeSession_->mode == 4 && connection_)
                connection_->OverrideTextCodePage(selectedCodePage_);
            activeSession_->codePage = selectedCodePage_;
            activeSession_->decoder.Reset();
        }
        const wchar_t* name = selectedCodePage_ == CP_UTF8 ? L"UTF-8" :
            (selectedCodePage_ == 936 ? L"GBK" : L"GB2312");
        AppendStatus(std::wstring(L"终端编码已切换为 ") + name + L"，仅影响后续收发", false);
        break;
    }
    case IdMenuSaveLog:
        SaveCurrentLog(owner);
        break;
    case IdMenuClear:
        if (activeSession_) {
            activeSession_->terminal.Clear();
            activeSession_->terminalScrollOffset = 0;
            activeSession_->terminalHasNewOutput = false;
        }
        ClearTerminalSelection();
        InvalidateRect(terminal_, nullptr, TRUE);
        AppendStatus(L"终端内容已清空", false);
        break;
    case IdMenuCopy:
        SendMessageW(terminal_, WM_COPY, 0, 0);
        break;
    case IdMenuSelectAll:
        if (activeSession_ && activeSession_->terminal.DisplayLineCount() > 0) {
            terminalSelectionAnchorLine_ = 0;
            terminalSelectionAnchorColumn_ = 0;
            terminalSelectionFocusLine_ = activeSession_->terminal.DisplayLineCount() - 1;
            terminalSelectionFocusColumn_ = activeSession_->terminal.Columns();
            terminalSelectionActive_ = true;
            InvalidateRect(terminal_, nullptr, FALSE);
        }
        break;
    default:
        break;
    }
}

void MainWindow::PasteToTerminal() {
    if (runningCommandIndex_ >= 0 || !activeSession_ || !connection_ || !connection_->IsConnected()) return;
    if (!OpenClipboard(terminal_)) {
        AppendStatus(L"剪贴板正被其他程序使用，请重试粘贴", true);
        return;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const auto* value = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    std::wstring pasted;
    bool clipboardInvalid = false;
    bool clipboardTooLarge = false;
    bool clipboardAllocationFailed = false;
    if (value) {
        const size_t capacity = GlobalSize(handle) / sizeof(wchar_t);
        const size_t maximumUnits = selectedMode_ == 4 ? 65536 : 4 * 1024 * 1024;
        const size_t inspectedUnits = std::min(capacity, maximumUnits + 1);
        const auto* end = std::find(value, value + inspectedUnits, L'\0');
        if (end == value + inspectedUnits) {
            clipboardTooLarge = capacity > maximumUnits;
            clipboardInvalid = !clipboardTooLarge;
        } else {
            try { pasted.assign(value, end); }
            catch (const std::bad_alloc&) { clipboardAllocationFailed = true; }
        }
        GlobalUnlock(handle);
    }
    CloseClipboard();
    if (clipboardTooLarge || clipboardInvalid || clipboardAllocationFailed) {
        AppendStatus(clipboardTooLarge ? L"剪贴板文本超过粘贴上限；未发送，原草稿保留" :
            clipboardInvalid ? L"剪贴板文本格式无效；未发送，原草稿保留" :
            L"内存不足，无法粘贴；未发送，原草稿保留", true);
        return;
    }
    if (pasted.empty()) return;

    if (selectedMode_ == 4) {
        auto &editor = activeSession_->cmdEditor;
        CmdLineEditor::PastePlan plan;
        if (!editor.PreparePaste(pasted, plan)) {
            AppendStatus(L"CMD 粘贴包含不支持的控制字符或超长行；未发送，原草稿保留", true);
            return;
        }
        // Validate ALL complete lines and the incomplete tail before enqueueing
        // any command. A rejected tail must not leave an already executed prefix.
        const auto combined = plan.submitted + plan.tail;
        const auto encodedAll = EncodeTerminalText(combined);
        if (encodedAll.empty()) return;
        try {
            Bytes validation(encodedAll.begin(), encodedAll.end());
            if (!plan.tail.empty()) {
                validation.push_back('\r');
                validation.push_back('\n');
            }
            const auto normalized = SessionService::CmdBatch(validation);
            if (normalized.size() > 65536) throw std::runtime_error("CMD paste batch is too large");
        } catch (const std::exception&) {
            AppendStatus(L"CMD 粘贴超过单行 8191 字节或批次 65536 字节限制；未发送，原草稿保留", true);
            return;
        }
        if (!plan.submitted.empty()) {
            const auto encoded = EncodeTerminalText(plan.submitted);
            if (encoded.empty() || !SendBytesToActive(Bytes(encoded.begin(), encoded.end()), false)) return;
        }
        editor.CommitPaste(plan);
        ScrollTerminalToBottom();
        ClearTerminalSelection();
        InvalidateRect(terminal_, nullptr, FALSE);
        return;
    }
    std::wstring transmitted = pasted;
    if (activeSession_->terminal.BracketedPaste())
        transmitted = std::wstring(1, 0x1b) + L"[200~" + pasted +
            std::wstring(1, 0x1b) + L"[201~";
    const std::string encoded = EncodeTerminalText(transmitted);
    if (!encoded.empty()) {
        if (SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_, pasted)) {
            ScrollTerminalToBottom();
            ClearTerminalSelection();
        }
    }
}

void MainWindow::UpdateTerminalDimensions() {
    if (!terminal_ || !terminalFont_ || rightPanelAnimating_ || rightPanelDragging_ || windowResizing_)
        return;
    RECT client{};
    GetClientRect(terminal_, &client);
    HDC dc = GetDC(terminal_);
    HFONT previous = reinterpret_cast<HFONT>(SelectObject(dc, terminalFont_));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SIZE characterSize{};
    GetTextExtentPoint32W(dc, L"M", 1, &characterSize);
    SelectObject(dc, previous);
    ReleaseDC(terminal_, dc);
    terminalCellWidth_ = std::max(Ui::Scale(7), static_cast<int>(characterSize.cx));
    terminalLineHeight_ = std::max(Ui::Scale(15), static_cast<int>(metrics.tmHeight) + Ui::Scale(2));
    const bool showTimestamps =
        timestampEnabled_ && !LiveMonitorPage() && activeSession_ && !activeSession_->terminal.AlternateScreen();
    const int timestampWidth = showTimestamps ? Ui::TerminalTimestampWidth : 0;
    const int contentWidth = std::max(1,
        static_cast<int>(client.right) - Ui::TerminalPadding * 2 - timestampWidth - Ui::OverlayScrollLaneWidth);
    const int contentHeight = std::max(1, static_cast<int>(client.bottom) - Ui::TerminalPadding * 2);
    const int oldColumns = terminalVisibleColumns_, oldRows = terminalVisibleRows_;
    terminalVisibleColumns_ = std::max(20, contentWidth / terminalCellWidth_);
    terminalVisibleRows_ = std::max(2, contentHeight / terminalLineHeight_);

    for (const auto& session : sessions_) {
        auto &model = session->terminal;
        const size_t oldTop =
            model.DisplayLineCount() > static_cast<size_t>(oldRows + session->terminalScrollOffset)
                ? model.DisplayLineCount() - static_cast<size_t>(oldRows + session->terminalScrollOffset)
                : 0;
        auto readAnchor = model.CaptureAnchor(oldTop);
        auto selectionAnchor = model.CaptureAnchor(terminalSelectionAnchorLine_, terminalSelectionAnchorColumn_);
        auto selectionFocus = model.CaptureAnchor(terminalSelectionFocusLine_, terminalSelectionFocusColumn_);
        model.SetLineWrapView(!LiveMonitorPage() || (powerVisible_ ? powerMonitorWrap_ : aiMonitorWrap_));
        session->terminal.Resize(terminalVisibleColumns_, terminalVisibleRows_);
        if (session->terminalScrollOffset > 0) {
            auto line = model.LocateAnchor(readAnchor).first;
            session->terminalScrollOffset =
                std::max(0, static_cast<int>(model.DisplayLineCount() - std::min(line, model.DisplayLineCount())) -
                                terminalVisibleRows_);
        }
        if (session.get() == activeSession_ && terminalSelectionActive_) {
            auto a = model.LocateAnchor(selectionAnchor), b = model.LocateAnchor(selectionFocus);
            terminalSelectionAnchorLine_ = a.first;
            terminalSelectionAnchorColumn_ = a.second;
            terminalSelectionFocusLine_ = b.first;
            terminalSelectionFocusColumn_ = b.second;
        }
        if (session->connection && (oldColumns != terminalVisibleColumns_ || oldRows != terminalVisibleRows_))
            session->connection->ResizeTerminal(terminalVisibleColumns_, terminalVisibleRows_);
        const int maximum = std::max(0,
            static_cast<int>(session->terminal.DisplayLineCount()) - terminalVisibleRows_);
        session->terminalScrollOffset = std::min(session->terminalScrollOffset, maximum);
        const int maximumHorizontal = std::max(0,
            static_cast<int>(session->terminal.MaximumDisplayColumns()) - terminalVisibleColumns_);
        session->terminalHorizontalOffset = std::max(0,
            std::min(session->terminalHorizontalOffset, maximumHorizontal));
    }
    InvalidateRect(terminal_, nullptr, TRUE);
}

void MainWindow::PaintTerminal(HDC dc) {
    RECT client{};
    GetClientRect(terminal_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    const Palette colors = Colors(darkMode_);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HBITMAP oldBitmap = reinterpret_cast<HBITMAP>(SelectObject(buffer, bitmap));
    HBRUSH background = CreateSolidBrush(colors.terminal);
    FillRect(buffer, &client, background);
    DeleteObject(background);

    if (activeSession_) {
        const TerminalModel& model = activeSession_->terminal;
        std::unique_ptr<TerminalModel> preview;
        if (activeSession_->mode == 4 && !activeSession_->cmdEditor.Text().empty() &&
            activeSession_->terminalScrollOffset == 0)
            preview = std::make_unique<TerminalModel>(
                model.PreviewInput(activeSession_->cmdEditor.Text(), activeSession_->cmdEditor.Cursor()));
        auto displayLine = [&](size_t i) -> const TerminalLine & {
            return preview && !model.LineWrapView()      ? preview->DisplayLine(i)
                   : preview && i >= model.HistorySize() ? preview->DisplayLine(i - model.HistorySize())
                                                         : model.DisplayLine(i);
        };
        const bool showTimestamps = timestampEnabled_ && !LiveMonitorPage() && !model.AlternateScreen();
        const int padding = Ui::TerminalPadding;
        const int timestampWidth = showTimestamps ? Ui::TerminalTimestampWidth : 0;
        const int textLeft = padding + timestampWidth;
        const size_t totalLines =
            (preview ? (model.LineWrapView() ? model.HistorySize() : 0) + preview->DisplayLineCount()
                     : model.DisplayLineCount());
        const size_t totalColumns = preview ? preview->MaximumDisplayColumns() : model.MaximumDisplayColumns();
        const int maximumHorizontalOffset = std::max(0,
            static_cast<int>(totalColumns) - terminalVisibleColumns_);
        activeSession_->terminalHorizontalOffset = std::max(0,
            std::min(activeSession_->terminalHorizontalOffset, maximumHorizontalOffset));
        const int horizontalOffset = activeSession_->terminalHorizontalOffset;
        const int maximumOffset = std::max(0,
            static_cast<int>(totalLines) - terminalVisibleRows_);
        activeSession_->terminalScrollOffset = std::max(0,
            std::min(activeSession_->terminalScrollOffset, maximumOffset));
        const size_t topLine = totalLines > static_cast<size_t>(terminalVisibleRows_ +
            activeSession_->terminalScrollOffset)
            ? totalLines - static_cast<size_t>(terminalVisibleRows_ +
                activeSession_->terminalScrollOffset)
            : 0;
        if (measurePaint_) {
            SetPropW(terminal_, L"SerialCtl.CellWidth", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(terminalCellWidth_)));
            SetPropW(terminal_, L"SerialCtl.LineHeight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(terminalLineHeight_)));
            SetPropW(terminal_, L"SerialCtl.TextLeft", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(textLeft)));
            SetPropW(terminal_, L"SerialCtl.TextTop", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(padding)));
            SetPropW(terminal_, L"SerialCtl.VisibleColumns", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(terminalVisibleColumns_)));
            SetPropW(terminal_, L"SerialCtl.VisibleRows", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(terminalVisibleRows_)));
            SetPropW(terminal_, L"SerialCtl.CmdDraftLength", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->cmdEditor.Text().size()) + 1));
            const auto anchor = model.CaptureAnchor(topLine);
            SetPropW(terminal_, L"SerialCtl.ScrollOffset",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->terminalScrollOffset) + 1));
            SetPropW(terminal_, L"SerialCtl.ReadAnchor", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(anchor.id)));
            SetPropW(terminal_, L"SerialCtl.ReadOffset",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(anchor.offset) + 1));
            SetPropW(terminal_, L"SerialCtl.NewOutput",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->terminalHasNewOutput) + 1));
            SetPropW(terminal_, L"SerialCtl.CmdInteractionBlocked",
                     reinterpret_cast<HANDLE>(static_cast<INT_PTR>(activeSession_->cmdInteractionWarning) + 1));
        }

        size_t selectionStartLine = terminalSelectionAnchorLine_;
        size_t selectionEndLine = terminalSelectionFocusLine_;
        int selectionStartColumn = terminalSelectionAnchorColumn_;
        int selectionEndColumn = terminalSelectionFocusColumn_;
        if (selectionStartLine > selectionEndLine ||
            (selectionStartLine == selectionEndLine && selectionStartColumn > selectionEndColumn)) {
            std::swap(selectionStartLine, selectionEndLine);
            std::swap(selectionStartColumn, selectionEndColumn);
        }

        HFONT previousFont = reinterpret_cast<HFONT>(SelectObject(buffer, terminalFont_));
        SetBkMode(buffer, TRANSPARENT);
        for (int viewRow = 0; viewRow < terminalVisibleRows_; ++viewRow) {
            const size_t lineIndex = topLine + static_cast<size_t>(viewRow);
            if (lineIndex >= totalLines) break;
            const TerminalLine &line = displayLine(lineIndex);
            const int y = padding + viewRow * terminalLineHeight_;
            if (showTimestamps && !line.timestamp.empty()) {
                RECT timestampRect{padding, y, textLeft - Ui::Gap, y + terminalLineHeight_};
                DrawTextSimple(buffer, line.timestamp, timestampRect, colors.muted,
                    smallFont_, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }

            const int availableColumns = std::max(0,
                (static_cast<int>(client.right) - textLeft - Ui::OverlayScrollLaneWidth) /
                    terminalCellWidth_);
            for (int viewColumn = 0; viewColumn < availableColumns; ++viewColumn) {
                const int column = viewColumn + horizontalOffset;
                if (column < 0 || static_cast<size_t>(column) >= line.cells.size()) break;
                const TerminalCell& cell = line.cells[static_cast<size_t>(column)];
                if (cell.continuation) continue;
                const int width = static_cast<size_t>(column + 1) < line.cells.size() &&
                    line.cells[static_cast<size_t>(column + 1)].continuation ? 2 : 1;
                RECT cellRect{textLeft + viewColumn * terminalCellWidth_, y,
                    textLeft + (viewColumn + width) * terminalCellWidth_, y + terminalLineHeight_};
                const bool selected = terminalSelectionActive_ &&
                    (lineIndex > selectionStartLine ||
                        (lineIndex == selectionStartLine && column >= selectionStartColumn)) &&
                    (lineIndex < selectionEndLine ||
                        (lineIndex == selectionEndLine && column <= selectionEndColumn));
                COLORREF foreground = ResolveTerminalColor(cell.attributes.foreground,
                    colors.text, darkMode_, cell.attributes.bold);
                COLORREF cellBackground = ResolveTerminalColor(cell.attributes.background,
                    colors.terminal, darkMode_, false);
                if (cell.attributes.inverse) std::swap(foreground, cellBackground);
                if (selected) {
                    cellBackground = colors.accentSoft;
                    foreground = colors.text;
                }
                if (cellBackground != colors.terminal) {
                    HBRUSH cellBrush = CreateSolidBrush(cellBackground);
                    FillRect(buffer, &cellRect, cellBrush);
                    DeleteObject(cellBrush);
                }
                if (cell.character != L' ') {
                    SetTextColor(buffer, foreground);
                    TextOutW(buffer, cellRect.left, cellRect.top,
                        &cell.character, 1);
                    if (cell.attributes.bold)
                        TextOutW(buffer, cellRect.left + 1, cellRect.top,
                            &cell.character, 1);
                }
                if (cell.attributes.underline) {
                    HPEN underline = CreatePen(PS_SOLID, 1, foreground);
                    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(buffer, underline));
                    MoveToEx(buffer, cellRect.left, cellRect.bottom - 2, nullptr);
                    LineTo(buffer, cellRect.right, cellRect.bottom - 2);
                    SelectObject(buffer, oldPen);
                    DeleteObject(underline);
                }
            }
        }

        if (GetFocus() == terminal_ && model.CursorVisible() &&
            activeSession_->terminalScrollOffset == 0) {
            const size_t cursorLine =
                preview ? (model.LineWrapView() ? model.HistorySize() : 0) + preview->CursorDisplayLine()
                        : model.CursorDisplayLine();
            const int cursorColumn = (preview ? preview->CursorColumn() : model.CursorColumn()) - horizontalOffset;
            if (cursorLine >= topLine &&
                cursorLine < topLine + static_cast<size_t>(terminalVisibleRows_) &&
                cursorColumn >= 0 && cursorColumn < terminalVisibleColumns_) {
                const int viewRow = static_cast<int>(cursorLine - topLine);
                RECT cursor{textLeft + cursorColumn * terminalCellWidth_,
                    padding + viewRow * terminalLineHeight_,
                    textLeft + (cursorColumn + 1) * terminalCellWidth_,
                    padding + (viewRow + 1) * terminalLineHeight_};
                HPEN cursorPen = CreatePen(PS_SOLID, 2, colors.accent);
                HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(buffer, cursorPen));
                MoveToEx(buffer, cursor.left, cursor.bottom - 1, nullptr);
                LineTo(buffer, cursor.right, cursor.bottom - 1);
                SelectObject(buffer, oldPen);
                DeleteObject(cursorPen);
            }
        }
        SelectObject(buffer, previousFont);

        const TerminalScrollMetrics scroll = GetTerminalScrollMetrics(terminal_,
            totalLines, terminalVisibleRows_, activeSession_->terminalScrollOffset);
        if (scroll.visible)
            DrawRoundedBox(buffer, scroll.thumb, Ui::Scale(3), colors.border, colors.border);
        const TerminalHorizontalScrollMetrics horizontalScroll =
            GetTerminalHorizontalScrollMetrics(terminal_, textLeft, totalColumns,
                terminalVisibleColumns_, activeSession_->terminalHorizontalOffset);
        if (horizontalScroll.visible)
            DrawRoundedBox(buffer, horizontalScroll.thumb, Ui::Scale(3), colors.border, colors.border);

        if (activeSession_->terminalHasNewOutput && activeSession_->terminalScrollOffset > 0) {
            const RECT notification = TerminalNewOutputRect(client, horizontalScroll.visible);
            DrawRoundedBox(buffer, notification, Ui::Radius, colors.accentSoft, colors.accent);
            DrawTextSimple(buffer, L"↓  新输出", notification, UiAccentInk(darkMode_),
                smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    if (measurePaint_ && activeSession_ && activeSession_->pendingProbeArrival) {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        auto microseconds =
            static_cast<INT_PTR>((now.QuadPart - activeSession_->pendingProbeArrival) * 1000000 / frequency.QuadPart);
        SetPropW(terminal_, L"SerialCtl.PaintUsec", reinterpret_cast<HANDLE>(std::max<INT_PTR>(1, microseconds)));
        SetPropW(terminal_, L"SerialCtl.PaintSample", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(++paintSample_)));
        SetPropW(terminal_, L"SerialCtl.PaintProbe", reinterpret_cast<HANDLE>(activeSession_->paintProbe));
        activeSession_->pendingProbeArrival = 0;
    }
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

void MainWindow::ScrollTerminal(int lines) {
    if (!activeSession_) return;
    const int maximum = std::max(0,
        static_cast<int>(activeSession_->terminal.DisplayLineCount()) - terminalVisibleRows_);
    activeSession_->terminalScrollOffset = std::max(0,
        std::min(maximum, activeSession_->terminalScrollOffset + lines));
    if (activeSession_->terminalScrollOffset == 0)
        activeSession_->terminalHasNewOutput = false;
    InvalidateRect(terminal_, nullptr, FALSE);
}

void MainWindow::ScrollTerminalHorizontal(int columns) {
    if (!activeSession_) return;
    const int maximum = std::max(0,
        static_cast<int>(activeSession_->terminal.MaximumDisplayColumns()) -
            terminalVisibleColumns_);
    activeSession_->terminalHorizontalOffset = std::max(0,
        std::min(maximum, activeSession_->terminalHorizontalOffset + columns));
    InvalidateRect(terminal_, nullptr, FALSE);
}

void MainWindow::ScrollTerminalToBottom() {
    if (!activeSession_) return;
    activeSession_->terminalScrollOffset = 0;
    activeSession_->terminalHasNewOutput = false;
    InvalidateRect(terminal_, nullptr, FALSE);
}

void MainWindow::ZoomTerminalFont(int steps) {
    if (steps == 0 || !terminal_) return;
    const int nextHeight = std::max(Ui::TerminalFontMinimumHeight,
        std::min(Ui::TerminalFontMaximumHeight, terminalFontHeight_ + Ui::Scale(steps)));
    if (nextHeight == terminalFontHeight_) return;
    HFONT nextFont = CreateFontW(-nextHeight, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (!nextFont) return;
    HFONT oldFont = terminalFont_;
    terminalFont_ = nextFont;
    terminalFontHeight_ = nextHeight;
    SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), FALSE);
    UpdateTerminalDimensions();
    if (oldFont) DeleteObject(oldFont);
    AppendStatus(L"终端字号：" + std::to_wstring(terminalFontHeight_), false);
}

bool MainWindow::TerminalPointToCell(POINT point, size_t& line, int& column) const {
    if (!activeSession_) return false;
    RECT client{};
    GetClientRect(terminal_, &client);
    const TerminalModel& model = activeSession_->terminal;
    const int timestampWidth =
        timestampEnabled_ && !LiveMonitorPage() && !model.AlternateScreen() ? Ui::TerminalTimestampWidth : 0;
    const int textLeft = Ui::TerminalPadding + timestampWidth;
    const size_t totalLines = model.DisplayLineCount();
    const size_t topLine = totalLines > static_cast<size_t>(terminalVisibleRows_ +
        activeSession_->terminalScrollOffset)
        ? totalLines - static_cast<size_t>(terminalVisibleRows_ +
            activeSession_->terminalScrollOffset)
        : 0;
    const int row = std::max(0, std::min(terminalVisibleRows_ - 1,
        (static_cast<int>(point.y) - Ui::TerminalPadding) / std::max(1, terminalLineHeight_)));
    if (totalLines == 0 || point.x < textLeft ||
        point.x >= client.right - Ui::OverlayScrollLaneWidth) return false;
    line = std::min(totalLines - 1, topLine + static_cast<size_t>(row));
    const TerminalLine& terminalLine = model.DisplayLine(line);
    if (terminalLine.cells.empty()) return false;
    const int viewColumn = (static_cast<int>(point.x) - textLeft) /
        std::max(1, terminalCellWidth_);
    column = std::max(0, std::min(static_cast<int>(terminalLine.cells.size()) - 1,
        viewColumn + activeSession_->terminalHorizontalOffset));
    if (column > 0 && terminalLine.cells[static_cast<size_t>(column)].continuation) --column;
    return true;
}

void MainWindow::UpdateTerminalSelection(size_t line, int column, bool extend) {
    if (!extend) {
        terminalSelectionAnchorLine_ = line;
        terminalSelectionAnchorColumn_ = column;
        terminalSelectionFocusLine_ = line;
        terminalSelectionFocusColumn_ = column;
        terminalSelectionActive_ = false;
    } else {
        terminalSelectionFocusLine_ = line;
        terminalSelectionFocusColumn_ = column;
        terminalSelectionActive_ = terminalSelectionAnchorLine_ != terminalSelectionFocusLine_ ||
            terminalSelectionAnchorColumn_ != terminalSelectionFocusColumn_;
    }
    InvalidateRect(terminal_, nullptr, FALSE);
}

void MainWindow::ClearTerminalSelection() {
    terminalSelecting_ = false;
    terminalSelectionActive_ = false;
    InvalidateRect(terminal_, nullptr, FALSE);
}

bool MainWindow::HasTerminalSelection() const {
    return activeSession_ && terminalSelectionActive_;
}

std::wstring MainWindow::TerminalSelectionText(bool selectAll) const {
    if (!activeSession_) return {};
    const TerminalModel& model = activeSession_->terminal;
    if (selectAll) return model.PlainText(false);
    if (!terminalSelectionActive_) return {};

    return model.SelectedText(terminalSelectionAnchorLine_, terminalSelectionAnchorColumn_,
        terminalSelectionFocusLine_, terminalSelectionFocusColumn_);
}

void MainWindow::CopyTerminalSelection(bool selectAll) {
    const std::wstring text = TerminalSelectionText(selectAll);
    if (text.empty()) return;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* destination = memory ? GlobalLock(memory) : nullptr;
    if (!destination) {
        if (memory) GlobalFree(memory);
        AppendStatus(L"无法分配复制所需内存；原剪贴板未改动", true);
        return;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(terminal_)) {
        GlobalFree(memory);
        AppendStatus(L"剪贴板正被其他程序使用，选区已保留，请重试复制", true);
        return;
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!copied) GlobalFree(memory);
    CloseClipboard();
    if (!copied) AppendStatus(L"复制失败，选区已保留，请重试复制", true);
}

std::string MainWindow::EncodeTerminalText(const std::wstring &text) {
    const UINT page = SelectedCodePage();
    if (selectedMode_ != 4)
        return WideToMultiByte(text, page);
    BOOL replacement = FALSE;
    const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    auto used = page == CP_UTF8 ? nullptr : &replacement;
    int size = WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, used);
    if (size <= 0 || replacement) {
        AppendStatus(L"当前 CMD 代码页无法表示输入，请先用 chcp 切换代码页", true);
        return {};
    }
    std::string encoded(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(page, flags, text.data(), static_cast<int>(text.size()), encoded.data(), size, nullptr, used);
    if (replacement) {
        AppendStatus(L"当前 CMD 代码页无法表示输入，请先用 chcp 切换代码页", true);
        return {};
    }
    return encoded;
}

UINT MainWindow::SelectedCodePage() const {
    if (selectedMode_ == 4 && connection_ && connection_->InputCodePage())
        return connection_->InputCodePage();
    return selectedCodePage_ == 20936 && !IsValidCodePage(20936) ? 936 : selectedCodePage_;
}

std::wstring MainWindow::SelectedLineEnding() const {
    switch (lineEndingIndex_) {
    case 0: return L"\r";
    case 1: return L"\n";
    case 2: return L"\r\n";
    default: return {};
    }
}

void MainWindow::SetConnectedUi(bool connected) {
    ShowWindow(disconnectButton_, (aiVisible_ || (activeSession_ && !powerVisible_)) ? SW_SHOW : SW_HIDE);
    EnableWindow(disconnectButton_, aiVisible_ ? aiPane_.Busy() : activeSession_ != nullptr);

    EnableWindow(sftpTabButton_, connected && selectedMode_ == 0 && SftpClient::IsAvailable());


    SetSftpBusy(sftpBusy_);
    UpdateCommandActions();
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);



    InvalidateRect(window_, nullptr, TRUE);
}

void MainWindow::RefreshConnectionList() {
    apiServer_.SetSessions(sessionService_.Ids());
    size_t selectedIndex = 0;
    if (activeSession_) {
        const auto selected = std::find_if(sessions_.begin(), sessions_.end(),
            [this](const std::unique_ptr<SessionState>& item) { return item.get() == activeSession_; });
        if (selected != sessions_.end()) selectedIndex = static_cast<size_t>(selected - sessions_.begin());
    }
    const LRESULT top = SendMessageW(connectionList_, LB_GETTOPINDEX, 0, 0);
    const auto topId = top >= 0 && static_cast<size_t>(top) < connectionListIds_.size()
                           ? connectionListIds_[static_cast<size_t>(top)]
                           : UINT64_MAX;
    std::vector<std::uint64_t> ids;
    for (const auto &session : sessions_)
        ids.push_back(session->id);
    if (powerPageOpened_)
        ids.push_back(0);
    if (aiPageOpened_) ids.push_back(UINT64_MAX - 1);
    bool same = ids == connectionListIds_ &&
                SendMessageW(connectionList_, LB_GETCOUNT, 0, 0) == static_cast<LRESULT>(ids.size());
    if (same)
        for (size_t i = 0; i < sessions_.size(); ++i) {
            auto length = SendMessageW(connectionList_, LB_GETTEXTLEN, i, 0);
            std::vector<wchar_t> text(static_cast<size_t>(std::max<LRESULT>(length, 0)) + 1);
            SendMessageW(connectionList_, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data()));
            if (sessions_[i]->name != text.data()) {
                same = false;
                break;
            }
        }
    if (!same) {
        SendMessageW(connectionList_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(connectionList_, LB_RESETCONTENT, 0, 0);
        for (const auto &session : sessions_)
            SendMessageW(connectionList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(session->name.c_str()));
        if (powerPageOpened_)
            SendMessageW(connectionList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"IT6332A"));
        if (aiPageOpened_) SendMessageW(connectionList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AI 助手"));
        auto found = std::find(ids.begin(), ids.end(), topId);
        const auto anchored = found == ids.end() ? top : static_cast<LRESULT>(std::distance(ids.begin(), found));
        SendMessageW(connectionList_, LB_SETTOPINDEX, anchored, 0);
        SendMessageW(connectionList_, WM_SETREDRAW, TRUE, 0);
    }
    connectionListIds_ = std::move(ids);
    aiPane_.RefreshTargets();
    if(powerVisible_)selectedIndex=sessions_.size();
    if(aiVisible_)selectedIndex=sessions_.size()+(powerPageOpened_?1:0);
    if (!sessions_.empty() || powerPageOpened_ || aiPageOpened_) {
        auto keepTop = SendMessageW(connectionList_, LB_GETTOPINDEX, 0, 0);
        SendMessageW(connectionList_, LB_SETCURSEL, selectedIndex, 0);
        SendMessageW(connectionList_, LB_SETTOPINDEX, keepTop, 0);
    }
    InvalidateRect(connectionList_, nullptr, TRUE);
    RefreshPowerTargets();
}

void MainWindow::RefreshPowerTargets() {
    if (!powerMonitorTarget_) return;
    std::vector<std::string> ids;
    std::vector<std::wstring> labels;
    bool eligible = false;
    std::wstring selectedLabel;
    for (const auto &session : sessions_) {
        if (!session->connection || !session->connection->IsConnected()) continue;
        const auto id = "session-" + std::to_string(session->id);
        ids.push_back(id);
        labels.push_back(session->name);
        if (id == powerMonitorSession_) {
            selectedLabel = session->name;
            eligible = session->mode == 1 || session->mode == 3;
        }
    }
    if (ids != powerMonitorIds_ || labels != powerMonitorLabels_ || SendMessageW(powerMonitorTarget_,CB_GETCOUNT,0,0)==0) {
        powerMonitorIds_ = ids;
        powerMonitorLabels_ = labels;
        SendMessageW(powerMonitorTarget_, CB_RESETCONTENT, 0, 0);
        SendMessageW(powerMonitorTarget_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"选择实时终端…"));
        for (const auto &label : labels) SendMessageW(powerMonitorTarget_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const auto selected = std::find(ids.begin(), ids.end(), powerMonitorSession_);
    if (selected == ids.end()) powerMonitorSession_.clear();
    SendMessageW(powerMonitorTarget_, CB_SETCURSEL, selected == ids.end() ? 0 : selected - ids.begin() + 1, 0);
    powerPane_.SetRecoveryTarget(powerMonitorSession_, selectedLabel, eligible);
}

void MainWindow::SyncPowerTerminal() {
    if (!powerVisible_) return;
    SetWindowTextW(aiTerminalHeader_, L"实时终端");
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (powerMonitorSession_ == "session-" + std::to_string(sessions_[i]->id)) {
            if (activeSession_ != sessions_[i].get()) SwitchSession(i, SessionFocus::PowerPreview);
            RECT r{}; GetClientRect(window_, &r); LayoutControls(r.right, r.bottom);
            return;
        }
    ShowWindow(terminal_, SW_HIDE);
    RECT r{}; GetClientRect(window_, &r); LayoutControls(r.right, r.bottom);
}

bool MainWindow::SidebarCollapsed() const {
    return (powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) || rightPanelAutoCollapsed_ || rightPanelAnimating_;
}
int MainWindow::SidebarWidth() const {
    if (!LiveMonitorPage())
        return rightPanelWidth_;
    RECT client{};
    GetClientRect(window_, &client);
    const int left = std::clamp(static_cast<int>(client.right) * 18 / 100, Ui::MinLeftWidth, Ui::MaxLeftWidth);
    return std::max(Ui::MinRightWidth,
                    MulDiv(std::max(1, static_cast<int>(client.right) - left - Ui::Gap * 3), powerVisible_ ? powerMonitorRatio_ : aiMonitorRatio_, 1000) +
                        Ui::Gap);
}
void MainWindow::SyncAiTerminal() {
    if (!aiVisible_) return;
    const auto& id = aiPane_.AssociatedTarget();
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (id == "session-" + std::to_string(sessions_[i]->id)) {
            if (activeSession_ != sessions_[i].get()) SwitchSession(i, SessionFocus::AiPreview);
            SetWindowTextW(aiTerminalHeader_, (L"实时终端 · " + sessions_[i]->name).c_str());
            RECT r{}; GetClientRect(window_, &r); LayoutControls(r.right, r.bottom);
            return;
        }
    SetWindowTextW(aiTerminalHeader_, L"选择关联终端后显示实时输出");
    ShowWindow(terminal_, SW_HIDE);
}

void MainWindow::LoadUiState() {
    std::wstring directory = DirectoryOf(DefaultCommandsPath());
    if (directory.empty()) {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        directory = DirectoryOf(modulePath);
    }
    const std::wstring path = JoinLocalPath(directory, L"settings.ini");
    petSettings_.enabled = GetPrivateProfileIntW(L"Pet", L"Enabled", 1, path.c_str()) != 0;
    petSettings_.follow = GetPrivateProfileIntW(L"Pet", L"Follow", 1, path.c_str()) != 0;
    petSettings_.opening = GetPrivateProfileIntW(L"Pet", L"Opening", 1, path.c_str()) != 0;
    petSettings_.desktop = GetPrivateProfileIntW(L"Pet", L"Desktop", 1, path.c_str()) != 0;
    petSettings_.topmost = GetPrivateProfileIntW(L"Pet", L"Topmost", 1, path.c_str()) != 0;
    petSettings_.desktopX = static_cast<int>(GetPrivateProfileIntW(L"Pet", L"DesktopX", -2147483647, path.c_str()));
    petSettings_.desktopY = static_cast<int>(GetPrivateProfileIntW(L"Pet", L"DesktopY", -2147483647, path.c_str()));
    petSettings_.size = static_cast<int>(GetPrivateProfileIntW(L"Pet", L"Size", 96, path.c_str()));
    petSettings_.x = static_cast<int>(GetPrivateProfileIntW(L"Pet", L"X", 500, path.c_str()));
    petSettings_.y = static_cast<int>(GetPrivateProfileIntW(L"Pet", L"Y", 500, path.c_str()));
    petSettings_.Normalize();
    const bool logical = GetPrivateProfileIntW(L"Layout", L"LayoutVersion", 0, path.c_str()) >= 2;
    const int savedDpi = std::clamp(static_cast<int>(GetPrivateProfileIntW(
        L"Layout", L"SavedDpi", Ui::Dpi, path.c_str())), 48, 768);
    auto width = [&](const wchar_t *key, const wchar_t *logicalKey, int fallback, int minimum, int maximum) {
        int value = static_cast<int>(GetPrivateProfileIntW(L"Layout", logical ? logicalKey : key, 0, path.c_str()));
        if (!value) value = fallback;
        else if (!logical) value = MulDiv(value, 96, savedDpi);
        return Ui::Scale(std::clamp(value, minimum, maximum));
    };
    rightPanelWidth_ = width(L"RightPanelWidth", L"RightPanelWidth96", 360, 260, 650);
    sftpNameWidth_ = width(L"SftpNameWidth", L"SftpNameWidth96", 180, 80, 600);
    aiMonitorWidth_ = width(L"AiMonitorWidth", L"AiMonitorWidth96", 360, 260, 650);
    // New ratio key migrates the old narrow/collapsed monitor default once.
    aiMonitorRatio_ =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Layout", L"AiMonitorRatio", 500, path.c_str())), 200, 800);
    aiMonitorCollapsed_ = GetPrivateProfileIntW(L"Layout", L"AiMonitorRatio", 0, path.c_str()) &&
                          GetPrivateProfileIntW(L"Layout", L"AiMonitorCollapsed", 0, path.c_str());
    aiMonitorWrap_ = GetPrivateProfileIntW(L"Layout", L"AiMonitorWrap", 0, path.c_str()) != 0;
    powerMonitorRatio_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Layout", L"PowerMonitorRatio", 500, path.c_str())), 200, 800);
    powerMonitorCollapsed_ = GetPrivateProfileIntW(L"Layout", L"PowerMonitorCollapsed", 0, path.c_str()) != 0;
    powerMonitorWrap_ = GetPrivateProfileIntW(L"Layout", L"PowerMonitorWrap", 0, path.c_str()) != 0;
    rightPanelCollapsed_ = GetPrivateProfileIntW(
        L"Layout", L"RightPanelCollapsed", 0, path.c_str()) != 0;
}

void MainWindow::SaveUiState() const {
    std::wstring directory = DirectoryOf(DefaultCommandsPath());
    if (directory.empty()) {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        directory = DirectoryOf(modulePath);
    }
    const std::wstring path = JoinLocalPath(directory, L"settings.ini");
    auto petValue = [&](const wchar_t *key, int value) {
        WritePrivateProfileStringW(L"Pet", key, std::to_wstring(value).c_str(), path.c_str());
    };
    petValue(L"Enabled", petSettings_.enabled); petValue(L"Follow", petSettings_.follow);
    petValue(L"Opening", petSettings_.opening); petValue(L"Size", petSettings_.size);
    petValue(L"Desktop", petSettings_.desktop); petValue(L"Topmost", petSettings_.topmost);
    petValue(L"DesktopX", petSettings_.desktopX); petValue(L"DesktopY", petSettings_.desktopY);
    petValue(L"X", petSettings_.x); petValue(L"Y", petSettings_.y);
    auto logicalWidth = [&](const wchar_t *key, int pixels) {
        WritePrivateProfileStringW(L"Layout", key, std::to_wstring(MulDiv(pixels, 96, Ui::Dpi)).c_str(), path.c_str());
    };
    logicalWidth(L"RightPanelWidth96", rightPanelWidth_);
    logicalWidth(L"SftpNameWidth96", sftpNameWidth_);
    logicalWidth(L"AiMonitorWidth96", aiMonitorWidth_);
    WritePrivateProfileStringW(L"Layout", L"SavedDpi", std::to_wstring(Ui::Dpi).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"LayoutVersion", L"2", path.c_str());
    WritePrivateProfileStringW(L"Layout", L"AiMonitorRatio", std::to_wstring(aiMonitorRatio_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"AiMonitorWrap", aiMonitorWrap_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Layout", L"PowerMonitorRatio", std::to_wstring(powerMonitorRatio_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"PowerMonitorCollapsed", powerMonitorCollapsed_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Layout", L"PowerMonitorWrap", powerMonitorWrap_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Layout", L"AiMonitorWidth", std::to_wstring(aiMonitorWidth_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"AiMonitorCollapsed", aiMonitorCollapsed_ ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Layout", L"SftpNameWidth", std::to_wstring(sftpNameWidth_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"RightPanelWidth",
        std::to_wstring(rightPanelWidth_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"RightPanelCollapsed",
        rightPanelCollapsed_ ? L"1" : L"0", path.c_str());
}

void MainWindow::SetRightPanelCollapsed(bool collapsed) {
    if (((powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) == collapsed && !rightPanelAnimating_)) return;
    RECT client{}; GetClientRect(window_, &client);
    const auto before =
        GetMainLayoutMetrics(client.right, client.bottom, SidebarWidth(),
                             (powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) || rightPanelAutoCollapsed_,
                             rightPanelAnimatedWidth_, LiveMonitorPage());
    (powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) = collapsed;
    const auto after = GetMainLayoutMetrics(
        client.right, client.bottom, SidebarWidth(),
        (powerVisible_ ? powerMonitorCollapsed_ : aiVisible_ ? aiMonitorCollapsed_ : rightPanelCollapsed_) || rightPanelAutoCollapsed_, -1, LiveMonitorPage());
    SetWindowTextW(rightPanelToggleButton_, collapsed ? L"展开侧栏" : L"折叠侧栏");
    rightPanelAnimationFrom_=before.rightWidth; rightPanelAnimationTo_=after.rightWidth;
    rightPanelAnimatedWidth_=before.rightWidth; rightPanelAnimationAt_=GetTickCount64();
    rightPanelAnimating_=before.rightWidth!=after.rightWidth;
    SaveUiState();
    if(rightPanelAnimating_ && SetTimer(window_,SideAnimationTimerId,16,nullptr)) {
        ShowSftpPanel(sftpPanelVisible_); return;
    }
    KillTimer(window_, SideAnimationTimerId);
    rightPanelAnimating_=false;rightPanelAnimatedWidth_=-1;
    LayoutControls(client.right,client.bottom);
    InvalidateRect(window_,nullptr,FALSE);
}

void MainWindow::ShowSftpPanel(bool show) {
    if (show && (!connection_ || selectedMode_ != 0 || !SftpClient::IsAvailable())) show = false;
    sftpPanelVisible_ = show;
    RECT listRect{};
    GetClientRect(sftpList_, &listRect);
    UiLayoutBatch batch;
    QueueSidebarVisibility(batch, listRect.right - listRect.left);
    batch.Commit();
    InvalidateRect(commandHeader_, nullptr, FALSE);
    InvalidateRect(sftpTabButton_, nullptr, FALSE);
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::QueueSidebarVisibility(UiLayoutBatch& batch, int contentWidth) {
    const bool expanded = !LiveMonitorPage() && !SidebarCollapsed();
    const bool show = sftpPanelVisible_;
    batch.Show(rightPanelToggleButton_, SW_SHOW);
    batch.Show(commandHeader_, expanded ? SW_SHOW : SW_HIDE);
    batch.Show(sftpTabButton_, expanded ? SW_SHOW : SW_HIDE);
    for (HWND control : {commandList_, importButton_, exportButton_, addCommandButton_, deleteCommandButton_, saveCommandsButton_})
        batch.Show(control, expanded && !show ? SW_SHOW : SW_HIDE);
    for (HWND control : {sftpPath_, sftpNameHeader_, sftpSizeHeader_, sftpList_,
             sftpTransferToggleButton_, sftpClearTransfersButton_, sftpUpButton_, sftpRefreshButton_,
             sftpUploadButton_, sftpDownloadButton_})
        batch.Show(control, expanded && show ? SW_SHOW : SW_HIDE);
    const bool modifiedColumn = contentWidth - Ui::OverlayScrollLaneWidth >= Ui::Scale(300);
    batch.Show(sftpModifiedHeader_, expanded && show && modifiedColumn ? SW_SHOW : SW_HIDE);
    batch.Show(sftpTransferList_, expanded && show && sftpTransferExpanded_ ? SW_SHOW : SW_HIDE);
}

std::vector<size_t> MainWindow::SelectedSftpIndices() const {
    std::vector<size_t> result;
    if (!sftpList_) return result;
    const LRESULT count = SendMessageW(sftpList_, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return result;
    std::vector<int> selected(static_cast<size_t>(count));
    const LRESULT copied = SendMessageW(sftpList_, LB_GETSELITEMS,
        static_cast<WPARAM>(selected.size()), reinterpret_cast<LPARAM>(selected.data()));
    if (copied <= 0) return result;
    for (int index = 0; index < copied; ++index) {
        if (selected[static_cast<size_t>(index)] >= 0 &&
            static_cast<size_t>(selected[static_cast<size_t>(index)]) < sftpEntries_.size())
            result.push_back(static_cast<size_t>(selected[static_cast<size_t>(index)]));
    }
    return result;
}

void MainWindow::SetSftpBusy(bool busy) {
    sftpBusy_ = busy;
    const bool available = connection_ && selectedMode_ == 0 && !busy;
    const std::vector<size_t> selected = SelectedSftpIndices();
    const bool fileSelected = std::any_of(selected.begin(), selected.end(), [this](size_t index) {
        return index < sftpEntries_.size() && !sftpEntries_[index].directory &&
            sftpEntries_[index].pathSafe;
    });
    EnableWindow(sftpUpButton_, available && sftpDirectory_ != L"/" && sftpDirectory_ != L".");
    EnableWindow(sftpRefreshButton_, available);
    EnableWindow(sftpUploadButton_, available);
    EnableWindow(sftpDownloadButton_, available && fileSelected);
    EnableWindow(sftpList_, available);
    for (HWND button : {sftpUpButton_, sftpRefreshButton_, sftpUploadButton_, sftpDownloadButton_})
        if (button) InvalidateRect(button, nullptr, TRUE);
}

void MainWindow::RefreshSftp(const std::wstring& directory) {
    if (sftpBusy_ || !activeSession_ || !connection_ || selectedMode_ != 0) return;
    if (sftpThread_.joinable()) sftpThread_.join();
    const std::wstring requested = directory.empty() ? sftpDirectory_ : directory;
    if (!directory.empty() && directory != activeSession_->pendingSftpDirectory && directory != activeSession_->sftpDirectory) {
        activeSession_->sftpFollowTerminal = false;
    }
    activeSession_->pendingSftpDirectory.clear();
    const std::uint64_t sessionId = activeSession_->id;
    const std::wstring host = activeSession_->host;
    const std::wstring username = activeSession_->username;
    const std::wstring password = activeSession_->password;
    const std::uint16_t port = activeSession_->port;
    const HWND target = window_;
    sftpOperationCancel_.store(false);
    sftpOperationSessionId_ = sessionId;
    SetSftpBusy(true);
    SetWindowTextW(sftpPath_, L"正在读取…");
    auto work = [target, sessionId, host, port, username, password, requested,
        cancel = &sftpOperationCancel_] {
        auto* result = new (std::nothrow) SftpMessage();
        if (!result) return;
        result->sessionId = sessionId;
        result->kind = SftpMessageKind::List;
        try {
            SftpClient client(host, port, username, password);
            result->success = client.List(requested, result->listing, result->error, cancel);
        } catch (...) {
            result->success = false;
            result->error = L"SFTP 目录读取发生内部错误。";
        }
        if (!PostMessageW(target, MessageSftp, 0, reinterpret_cast<LPARAM>(result))) delete result;
    };
    try {
        sftpThread_ = std::thread(std::move(work));
    } catch (...) {
        sftpOperationSessionId_ = 0;
        SetSftpBusy(false);
        SetWindowTextW(sftpPath_, sftpDirectory_.c_str());
        AppendStatus(L"无法启动 SFTP 目录读取任务。", true);
    }
}

void MainWindow::RefreshSftpList() {
    std::vector<std::wstring> selectedNames;
    for (size_t index : SelectedSftpIndices()) selectedNames.push_back(sftpEntries_[index].name);
    SortSftpEntries(sftpEntries_, sftpSortColumn_, sftpSortAscending_);
    if (activeSession_) activeSession_->sftpEntries = sftpEntries_;
    SendMessageW(sftpList_, LB_RESETCONTENT, 0, 0);
    for (size_t index = 0; index < sftpEntries_.size(); ++index) {
        const SftpEntry& entry = sftpEntries_[index];
        SendMessageW(sftpList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name.c_str()));
        if (std::find(selectedNames.begin(), selectedNames.end(), entry.name) != selectedNames.end())
            SendMessageW(sftpList_, LB_SETSEL, TRUE, static_cast<LPARAM>(index));
    }
    SetWindowTextW(sftpPath_, sftpDirectory_.c_str());
    InvalidateRect(sftpPath_, nullptr, TRUE);
    InvalidateRect(sftpList_, nullptr, TRUE);
    SetSftpBusy(sftpBusy_);
}

void MainWindow::SetSftpSort(SftpSortColumn column) {
    if (sftpSortColumn_ == column) sftpSortAscending_ = !sftpSortAscending_;
    else {
        sftpSortColumn_ = column;
        sftpSortAscending_ = true;
    }
    RefreshSftpList();
    for (HWND header : {sftpNameHeader_, sftpSizeHeader_, sftpModifiedHeader_})
        InvalidateRect(header, nullptr, TRUE);
}

void MainWindow::EditSftpPath() {
    if (!activeSession_ || selectedMode_ != 0 || sftpBusy_) return;
    const auto sessionId = activeSession_->id;
    std::wstring path;
    if (PromptSftpValue(L"打开 SFTP 路径", L"远端目录（可粘贴 pwd 输出）", sftpDirectory_, SftpInputPurpose::Path, path)) {
        if (!activeSession_ || activeSession_->id != sessionId) return;
        if (path == L"~") path = activeSession_->sftpHomeDirectory;
        else if (path.rfind(L"~/", 0) == 0) path = JoinSftpRemotePath(activeSession_->sftpHomeDirectory, path.substr(2));
        if (path.empty() || path.front() != L'/') { AppendStatus(L"请输入绝对路径或 ~/ 路径", true); return; }
        activeSession_->sftpFollowTerminal = false;
        activeSession_->pendingSftpDirectory.clear();
        RefreshSftp(path);
    }
}
void MainWindow::QueryTerminalDirectory(bool installHook) {
    if (!activeSession_ || selectedMode_ != 0 || runningCommandIndex_ >= 0) return;
    const auto sessionId = activeSession_->id;
    const wchar_t* prompt = installHook ? L"请确认终端已回到空闲 Bash/Zsh 提示符。将为当前 Shell 安装目录通知，不修改远端配置文件。继续？" : L"请确认终端已回到空闲 Shell 提示符。将在此终端执行 pwd 并更新 SFTP 路径。继续？";
    if (MessageBoxW(window_, prompt, L"获取终端目录", MB_OKCANCEL | MB_ICONQUESTION) != IDOK || !activeSession_ || activeSession_->id != sessionId) return;
    // Runs in the original interactive shell: a separate SSH process would report the wrong directory.
    std::wstring command = LR"shell(__serialctl_cwd() { printf '\033]7;file://localhost%s\007' "$(pwd -P | sed 's/%/%25/g')"; }; )shell";
    if (installHook) command += LR"shell(if [ -n "$BASH_VERSION" ]; then case "$(declare -p PROMPT_COMMAND 2>/dev/null)" in 'declare -a '*) [[ " ${PROMPT_COMMAND[*]} " == *' __serialctl_cwd '* ]] || PROMPT_COMMAND+=(__serialctl_cwd);; *) case ";${PROMPT_COMMAND-};" in *';__serialctl_cwd;'*) ;; *) PROMPT_COMMAND="${PROMPT_COMMAND:+$PROMPT_COMMAND; }__serialctl_cwd";; esac;; esac; elif [ -n "$ZSH_VERSION" ]; then (( ${precmd_functions[(Ie)__serialctl_cwd]} )) || precmd_functions+=(__serialctl_cwd); else printf 'Shell unsupported: use manual path\n'; fi; )shell";
    command += L"__serialctl_cwd\r";
    activeSession_->sftpFollowTerminal = true;
    const auto bytes = WideToMultiByte(command, CP_UTF8);
    SendBytesToActive(Bytes(bytes.begin(), bytes.end()), false);
}
void MainWindow::SftpColumnWidths(int width, int& name, int& size, int& modified) const {
    modified = width >= Ui::Scale(300) ? Ui::Scale(104) : 0;
    const int remaining = std::max(Ui::Scale(128), width - modified);
    name = std::clamp(sftpNameWidth_, Ui::Scale(80), remaining - Ui::Scale(48));
    size = remaining - name;
}
LRESULT CALLBACK MainWindow::SftpHeaderSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    RECT rect{}; GetClientRect(window, &rect);
    const bool edge = GET_X_LPARAM(lParam) >= rect.right - Ui::Space;
    if (message == WM_LBUTTONDOWN && edge) {
        self->sftpColumnDragging_ = GetDlgCtrlID(window) == IdSftpNameHeader ? 1 : 2;
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ClientToScreen(window, &p); self->sftpColumnDragX_ = p.x;
        RECT name{}; GetClientRect(self->sftpNameHeader_, &name); self->sftpColumnDragWidth_ = name.right;
        SetCapture(window); return 0;
    }
    if (message == WM_MOUSEMOVE && self->sftpColumnDragging_) {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ClientToScreen(window, &p);
        const int nextWidth =
            std::max(Ui::Scale(80), self->sftpColumnDragWidth_ + (static_cast<int>(p.x) - self->sftpColumnDragX_) *
                                                                     (self->sftpColumnDragging_ == 1 ? 1 : -1));
        if (nextWidth == self->sftpNameWidth_)
            return 0;
        self->sftpNameWidth_ = nextWidth;
        RECT list{}, header{}; GetWindowRect(self->sftpList_, &list);
        GetWindowRect(self->sftpNameHeader_, &header);
        MapWindowPoints(HWND_DESKTOP, self->window_, reinterpret_cast<POINT*>(&list), 2);
        MapWindowPoints(HWND_DESKTOP, self->window_, reinterpret_cast<POINT*>(&header), 2);
        int name, size, modified; self->SftpColumnWidths(list.right - list.left - Ui::OverlayScrollLaneWidth, name, size, modified);
        UiLayoutBatch batch;
        batch.Move(self->sftpNameHeader_, list.left, header.top, name, Ui::CompactHeight);
        batch.Move(self->sftpSizeHeader_, list.left + name, header.top, size, Ui::CompactHeight);
        batch.Move(self->sftpModifiedHeader_, list.left + name + size, header.top, modified, Ui::CompactHeight);
        batch.Commit();
        RECT redraw{list.left, header.top, list.right, header.bottom};
        InvalidateRect(self->window_, &redraw, FALSE);
        for (HWND headerControl : {self->sftpNameHeader_, self->sftpSizeHeader_, self->sftpModifiedHeader_})
            InvalidateRect(headerControl, nullptr, FALSE);
        InvalidateRect(self->sftpList_, nullptr, FALSE); return 0;
    }
    if ((message == WM_LBUTTONUP || message == WM_CAPTURECHANGED) && self->sftpColumnDragging_) {
        self->sftpColumnDragging_ = 0; if (GetCapture() == window) ReleaseCapture(); self->SaveUiState(); return 0;
    }
    if (message == WM_SETCURSOR) { POINT p{}; GetCursorPos(&p); ScreenToClient(window, &p); if (p.x >= rect.right - Ui::Space) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; } }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SftpHeaderSubclassProc, id);
    return DefSubclassProc(window, message, wParam, lParam);
}

void MainWindow::NavigateSftp(size_t index) {
    if (sftpBusy_ || index >= sftpEntries_.size()) return;
    const SftpEntry& entry = sftpEntries_[index];
    if ((entry.directory || entry.symlink) && !entry.pathSafe) {
        AppendStatus(L"该远端名称包含无法安全写入 SFTP 命令的字符。", true);
    } else if (entry.directory || entry.symlink)
        RefreshSftp(JoinSftpRemotePath(sftpDirectory_, entry.name));
    else DownloadSftp();
}

void MainWindow::NavigateSftpBreadcrumb(POINT point) {
    if (sftpBusy_) return;
    for (const SftpBreadcrumbHit& hit : sftpBreadcrumbHits_) {
        if (PointInRect(hit.rect, point) && hit.path != sftpDirectory_) {
            RefreshSftp(hit.path);
            return;
        }
    }
}

void MainWindow::ShowSftpContextMenu(POINT screenPoint) {
    if (!sftpList_) return;
    POINT clientPoint = screenPoint;
    ScreenToClient(sftpList_, &clientPoint);
    const DWORD hit = static_cast<DWORD>(SendMessageW(sftpList_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y)));
    if (!HIWORD(hit) && LOWORD(hit) < sftpEntries_.size()) {
        const int index = LOWORD(hit);
        if (!SendMessageW(sftpList_, LB_GETSEL, index, 0)) {
            SendMessageW(sftpList_, LB_SETSEL, FALSE, -1);
            SendMessageW(sftpList_, LB_SETSEL, TRUE, index);
        }
        SendMessageW(sftpList_, LB_SETCARETINDEX, index, FALSE);
        SetSftpBusy(sftpBusy_);
    }
    const std::vector<size_t> selected = SelectedSftpIndices();
    const bool available = connection_ && selectedMode_ == 0 && !sftpBusy_;
    const bool one = selected.size() == 1;
    const bool allPathSafe = !selected.empty() &&
        std::all_of(selected.begin(), selected.end(), [this](size_t index) {
            return sftpEntries_[index].pathSafe;
        });
    const bool allModeSafe = !selected.empty() &&
        std::all_of(selected.begin(), selected.end(), [this](size_t index) {
            return sftpEntries_[index].pathSafe && !sftpEntries_[index].symlink;
        });
    const bool anyFiles = std::any_of(selected.begin(), selected.end(), [this](size_t index) {
        return !sftpEntries_[index].directory && sftpEntries_[index].pathSafe;
    });
    const bool oneDirectory = one &&
        sftpEntries_[selected.front()].pathSafe &&
        (sftpEntries_[selected.front()].directory || sftpEntries_[selected.front()].symlink);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HBRUSH menuBrush = CreateSolidBrush(Colors(darkMode_).panel);
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = menuBrush;
    SetMenuInfo(menu, &info);
    static const wchar_t downloadLabel[] = L"下载";
    static const wchar_t uploadLabel[] = L"上传文件…";
    static const wchar_t mkdirLabel[] = L"新建文件夹…";
    static const wchar_t renameLabel[] = L"重命名…";
    static const wchar_t deleteLabel[] = L"删除";
    static const wchar_t refreshLabel[] = L"刷新";
    static const wchar_t copyLabel[] = L"复制远端路径";
    static const wchar_t enterLabel[] = L"在终端进入此目录";
    static const wchar_t modeLabel[] = L"修改权限…";
    static const wchar_t pathLabel[] = L"输入路径（可粘贴 pwd）…";
    static const wchar_t followLabel[] = L"跟随终端目录";
    static const wchar_t pwdLabel[] = L"从当前终端获取目录…";
    static const wchar_t hookLabel[] = L"启用当前 Shell 目录通知…";
    AppendMenuW(menu, MF_OWNERDRAW | (!available ? MF_GRAYED : 0), IdMenuSftpPath, pathLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!activeSession_ ? MF_GRAYED : 0) | (activeSession_ && activeSession_->sftpFollowTerminal ? MF_CHECKED : 0), IdMenuSftpFollow, followLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || runningCommandIndex_ >= 0 ? MF_GRAYED : 0), IdMenuSftpPwd, pwdLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || runningCommandIndex_ >= 0 ? MF_GRAYED : 0), IdMenuSftpHook, hookLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || !anyFiles ? MF_GRAYED : 0),
        IdMenuSftpDownload, downloadLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available ? MF_GRAYED : 0), IdMenuSftpUpload, uploadLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (!available ? MF_GRAYED : 0),
        IdMenuSftpCreateDirectory, mkdirLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || !one || !allPathSafe ? MF_GRAYED : 0),
        IdMenuSftpRename, renameLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || !allPathSafe ? MF_GRAYED : 0),
        IdMenuSftpDelete, deleteLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || !allModeSafe ? MF_GRAYED : 0),
        IdMenuSftpChangeMode, modeLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (selected.empty() ? MF_GRAYED : 0),
        IdMenuSftpCopyPath, copyLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available || !oneDirectory ? MF_GRAYED : 0),
        IdMenuSftpEnterDirectory, enterLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!available ? MF_GRAYED : 0), IdMenuSftpRefresh, refreshLabel);

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    PostMessageW(window_, WM_NULL, 0, 0);
    switch (command) {
    case IdMenuSftpPath: EditSftpPath(); break;
    case IdMenuSftpFollow:
        if (activeSession_) { activeSession_->sftpFollowTerminal = !activeSession_->sftpFollowTerminal; if (activeSession_->sftpFollowTerminal) { activeSession_->pendingSftpDirectory.clear(); SyncSftpDirectoryFromTerminal(*activeSession_); } } break;
    case IdMenuSftpPwd: QueryTerminalDirectory(false); break;
    case IdMenuSftpHook: QueryTerminalDirectory(true); break;
    case IdMenuSftpDownload: DownloadSftp(); break;
    case IdMenuSftpUpload: UploadSftp(); break;
    case IdMenuSftpCreateDirectory: CreateSftpDirectory(); break;
    case IdMenuSftpRename: RenameSelectedSftpEntry(); break;
    case IdMenuSftpDelete: DeleteSelectedSftpEntries(); break;
    case IdMenuSftpChangeMode: ChangeSelectedSftpMode(); break;
    case IdMenuSftpCopyPath: CopySelectedSftpPaths(); break;
    case IdMenuSftpEnterDirectory: EnterSelectedSftpDirectoryInTerminal(); break;
    case IdMenuSftpRefresh: RefreshSftp(sftpDirectory_); break;
    default: break;
    }
}

void MainWindow::ShowSftpTransferContextMenu(POINT screenPoint) {
    POINT clientPoint = screenPoint;
    ScreenToClient(sftpTransferList_, &clientPoint);
    const DWORD hit = static_cast<DWORD>(SendMessageW(sftpTransferList_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(clientPoint.x, clientPoint.y)));
    if (!HIWORD(hit) && LOWORD(hit) < sftpTransfers_.size())
        SendMessageW(sftpTransferList_, LB_SETCURSEL, LOWORD(hit), 0);
    const LRESULT selected = SendMessageW(sftpTransferList_, LB_GETCURSEL, 0, 0);
    const bool valid = selected != LB_ERR && static_cast<size_t>(selected) < sftpTransfers_.size();
    const SftpTransferState state = valid ? sftpTransfers_[static_cast<size_t>(selected)].state :
        SftpTransferState::Completed;
    const bool anyFinished = std::any_of(sftpTransfers_.begin(), sftpTransfers_.end(),
        [](const SftpTransferItem& item) {
            return item.state == SftpTransferState::Completed ||
                item.state == SftpTransferState::Failed ||
                item.state == SftpTransferState::Canceled;
        });
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HBRUSH menuBrush = CreateSolidBrush(Colors(darkMode_).panel);
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = menuBrush;
    SetMenuInfo(menu, &info);
    static const wchar_t cancelLabel[] = L"取消传输";
    static const wchar_t retryLabel[] = L"重试";
    static const wchar_t clearLabel[] = L"清理已结束记录";
    AppendMenuW(menu, MF_OWNERDRAW | (!valid || (state != SftpTransferState::Queued &&
        state != SftpTransferState::Running) ? MF_GRAYED : 0), IdMenuTransferCancel, cancelLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!valid || (state != SftpTransferState::Failed &&
        state != SftpTransferState::Canceled) ? MF_GRAYED : 0), IdMenuTransferRetry, retryLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (!anyFinished ? MF_GRAYED : 0),
        IdMenuTransferClearFinished, clearLabel);
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == IdMenuTransferCancel && valid)
        CancelSftpTransfer(sftpTransfers_[static_cast<size_t>(selected)].id);
    else if (command == IdMenuTransferRetry && valid)
        RetrySftpTransfer(sftpTransfers_[static_cast<size_t>(selected)].id);
    else if (command == IdMenuTransferClearFinished)
        ClearFinishedSftpTransfers();
}

void MainWindow::HandleSftpDrop(HDROP drop) {
    if (!drop) return;
    if (sftpBusy_ || !activeSession_ || !connection_ || selectedMode_ != 0) {
        DragFinish(drop);
        return;
    }
    POINT point{};
    DragQueryPoint(drop, &point);
    std::wstring remoteDirectory = sftpDirectory_;
    const DWORD hit = static_cast<DWORD>(SendMessageW(sftpList_, LB_ITEMFROMPOINT, 0,
        MAKELPARAM(point.x, point.y)));
    if (!HIWORD(hit) && LOWORD(hit) < sftpEntries_.size()) {
        const SftpEntry& entry = sftpEntries_[LOWORD(hit)];
        if (entry.directory && entry.pathSafe)
            remoteDirectory = JoinSftpRemotePath(sftpDirectory_, entry.name);
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    unsigned int queued = 0;
    unsigned int rejected = 0;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::vector<wchar_t> path(static_cast<size_t>(length) + 1);
        DragQueryFileW(drop, index, path.data(), static_cast<UINT>(path.size()));
        const DWORD attributes = GetFileAttributesW(path.data());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ++rejected;
            continue;
        }
        if (StartSftpUpload(path.data(), remoteDirectory)) ++queued;
    }
    DragFinish(drop);
    if (queued > 0) AppendStatus(L"已加入 " + std::to_wstring(queued) + L" 个上传任务", false);
    if (rejected > 0) AppendStatus(L"暂不支持直接上传文件夹，已跳过 " +
        std::to_wstring(rejected) + L" 项", true);
}

void MainWindow::UploadSftp() {
    if (sftpBusy_ || !activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto destination = sftpDirectory_;
    std::vector<wchar_t> paths(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = paths.data();
    dialog.nMaxFile = static_cast<DWORD>(paths.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
        OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&dialog) || !activeSession_ || activeSession_->id != sessionId) return;
    const std::wstring first = paths.data();
    const wchar_t* next = paths.data() + first.size() + 1;
    if (*next == L'\0') {
        StartSftpUpload(first, destination);
        return;
    }
    while (*next) {
        const std::wstring name = next;
        StartSftpUpload(JoinLocalPath(first, name), destination);
        next += name.size() + 1;
    }
}

void MainWindow::DownloadSftp() {
    if (sftpBusy_ || !activeSession_) return;
    std::vector<size_t> selected = SelectedSftpIndices();
    const auto entries = sftpEntries_;
    const size_t beforePathValidation = selected.size();
    selected.erase(std::remove_if(selected.begin(), selected.end(), [&entries](size_t index) {
        return entries[index].directory || !entries[index].pathSafe;
    }), selected.end());
    if (selected.size() != beforePathValidation)
        AppendStatus(L"已跳过不能安全下载的远端项目。", true);
    if (selected.empty() || !activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto sourceDirectory = sftpDirectory_;
    if (selected.size() == 1) {
        const SftpEntry entry = entries[selected.front()];
        std::vector<wchar_t> path(std::max<size_t>(MAX_PATH, entry.name.size() + 1), L'\0');
        std::copy(entry.name.begin(), entry.name.end(), path.begin());
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog) && activeSession_ && activeSession_->id == sessionId) {
            const bool exists = GetFileAttributesW(path.data()) != INVALID_FILE_ATTRIBUTES;
            StartSftpDownload(JoinSftpRemotePath(sourceDirectory, entry.name),
                path.data(), entry.size, exists);
        }
        return;
    }

    const size_t selectedBeforeValidation = selected.size();
    selected.erase(std::remove_if(selected.begin(), selected.end(), [&entries](size_t index) {
        return !IsSafeAutomaticLocalFileName(entries[index].name);
    }), selected.end());
    if (selected.empty()) {
        AppendStatus(L"所选远端文件名均不能安全保存到 Windows，请逐个下载并指定本地文件名。", true);
        return;
    }
    if (selected.size() != selectedBeforeValidation) {
        AppendStatus(L"已跳过 " + std::to_wstring(selectedBeforeValidation - selected.size()) +
            L" 个不适合自动保存到 Windows 的远端文件名。", true);
    }

    BROWSEINFOW browse{};
    browse.hwndOwner = window_;
    browse.lpszTitle = L"选择保存多个下载文件的文件夹";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return;
    wchar_t directory[MAX_PATH]{};
    const bool resolved = SHGetPathFromIDListW(item, directory) != FALSE;
    CoTaskMemFree(item);
    if (!resolved || !activeSession_ || activeSession_->id != sessionId) return;
    std::vector<size_t> uniqueSelected;
    std::vector<std::wstring> localPaths;
    std::vector<bool> overwriteAuthorized;
    localPaths.reserve(selected.size());
    uniqueSelected.reserve(selected.size());
    overwriteAuthorized.reserve(selected.size());
    size_t duplicateTargets = 0;
    for (size_t index : selected) {
        const std::wstring localPath = JoinLocalPath(directory, entries[index].name);
        const bool duplicate = std::any_of(localPaths.begin(), localPaths.end(),
            [&localPath](const std::wstring& existing) {
                return ExistingLocalPathsReferToSameFile(existing, localPath);
            });
        if (duplicate) {
            ++duplicateTargets;
            continue;
        }
        uniqueSelected.push_back(index);
        localPaths.push_back(localPath);
        const bool exists = GetFileAttributesW(localPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        overwriteAuthorized.push_back(exists);
    }
    selected = std::move(uniqueSelected);
    if (duplicateTargets > 0)
        AppendStatus(L"已跳过 " + std::to_wstring(duplicateTargets) +
            L" 个在 Windows 中会映射到同一路径的文件。", true);
    if (selected.empty()) return;
    for (size_t position = 0; position < selected.size(); ++position) {
        const size_t index = selected[position];
        const SftpEntry& entry = entries[index];
        StartSftpDownload(JoinSftpRemotePath(sourceDirectory, entry.name),
            localPaths[position], entry.size, overwriteAuthorized[position]);
    }
}

bool MainWindow::StartSftpUpload(
    const std::wstring& localPath, const std::wstring& remoteDirectory) {
    if (!activeSession_ || localPath.empty()) return false;
    SftpTransferItem transfer;
    transfer.id = nextSftpTransferId_++;
    transfer.sessionId = activeSession_->id;
    transfer.direction = SftpTransferDirection::Upload;
    transfer.name = LocalFileNameOf(localPath);
    transfer.localPath = localPath;
    transfer.remotePath = remoteDirectory.empty() ? sftpDirectory_ : remoteDirectory;
    if (transfer.remotePath == sftpDirectory_) {
        const auto existing = std::find_if(sftpEntries_.begin(), sftpEntries_.end(),
            [&transfer](const SftpEntry& entry) { return entry.name == transfer.name; });
        if (existing != sftpEntries_.end()) {
            if (existing->directory) {
                AppendStatus(L"远端已有同名文件夹，无法上传该文件。", true);
                return false;
            }
            transfer.replaceExisting = true;
        }
    }
    transfer.total = LocalFileSize(localPath);
    sftpTransfers_.push_back(std::move(transfer));
    sftpTransferExpanded_ = true;
    RefreshSftpTransferList();
    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
    StartNextSftpTransfer();
    return true;
}

void MainWindow::StartSftpDownload(const std::wstring& remotePath,
    const std::wstring& localPath, std::uint64_t expectedSize, bool replaceExisting) {
    if (!activeSession_ || remotePath.empty() || localPath.empty()) return;
    const bool alreadyQueued = std::any_of(sftpTransfers_.begin(), sftpTransfers_.end(),
        [&localPath](const SftpTransferItem& item) {
            return item.direction == SftpTransferDirection::Download &&
                (item.state == SftpTransferState::Queued ||
                 item.state == SftpTransferState::Running) &&
                ExistingLocalPathsReferToSameFile(item.localPath, localPath);
        });
    if (alreadyQueued) {
        AppendStatus(L"已有任务正在写入同一本地路径，已跳过重复下载。", true);
        return;
    }
    SftpTransferItem transfer;
    transfer.id = nextSftpTransferId_++;
    transfer.sessionId = activeSession_->id;
    transfer.direction = SftpTransferDirection::Download;
    transfer.name = BaseNameSftpRemotePath(remotePath);
    transfer.localPath = localPath;
    transfer.remotePath = remotePath;
    transfer.total = expectedSize;
    transfer.replaceExisting = replaceExisting;
    sftpTransfers_.push_back(std::move(transfer));
    sftpTransferExpanded_ = true;
    RefreshSftpTransferList();
    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
    StartNextSftpTransfer();
}

void MainWindow::StartNextSftpTransfer() {
    if (activeSftpTransferId_ != 0) return;
    if (sftpTransferThread_.joinable()) sftpTransferThread_.join();
    for (;;) {
        auto queued = std::find_if(sftpTransfers_.begin(), sftpTransfers_.end(),
            [](const SftpTransferItem& item) { return item.state == SftpTransferState::Queued; });
        if (queued == sftpTransfers_.end()) {
            RefreshSftpTransferList();
            if (sftpRefreshSessionId_ != 0) {
                const std::uint64_t refreshSessionId = sftpRefreshSessionId_;
                const std::wstring refreshDirectory = sftpRefreshDirectory_;
                sftpRefreshSessionId_ = 0;
                sftpRefreshDirectory_.clear();
                if (!sftpBusy_ && activeSession_ && activeSession_->id == refreshSessionId &&
                    sftpDirectory_ == refreshDirectory)
                    RefreshSftp(refreshDirectory);
            }
            return;
        }
        SessionState* session = FindSession(queued->sessionId);
        if (!session) {
            queued->state = SftpTransferState::Canceled;
            queued->error = L"连接已断开";
            continue;
        }

        if (queued->replaceExisting) {
            const bool upload = queued->direction == SftpTransferDirection::Upload;
            const std::wstring target = upload ?
                JoinSftpRemotePath(queued->remotePath, queued->name) : queued->localPath;
            const std::wstring prompt = (upload ?
                L"远端文件已存在，是否安全替换？\n\n" :
                L"本地文件已存在，是否覆盖？\n\n") + target;
            if (MessageBoxW(window_, prompt.c_str(),
                    upload ? L"确认替换" : L"确认覆盖",
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
                queued->state = SftpTransferState::Canceled;
                queued->error = L"用户取消覆盖";
                continue;
            }
        }

        queued->state = SftpTransferState::Running;
        queued->error.clear();
        queued->percent = queued->total == 0 ? -1 : 0;
        activeSftpTransferId_ = queued->id;
        sftpTransferCancel_.store(false);
        const SftpTransferItem task = *queued;
        const std::wstring host = session->host;
        const std::uint16_t port = session->port;
        const std::wstring username = session->username;
        const std::wstring password = session->password;
        const HWND target = window_;
        RefreshSftpTransferList();
        auto work = [this, target, task, host, port, username, password] {
            auto* result = new (std::nothrow) SftpMessage();
            if (!result) return;
            result->sessionId = task.sessionId;
            result->transferId = task.id;
            result->kind = task.direction == SftpTransferDirection::Upload ?
                SftpMessageKind::Upload : SftpMessageKind::Download;
            try {
                SftpClient client(host, port, username, password);
                const auto progress = [target, id = task.id](const SftpTransferProgress& value) {
                    auto* update = new (std::nothrow) SftpProgressMessage();
                    if (!update) return;
                    update->transferId = id;
                    update->progress = value;
                    if (!PostMessageW(target, MessageSftpProgress, 0,
                            reinterpret_cast<LPARAM>(update))) delete update;
                };
                if (task.direction == SftpTransferDirection::Upload)
                    result->success = client.Upload(task.localPath, task.remotePath,
                        task.replaceExisting, result->error, progress, &sftpTransferCancel_);
                else
                    result->success = client.Download(task.remotePath, task.localPath, task.total,
                        task.replaceExisting, result->error, progress, &sftpTransferCancel_);
                result->canceled = sftpTransferCancel_.load();
            } catch (...) {
                result->success = false;
                result->error = L"SFTP 传输发生内部错误。";
            }
            if (!PostMessageW(target, MessageSftp, 0,
                    reinterpret_cast<LPARAM>(result))) delete result;
        };
        try {
            sftpTransferThread_ = std::thread(std::move(work));
        } catch (...) {
            queued->state = SftpTransferState::Failed;
            queued->error = L"无法启动传输任务";
            activeSftpTransferId_ = 0;
            RefreshSftpTransferList();
            continue;
        }
        return;
    }
}

void MainWindow::CancelSftpTransfer(std::uint64_t transferId) {
    auto found = std::find_if(sftpTransfers_.begin(), sftpTransfers_.end(),
        [transferId](const SftpTransferItem& item) { return item.id == transferId; });
    if (found == sftpTransfers_.end()) return;
    if (found->state == SftpTransferState::Queued) {
        found->state = SftpTransferState::Canceled;
        found->error = L"传输已取消";
        RefreshSftpTransferList();
        StartNextSftpTransfer();
    } else if (found->state == SftpTransferState::Running) {
        sftpTransferCancel_.store(true);
        found->error = L"正在取消…";
        InvalidateRect(sftpTransferList_, nullptr, TRUE);
    }
}

void MainWindow::RetrySftpTransfer(std::uint64_t transferId) {
    auto found = std::find_if(sftpTransfers_.begin(), sftpTransfers_.end(),
        [transferId](const SftpTransferItem& item) { return item.id == transferId; });
    if (found == sftpTransfers_.end() || (found->state != SftpTransferState::Failed &&
        found->state != SftpTransferState::Canceled)) return;
    if (!FindSession(found->sessionId)) {
        found->error = L"原连接已断开，无法重试";
        found->state = SftpTransferState::Failed;
    } else {
        found->state = SftpTransferState::Queued;
        found->error.clear();
        found->transferred = 0;
        found->bytesPerSecond = 0;
        found->remainingSeconds = 0;
        found->percent = found->total == 0 ? -1 : 0;
    }
    RefreshSftpTransferList();
    StartNextSftpTransfer();
}

void MainWindow::ClearFinishedSftpTransfers() {
    sftpTransfers_.erase(std::remove_if(sftpTransfers_.begin(), sftpTransfers_.end(),
        [](const SftpTransferItem& item) {
            return item.state == SftpTransferState::Completed ||
                item.state == SftpTransferState::Failed ||
                item.state == SftpTransferState::Canceled;
        }), sftpTransfers_.end());
    RefreshSftpTransferList();
}

void MainWindow::RefreshSftpTransferList() {
    if (!sftpTransferList_) return;
    SendMessageW(sftpTransferList_, LB_RESETCONTENT, 0, 0);
    int queued = 0;
    int running = 0;
    int finished = 0;
    for (const SftpTransferItem& item : sftpTransfers_) {
        SendMessageW(sftpTransferList_, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(item.name.c_str()));
        if (item.state == SftpTransferState::Queued) ++queued;
        else if (item.state == SftpTransferState::Running) ++running;
        else ++finished;
    }
    std::wstring title = L"传输";
    if (running > 0) title += L" · " + std::to_wstring(running) + L" 进行中";
    if (queued > 0) title += L" · " + std::to_wstring(queued) + L" 等待";
    if (running == 0 && queued == 0 && !sftpTransfers_.empty())
        title += L" · " + std::to_wstring(sftpTransfers_.size()) + L" 条记录";
    SetWindowTextW(sftpTransferToggleButton_, title.c_str());
    EnableWindow(sftpClearTransfersButton_, finished > 0);
    InvalidateRect(sftpTransferToggleButton_, nullptr, TRUE);
    InvalidateRect(sftpClearTransfersButton_, nullptr, TRUE);
    InvalidateRect(sftpTransferList_, nullptr, TRUE);
}

void MainWindow::HandleSftpProgress(LPARAM value) {
    std::unique_ptr<SftpProgressMessage> update(reinterpret_cast<SftpProgressMessage*>(value));
    if (!update) return;
    auto found = std::find_if(sftpTransfers_.begin(), sftpTransfers_.end(),
        [id = update->transferId](const SftpTransferItem& item) { return item.id == id; });
    if (found == sftpTransfers_.end() || found->state != SftpTransferState::Running) return;
    found->transferred = update->progress.transferred;
    found->total = update->progress.total;
    found->bytesPerSecond = update->progress.bytesPerSecond;
    found->remainingSeconds = update->progress.remainingSeconds;
    found->percent = update->progress.percent;
    InvalidateRect(sftpTransferList_, nullptr, FALSE);
}

void MainWindow::CreateSftpDirectory() {
    if (!activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto sourceDirectory = sftpDirectory_;
    if (sftpBusy_) return;
    std::wstring name;
    if (PromptSftpValue(L"新建文件夹", L"文件夹名称", L"",
            SftpInputPurpose::Name, name))
        if (activeSession_ && activeSession_->id == sessionId) StartSftpMutation(SftpMutationKind::CreateDirectory,
            {JoinSftpRemotePath(sourceDirectory, name)});
}

void MainWindow::RenameSelectedSftpEntry() {
    if (!activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto sourceDirectory = sftpDirectory_;
    const std::vector<size_t> selected = SelectedSftpIndices();
    if (sftpBusy_ || selected.size() != 1 || !sftpEntries_[selected.front()].pathSafe) return;
    const SftpEntry entry = sftpEntries_[selected.front()];
    std::wstring name;
    if (PromptSftpValue(L"重命名", L"新名称", entry.name,
            SftpInputPurpose::Name, name) && name != entry.name) {
        if (std::any_of(sftpEntries_.begin(), sftpEntries_.end(),
                [&name](const SftpEntry& other) { return other.name == name; })) {
            AppendStatus(L"远端已存在同名项目。", true);
            return;
        }
        if (activeSession_ && activeSession_->id == sessionId) StartSftpMutation(SftpMutationKind::Rename,
            {JoinSftpRemotePath(sourceDirectory, entry.name),
             JoinSftpRemotePath(sourceDirectory, name)});
    }
}

void MainWindow::DeleteSelectedSftpEntries() {
    if (!activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto sourceDirectory = sftpDirectory_;
    const std::vector<size_t> selected = SelectedSftpIndices();
    if (sftpBusy_ || selected.empty()) return;
    if (std::any_of(selected.begin(), selected.end(), [this](size_t index) {
            return !sftpEntries_[index].pathSafe || sftpEntries_[index].symlink;
        })) {
        AppendStatus(L"所选项目含有无法安全修改权限的名称或符号链接。", true);
        return;
    }
    std::vector<std::wstring> paths;
    std::vector<bool> directories;
    std::wstring prompt = L"确定删除以下远端项目？\n\n";
    for (size_t index : selected) {
        const SftpEntry& entry = sftpEntries_[index];
        const std::wstring path = JoinSftpRemotePath(sourceDirectory, entry.name);
        paths.push_back(path);
        directories.push_back(entry.directory);
        prompt += path + L"\n";
    }
    if (std::any_of(directories.begin(), directories.end(), [](bool value) { return value; }))
        prompt += L"\n文件夹仅在为空时才能删除。";
    if (MessageBoxW(window_, prompt.c_str(), L"删除远端项目",
            MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK)
        if (activeSession_ && activeSession_->id == sessionId) StartSftpMutation(SftpMutationKind::Delete, paths, L"", directories);
}

void MainWindow::ChangeSelectedSftpMode() {
    if (!activeSession_) return;
    const auto sessionId = activeSession_->id;
    const auto sourceDirectory = sftpDirectory_;
    const std::vector<size_t> selected = SelectedSftpIndices();
    if (sftpBusy_ || selected.empty()) return;
    if (std::any_of(selected.begin(), selected.end(), [this](size_t index) {
            return !sftpEntries_[index].pathSafe;
        })) {
        AppendStatus(L"所选项目含有无法安全操作的远端名称。", true);
        return;
    }
    std::wstring mode = L"644";
    if (selected.size() == 1 && sftpEntries_[selected.front()].directory) mode = L"755";
    std::vector<std::wstring> paths;
    for (size_t index : selected)
        paths.push_back(JoinSftpRemotePath(sourceDirectory, sftpEntries_[index].name));
    if (!PromptSftpValue(L"修改权限", L"权限（八进制，如 644）", mode,
            SftpInputPurpose::Mode, mode)) return;
    if (activeSession_ && activeSession_->id == sessionId) StartSftpMutation(SftpMutationKind::ChangeMode, paths, mode);
}

void MainWindow::CopySelectedSftpPaths() {
    const std::vector<size_t> selected = SelectedSftpIndices();
    std::wstring text;
    for (size_t index : selected) {
        if (!text.empty()) text += L"\r\n";
        text += JoinSftpRemotePath(sftpDirectory_, sftpEntries_[index].name);
    }
    if (SetClipboardText(window_, text)) AppendStatus(L"远端路径已复制", false);
}

void MainWindow::EnterSelectedSftpDirectoryInTerminal() {
    const std::vector<size_t> selected = SelectedSftpIndices();
    if (selected.size() != 1 || !activeSession_) return;
    const SftpEntry entry = sftpEntries_[selected.front()];
    if ((!entry.directory && !entry.symlink) || !entry.pathSafe) return;
    const std::wstring path = JoinSftpRemotePath(sftpDirectory_, entry.name);
    const std::wstring text = L"cd -- " + QuoteShellArgument(path) + SelectedLineEnding();
    const std::string encoded = EncodeTerminalText(text);
    const Bytes bytes(encoded.begin(), encoded.end());
    if (SendBytesToActive(bytes, false))
        AppendStatus(L"已在终端进入该目录", false);
}

void MainWindow::StartSftpMutation(SftpMutationKind kind,
    const std::vector<std::wstring>& paths, const std::wstring& value,
    const std::vector<bool>& directories) {
    if (sftpBusy_ || !activeSession_ || paths.empty()) return;
    if (sftpThread_.joinable()) sftpThread_.join();
    const std::uint64_t sessionId = activeSession_->id;
    const std::wstring host = activeSession_->host;
    const std::uint16_t port = activeSession_->port;
    const std::wstring username = activeSession_->username;
    const std::wstring password = activeSession_->password;
    const HWND target = window_;
    sftpOperationCancel_.store(false);
    sftpOperationSessionId_ = sessionId;
    SetSftpBusy(true);
    AppendStatus(L"正在执行 SFTP 操作…", false);
    auto work = [target, sessionId, host, port, username, password,
        kind, paths, value, directories, cancel = &sftpOperationCancel_] {
        auto* result = new (std::nothrow) SftpMessage();
        if (!result) return;
        result->sessionId = sessionId;
        result->kind = SftpMessageKind::Mutation;
        try {
            SftpClient client(host, port, username, password);
            if (kind == SftpMutationKind::CreateDirectory) {
                result->success = client.CreateDirectory(paths.front(), result->error, cancel);
            } else if (kind == SftpMutationKind::Rename) {
                result->success = paths.size() >= 2 &&
                    client.Rename(paths[0], paths[1], result->error, cancel);
            } else if (kind == SftpMutationKind::Delete) {
                std::vector<SftpDeleteTarget> targets;
                for (size_t index = 0; index < paths.size(); ++index)
                    targets.push_back({paths[index], index < directories.size() && directories[index]});
                result->success = client.Delete(targets, result->error, cancel);
            } else {
                result->success = client.ChangeMode(paths, value, result->error, cancel);
            }
        } catch (...) {
            result->success = false;
            result->error = L"SFTP 文件操作发生内部错误。";
        }
        if (!PostMessageW(target, MessageSftp, 0, reinterpret_cast<LPARAM>(result))) delete result;
    };
    try {
        sftpThread_ = std::thread(std::move(work));
    } catch (...) {
        sftpOperationSessionId_ = 0;
        SetSftpBusy(false);
        AppendStatus(L"无法启动 SFTP 文件操作。", true);
    }
}

bool MainWindow::PromptSftpValue(const std::wstring& title, const std::wstring& label,
    const std::wstring& initialValue, SftpInputPurpose purpose, std::wstring& value, HWND owner) {
    pendingSftpInputTitle_ = title;
    pendingSftpInputLabel_ = label;
    pendingSftpInputValue_ = initialValue;
    pendingSftpInputError_.clear();
    pendingSftpInputPurpose_ = purpose;
    if (UiDialogBoxOwned(instance_, MAKEINTRESOURCEW(IDD_SFTP_INPUT), owner ? owner : window_, SftpInputDialogProc,
                         reinterpret_cast<LPARAM>(this)) != IDOK)
        return false;
    value = pendingSftpInputValue_;
    return true;
}

void MainWindow::HandleSftpMessage(LPARAM value) {
    std::unique_ptr<SftpMessage> result(reinterpret_cast<SftpMessage*>(value));
    if (!result) return;
    const bool transfer = result->kind == SftpMessageKind::Upload ||
        result->kind == SftpMessageKind::Download;
    if (transfer) {
        if (sftpTransferThread_.joinable()) sftpTransferThread_.join();
        auto found = std::find_if(sftpTransfers_.begin(), sftpTransfers_.end(),
            [id = result->transferId](const SftpTransferItem& item) { return item.id == id; });
        if (found != sftpTransfers_.end()) {
            found->state = result->success ? SftpTransferState::Completed :
                (result->canceled ? SftpTransferState::Canceled : SftpTransferState::Failed);
            found->error = result->error;
            if (result->success) {
                found->percent = 100;
                found->transferred = found->total;
                if (found->direction == SftpTransferDirection::Upload) {
                    sftpRefreshSessionId_ = found->sessionId;
                    sftpRefreshDirectory_ = found->remotePath;
                }
            }
        }
        activeSftpTransferId_ = 0;
        SessionState* session = FindSession(result->sessionId);
        if (session == activeSession_) {
            const std::wstring action = result->kind == SftpMessageKind::Upload ? L"上传" : L"下载";
            AppendStatus(result->success ? L"文件" + action + L"完成" :
                (result->error.empty() ? L"文件" + action + L"失败" : result->error),
                !result->success && !result->canceled);
        } else if (session && session->logger) {
            session->logger->WriteStatus(result->success ? L"SFTP 传输完成" : result->error);
        }
        RefreshSftpTransferList();
        StartNextSftpTransfer();
        return;
    }

    if (sftpThread_.joinable()) sftpThread_.join();
    sftpOperationSessionId_ = 0;
    SessionState* session = FindSession(result->sessionId);
    SetSftpBusy(false);
    if (!session) {
        if (activeSession_ && selectedMode_ == 0 && !sftpBusy_)
            RefreshSftp(activeSession_->sftpDirectory);
        return;
    }
    const bool active = session == activeSession_;
    if (!result->success) {
        const std::wstring message = result->error.empty() ? L"SFTP 操作失败" : result->error;
        if (active) {
            if (sftpPanelVisible_) SetWindowTextW(sftpPath_, sftpDirectory_.c_str());
            AppendStatus(message, true);
            if (result->kind == SftpMessageKind::Mutation)
                RefreshSftp(sftpDirectory_);
        } else if (session->logger) session->logger->WriteStatus(message);
        return;
    }
    if (result->kind == SftpMessageKind::List) {
        const bool learnedHomeDirectory = session->sftpHomeDirectory.empty();
        if (learnedHomeDirectory) session->sftpHomeDirectory = result->listing.directory;
        session->sftpDirectory = result->listing.directory;
        session->sftpEntries = std::move(result->listing.entries);
        if (active) {
            sftpDirectory_ = session->sftpDirectory;
            sftpEntries_ = session->sftpEntries;
            RefreshSftpList();
            AppendStatus(L"SFTP 目录已刷新", false);
        }
        const std::wstring pendingDirectory = session->pendingSftpDirectory;
        session->pendingSftpDirectory.clear();
        if (active && session->sftpFollowTerminal && !pendingDirectory.empty() && pendingDirectory != session->sftpDirectory) {
            session->pendingSftpDirectory = pendingDirectory;
            RefreshSftp(pendingDirectory);
        }
        else if (learnedHomeDirectory)
            SyncSftpDirectoryFromTerminal(*session);
        if (!active && activeSession_ && selectedMode_ == 0 && !sftpBusy_)
            RefreshSftp(activeSession_->sftpDirectory);
    } else if (result->kind == SftpMessageKind::Mutation && active) {
        AppendStatus(L"SFTP 操作完成", false);
        RefreshSftp(sftpDirectory_);
    }
}

INT_PTR CALLBACK MainWindow::SftpInputDialogProc(
    HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        ApplyDarkTitleBar(dialog, self->darkMode_);
        for (HWND child = GetWindow(dialog, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont_), TRUE);
            wchar_t className[32]{};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (_wcsicmp(className, L"Edit") == 0) {
                PrepareDialogEditField(dialog, child, self);
                SetWindowSubclass(child, DialogControlSubclassProc, 1,
                    reinterpret_cast<DWORD_PTR>(self));
            } else if (_wcsicmp(className, L"Button") == 0) {
                SetWindowSubclass(child, ButtonSubclassProc, 1,
                    reinterpret_cast<DWORD_PTR>(self));
            }
        }
        SetWindowTextW(dialog, self->pendingSftpInputTitle_.c_str());
        SetDlgItemTextW(dialog, IDC_SFTP_INPUT_LABEL, self->pendingSftpInputLabel_.c_str());
        SetDlgItemTextW(dialog, IDC_SFTP_INPUT, self->pendingSftpInputValue_.c_str());
        SetDlgItemTextW(dialog, IDC_SFTP_INPUT_ERROR, L"");
        FitDialogLabels(dialog);
        SendDlgItemMessageW(dialog, IDC_SFTP_INPUT, EM_LIMITTEXT,
            self->pendingSftpInputPurpose_ == SftpInputPurpose::Mode ? 4 : self->pendingSftpInputPurpose_ == SftpInputPurpose::Port ? 5 : self->pendingSftpInputPurpose_ == SftpInputPurpose::Path ? 1024 : 255, 0);
        SendDlgItemMessageW(dialog, IDC_SFTP_INPUT, EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        CenterOnOwner(dialog, self->window_);
        SetFocus(GetDlgItem(dialog, IDC_SFTP_INPUT));
        SendDlgItemMessageW(dialog, IDC_SFTP_INPUT, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (!self) return FALSE;
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(dialog, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, self->panelBrush_);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC ||
        message == WM_CTLCOLOREDIT) {
        const Palette colors = Colors(self->darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetDlgCtrlID(control) == IDC_SFTP_INPUT_ERROR ?
            colors.danger : colors.text);
        if (message == WM_CTLCOLOREDIT) {
            SetBkColor(dc, colors.terminal);
            return reinterpret_cast<INT_PTR>(self->terminalBrush_);
        }
        SetBkColor(dc, colors.panel);
        return reinterpret_cast<INT_PTR>(self->panelBrush_);
    }
    if (message == WM_DRAWITEM) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wParam);
        if (id == IDC_SFTP_INPUT && HIWORD(wParam) == EN_CHANGE) {
            const std::wstring rawValue = ControlText(GetDlgItem(dialog, IDC_SFTP_INPUT));
            const std::wstring value = self->pendingSftpInputPurpose_ == SftpInputPurpose::Mode ?
                Trim(rawValue) : rawValue;
            bool valid = !value.empty();
            if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Mode) {
                valid = (value.size() == 3 || value.size() == 4) &&
                    std::all_of(value.begin(), value.end(), [](wchar_t character) {
                        return character >= L'0' && character <= L'7';
                    });
            } else if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Port) {
                valid = ParsePositive(value) > 0 && ParsePositive(value) <= 65535;
            } else if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Path) {
                valid = !value.empty() && value.find_first_of(L"\r\n\x001A") == std::wstring::npos && (value.front() == L'/' || value == L"~" || value.rfind(L"~/", 0) == 0);
            } else {
                valid = !value.empty() && value != L"." && value != L".." &&
                    value.find_first_of(L"/\r\n\x001A") == std::wstring::npos;
            }
            EnableWindow(GetDlgItem(dialog, IDOK), valid);
            SetDlgItemTextW(dialog, IDC_SFTP_INPUT_ERROR, L"");
        FitDialogLabels(dialog);
            InvalidateRect(GetDlgItem(dialog, IDOK), nullptr, TRUE);
            InvalidateRect(GetDlgItem(dialog, IDC_SFTP_INPUT_ERROR), nullptr, TRUE);
            return TRUE;
        }
        if (id == IDOK) {
            const std::wstring rawValue = ControlText(GetDlgItem(dialog, IDC_SFTP_INPUT));
            const std::wstring value = self->pendingSftpInputPurpose_ == SftpInputPurpose::Mode ?
                Trim(rawValue) : rawValue;
            std::wstring error;
            if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Mode) {
                if ((value.size() != 3 && value.size() != 4) ||
                    !std::all_of(value.begin(), value.end(), [](wchar_t character) {
                        return character >= L'0' && character <= L'7';
                    }))
                    error = L"请输入三位或四位八进制权限，例如 644。";
            } else if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Port) {
                if (ParsePositive(value) == 0 || ParsePositive(value) > 65535) error = L"端口必须在 1–65535 之间";
            } else if (self->pendingSftpInputPurpose_ == SftpInputPurpose::Path) {
                if (value.empty() || value.find_first_of(L"\r\n\x001A") != std::wstring::npos || !(value.front() == L'/' || value == L"~" || value.rfind(L"~/", 0) == 0)) error = L"请输入绝对路径或 ~/ 路径";
            } else if (value.empty()) {
                error = L"名称不能为空。";
            } else if (value == L"." || value == L".." ||
                value.find_first_of(L"/\r\n\x001A") != std::wstring::npos) {
                error = L"名称不能是 .、..，也不能包含 /、换行或 Ctrl-Z。";
            }
            if (!error.empty()) {
                SetDlgItemTextW(dialog, IDC_SFTP_INPUT_ERROR, error.c_str());
                InvalidateRect(GetDlgItem(dialog, IDC_SFTP_INPUT_ERROR), nullptr, TRUE);
                return TRUE;
            }
            self->pendingSftpInputValue_ = value;
            UiEndOwnedDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            UiEndOwnedDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

std::wstring MainWindow::DefaultCommandsPath() const {
    wchar_t appData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData))) return L"commands.txt";
    std::wstring directory = std::wstring(appData) + L"\\SerialCtl";
    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    return directory + L"\\commands.txt";
}

std::wstring MainWindow::InitialCommandDirectory() {
    if (!commandDirectory_.empty()) return commandDirectory_;
    const std::wstring statePath = DirectoryOf(DefaultCommandsPath()) + L"\\last-directory.txt";
    FILE* file = nullptr;
    if (_wfopen_s(&file, statePath.c_str(), L"rb") == 0 && file) {
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size < 0 || size > 4 * 1024 * 1024) { fclose(file); return {}; }
    std::string bytes(static_cast<size_t>(size), '\0');
        if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) { fclose(file); return {}; }
        fclose(file);
        commandDirectory_ = Trim(MultiByteToWide(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), CP_UTF8));
        const DWORD attributes = GetFileAttributesW(commandDirectory_.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return commandDirectory_;
        commandDirectory_.clear();
    }
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath))))
        commandDirectory_ = DirectoryOf(modulePath);
    return commandDirectory_;
}

void MainWindow::RememberCommandDirectory(const std::wstring& selectedPath) {
    commandDirectory_ = DirectoryOf(selectedPath);
    if (commandDirectory_.empty()) return;
    const std::wstring statePath = DirectoryOf(DefaultCommandsPath()) + L"\\last-directory.txt";
    FILE* file = nullptr;
    if (_wfopen_s(&file, statePath.c_str(), L"wb") != 0 || !file) return;
    const std::string bytes = WideToMultiByte(commandDirectory_, CP_UTF8);
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

void MainWindow::LoadCommands() {
    std::wstring error;
    const std::wstring path = DefaultCommandsPath();
    if (!LoadCommandsFromFile(path, error)) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            commandsFileProtected_ = true;
            AppendStatus(error + L" 原文件已保留，可导入有效文件后保存。", true);
            return;
        }
        commands_ = {{L"帮助", L"help"}, {L"打印环境变量", L"printenv"}, {L"存储设备", L"mmc list"},
            {L"复位", L"reset"}, {L"启动", L"boot"}};
        SaveCommands();
    }
}

bool MainWindow::SaveCommands() {
    if (commandsFileProtected_) { AppendStatus(L"命令文件读取失败，原文件已保护。请导入有效文件。", true); return false; }
    std::wstring error;
    if (!SaveCommandsToFile(DefaultCommandsPath(), error)) { if (window_) AppendStatus(error, true); return false; }
    commandsDirty_ = false;
    UpdateCommandActions();
    if (window_) AppendStatus(L"命令及顺序已保存", false);
    return true;
}
void MainWindow::MarkCommandsDirty() {
    commandsDirty_ = true;
    UpdateCommandActions();
}

bool MainWindow::LoadCommandsFromFile(const std::wstring& path, std::wstring& error) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) { error = L"无法打开命令文件。"; return false; }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) { fclose(file); error = L"命令文件大小无效或超过 4 MB。"; return false; }
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) { fclose(file); error = L"命令文件读取失败。"; return false; }
    fclose(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) bytes.erase(0, 3);
    if (!bytes.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0)) { error = L"命令文件不是有效 UTF-8。"; return false; }
    const std::wstring content = MultiByteToWide(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), CP_UTF8);
    std::vector<CommandItem> parsed;
    std::wstring pendingName;
    CommandItem pendingMacro;
    bool readingMacro = false;
    size_t start = 0;
    while (start <= content.size()) {
        const size_t end = content.find(L'\n', start);
        std::wstring line = Trim(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty()) {
            if (readingMacro) {
                if (line == L"[[/macro]]") {
                    if (pendingMacro.commands.empty()) {
                        error = L"宏中没有有效指令。";
                        return false;
                    }
                    if (pendingMacro.name.empty()) pendingMacro.name = pendingMacro.commands.front();
                    parsed.push_back(std::move(pendingMacro));
                    pendingMacro = {};
                    readingMacro = false;
                } else {
                    UnescapeMacroCommandLine(line);
                    pendingMacro.commands.push_back(std::move(line));
                    if (pendingMacro.commands.size() > MaximumCommandSteps) {
                        error = L"每个宏最多包含 " + std::to_wstring(MaximumCommandSteps) + L" 条指令。";
                        return false;
                    }
                }
            } else if (UnescapeSingleCommandLine(line)) {
                parsed.push_back({pendingName.empty() ? line : pendingName, line});
                pendingName.clear();
            } else if (line.front() == L'#') {
                pendingName = Trim(line.substr(1));
            } else {
                std::uint32_t intervalMs = 0;
                if (ParseMacroHeader(line, intervalMs)) {
                    pendingMacro = {};
                    pendingMacro.name = pendingName;
                    pendingMacro.intervalMs = intervalMs;
                    pendingName.clear();
                    readingMacro = true;
                } else if (line.rfind(L"[[macro", 0) == 0) {
                    error = L"宏开始标记无效，指令间隔必须是 1 到 60000 毫秒。";
                    return false;
                } else if (line == L"[[/macro]]") {
                    error = L"命令文件中存在没有开始标记的宏结束行。";
                    return false;
                } else {
                    parsed.push_back({pendingName.empty() ? line : pendingName, line});
                    pendingName.clear();
                }
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (readingMacro) {
        error = L"命令文件中的宏缺少 [[/macro]] 结束标记。";
        return false;
    }
    commands_ = std::move(parsed);
    return true;
}

bool MainWindow::SaveCommandsToFile(const std::wstring& path, std::wstring& error) const {
    FILE* file = nullptr;
    const std::wstring temporary = path + L".tmp";
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) { error = L"无法保存命令文件。"; return false; }
    bool wrote = true;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    wrote = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom);
    for (const CommandItem& item : commands_) {
        std::wstring block = L"#" + item.name + L"\r\n";
        if (item.commands.size() > 1) {
            block += L"[[macro interval=" + std::to_wstring(item.intervalMs) + L"]]\r\n";
            for (const std::wstring& command : item.commands)
                block += EscapeMacroCommandLine(command) + L"\r\n";
            block += L"[[/macro]]\r\n";
        } else if (!item.commands.empty()) {
            block += EscapeSingleCommandLine(item.commands.front()) + L"\r\n";
        }
        const std::string utf8 = WideToMultiByte(block, CP_UTF8);
        if (fwrite(utf8.data(), 1, utf8.size(), file) != utf8.size()) wrote = false;
    }
    if (fflush(file) != 0) wrote = false;
    if (fclose(file) != 0) wrote = false;
    if (!wrote || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); error = L"保存失败，原命令文件已保留。"; return false;
    }
    return true;
}

void MainWindow::RefreshCommandList() {
    SendMessageW(commandList_, LB_RESETCONTENT, 0, 0);
    for (const CommandItem& item : commands_)
        SendMessageW(commandList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name.c_str()));
    UpdateCommandActions();
    InvalidateRect(commandList_, nullptr, TRUE);
}

void MainWindow::UpdateCommandActions() {
    if (!deleteCommandButton_ || !commandList_) return;
    const LRESULT selected = SendMessageW(commandList_, LB_GETCURSEL, 0, 0);
    const bool idle = runningCommandIndex_ < 0;
    EnableWindow(saveCommandsButton_, commandsDirty_ && idle && !commandsFileProtected_);
    SetWindowTextW(saveCommandsButton_, commandsDirty_ ? L"保存*" : L"保存");
    InvalidateRect(saveCommandsButton_, nullptr, TRUE);
    EnableWindow(deleteCommandButton_, idle && selected != LB_ERR &&
        selected < static_cast<LRESULT>(commands_.size()));
    EnableWindow(importButton_, idle);
    EnableWindow(addCommandButton_, idle);


    InvalidateRect(deleteCommandButton_, nullptr, TRUE);
    InvalidateRect(importButton_, nullptr, TRUE);
    InvalidateRect(addCommandButton_, nullptr, TRUE);


    InvalidateRect(commandList_, nullptr, FALSE);
}

void MainWindow::UpdateCommandDrag(POINT point, bool edgeScroll) {
    if (draggingCommandIndex_ < 0 || !commandDragGhost_) return;
    RECT client{}; GetClientRect(commandList_, &client);
    const bool wasInside = commandDragInside_;
    commandDragInside_ = PointInRect(client, point);
    bool scrolled = false;
    if (edgeScroll && commandDragInside_) {
        const int top = static_cast<int>(SendMessageW(commandList_, LB_GETTOPINDEX, 0, 0));
        const int rowHeight = std::max(1, static_cast<int>(SendMessageW(commandList_, LB_GETITEMHEIGHT, 0, 0)));
        const int visibleRows = std::max(1, static_cast<int>(client.bottom / rowHeight));
        const int lastTop = std::max(0, static_cast<int>(commands_.size()) - visibleRows);
        const int next = point.y < Ui::Scale(28) ? std::max(0, top - 1) :
            point.y > client.bottom - Ui::Scale(28) ? std::min(lastTop, top + 1) : top;
        if (next != top) {
            SendMessageW(commandList_, LB_SETTOPINDEX, next, 0);
            scrolled = true;
        }
    }
    const int previousInsertion = commandInsertIndex_;
    const int previousDrop = commandDropIndex_;
    if (commandDragInside_ && !commands_.empty()) {
        const DWORD hit = static_cast<DWORD>(SendMessageW(commandList_, LB_ITEMFROMPOINT, 0,
            MAKELPARAM(point.x, point.y)));
        const int row = std::min<int>(LOWORD(hit), static_cast<int>(commands_.size()) - 1);
        RECT item{};
        if (SendMessageW(commandList_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) != LB_ERR) {
            commandInsertIndex_ = row + (point.y >= (item.top + item.bottom) / 2 ? 1 : 0);
            commandDropIndex_ = commandInsertIndex_ - (commandInsertIndex_ > draggingCommandIndex_ ? 1 : 0);
            commandDropIndex_ = std::clamp(commandDropIndex_, 0, static_cast<int>(commands_.size()) - 1);
        }
    }
    POINT screen = point;
    ClientToScreen(commandList_, &screen);
    RECT old{}; GetWindowRect(commandDragGhost_, &old);
    const int x = screen.x - commandDragGrab_.x, y = screen.y - commandDragGrab_.y;
    if (!IsWindowVisible(commandDragGhost_) || old.left != x || old.top != y) {
        SetWindowPos(commandDragGhost_, HWND_TOP, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    if (wasInside != commandDragInside_ || previousDrop != commandDropIndex_)
        InvalidateRect(commandDragGhost_, nullptr, FALSE);
    if (scrolled) {
        InvalidateRect(commandList_, nullptr, FALSE);
    } else if (previousInsertion != commandInsertIndex_ || wasInside != commandDragInside_) {
        for (int insertion : {previousInsertion, commandInsertIndex_}) {
            if (insertion < 0 || commands_.empty()) continue;
            RECT row{};
            const int index = std::min(insertion, static_cast<int>(commands_.size()) - 1);
            if (SendMessageW(commandList_, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row)) != LB_ERR)
                InvalidateRect(commandList_, &row, FALSE);
        }
    }
}

void MainWindow::EndCommandDrag(bool commit) {
    if (draggingCommandIndex_ < 0 && pendingCommandDragIndex_ < 0) return;
    const int from = draggingCommandIndex_, to = commandDropIndex_;
    draggingCommandIndex_ = commandDropIndex_ = pendingCommandDragIndex_ = commandInsertIndex_ = -1;
    KillTimer(commandList_, CommandDragTimerId);
    if (commandDragGhost_) { DestroyWindow(commandDragGhost_); commandDragGhost_ = nullptr; }
    if (GetCapture() == commandList_) ReleaseCapture();
    if (commit && from >= 0 && to >= 0) MoveCommand(static_cast<size_t>(from), static_cast<size_t>(to));
    InvalidateRect(commandList_, nullptr, FALSE);
}

void MainWindow::MoveCommand(size_t from, size_t to) {
    if (runningCommandIndex_ >= 0 || from >= commands_.size() || to >= commands_.size() || from == to) return;
    CommandItem item = commands_[from];
    commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(from));
    to = std::min(to, commands_.size());
    commands_.insert(commands_.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
    MarkCommandsDirty();
    RefreshCommandList();
    SendMessageW(commandList_, LB_SETCURSEL, static_cast<WPARAM>(to), 0);
    UpdateCommandActions();
    AppendStatus(L"命令顺序已更新", false);
}

void MainWindow::ImportCommands() {
    if (runningCommandIndex_ >= 0) return;
    wchar_t path[MAX_PATH]{};
    const std::wstring initialDirectory = InitialCommandDirectory();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"命令文本 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    ImportCommandFile(path);
}

void MainWindow::ImportCommandFile(const std::wstring& path) {
    if (runningCommandIndex_ >= 0 || path.empty()) return;
    if (commandsDirty_) {
        const int choice = MessageBoxW(window_, L"当前命令尚未保存。导入前是否保存？", L"导入命令", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL || (choice == IDYES && !SaveCommands())) return;
    }
    RememberCommandDirectory(path);
    std::wstring error;
    auto previousCommands = commands_;
    if (!LoadCommandsFromFile(path, error)) { AppendStatus(error, true); return; }
    if (commandsFileProtected_) {
        const std::wstring backup = DefaultCommandsPath() + L".unreadable-" + std::to_wstring(GetTickCount64()) + L".bak";
        if (!CopyFileW(DefaultCommandsPath().c_str(), backup.c_str(), TRUE)) {
            commands_ = std::move(previousCommands);
            AppendStatus(L"无法备份原命令文件，尚未允许覆盖。", true); return;
        }
        commandsFileProtected_ = false;
    }
    MarkCommandsDirty();
    RefreshCommandList();
    AppendStatus(L"已导入常用命令。", false);
}

void MainWindow::ExportCommands() {
    wchar_t path[MAX_PATH] = L"常用命令.txt";
    const std::wstring initialDirectory = InitialCommandDirectory();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"命令文本 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    RememberCommandDirectory(path);
    std::wstring error;
    if (!SaveCommandsToFile(path, error)) { AppendStatus(error, true); return; }
    AppendStatus(L"常用命令已导出。", false);
}

void MainWindow::CreateCommandDialogStep(HWND dialog, const std::wstring& text) {
    const size_t index = commandDialogStepEdits_.size();
    HWND edit = nullptr;
    HWND remove = nullptr;
    if (index == 0) {
        edit = GetDlgItem(dialog, IDC_COMMAND_TEXT);
        remove = GetDlgItem(dialog, IDC_COMMAND_REMOVE_BASE);
    } else {
        edit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 1, 1, dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMMAND_STEP_BASE + index)),
            instance_, nullptr);
        remove = CreateWindowExW(0, L"BUTTON", L"删除",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMMAND_REMOVE_BASE + index)),
            instance_, nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        SendMessageW(remove, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        PrepareDialogEditField(dialog, edit, this);
        SetWindowSubclass(edit, DialogControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(remove, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    SendMessageW(edit, EM_LIMITTEXT, 4096, 0);
    SetWindowTextW(edit, text.c_str());
    commandDialogStepEdits_.push_back(edit);
    commandDialogRemoveButtons_.push_back(remove);
}

void MainWindow::ResetCommandDialogSteps(HWND dialog, const std::vector<std::wstring>& commands) {
    for (size_t index = 1; index < commandDialogStepEdits_.size(); ++index) {
        HWND frame = reinterpret_cast<HWND>(GetPropW(commandDialogStepEdits_[index], Ui::FieldFrameProperty));
        if (frame) DestroyWindow(frame);
        DestroyWindow(commandDialogStepEdits_[index]);
        DestroyWindow(commandDialogRemoveButtons_[index]);
    }
    commandDialogStepEdits_.clear();
    commandDialogRemoveButtons_.clear();
    if (commands.empty()) {
        CreateCommandDialogStep(dialog, L"");
    } else {
        for (const std::wstring& command : commands) CreateCommandDialogStep(dialog, command);
    }
}

void MainWindow::LayoutCommandDialog(HWND dialog) {
    UiRedrawScope redraw(dialog);
    UiDialogViewport::Reset(dialog);
    constexpr int DialogWidth = 238;
    constexpr int Margin = 12;
    constexpr int StepTop = 58;
    constexpr int StepStride = 18;
    constexpr int FieldWidth = 174;
    constexpr int ActionLeft = 194;
    constexpr int ActionWidth = 32;
    const int stepCount = static_cast<int>(commandDialogStepEdits_.size());
    const bool macro = stepCount > 1;

    MoveDialogItemDlu(dialog, IDC_COMMAND_NAME, Margin, 22, DialogWidth - Margin * 2, 14);
    for (int index = 0; index < stepCount; ++index) {
        const int y = StepTop + index * StepStride;
        const int columnLeft = 0;
        MoveDialogItemDlu(dialog, GetDlgCtrlID(commandDialogStepEdits_[static_cast<size_t>(index)]),
            Margin + columnLeft, y, macro ? FieldWidth : 214, 14);
        MoveDialogItemDlu(dialog, GetDlgCtrlID(commandDialogRemoveButtons_[static_cast<size_t>(index)]),
            ActionLeft + columnLeft, y, ActionWidth, 14);
        ShowWindow(commandDialogRemoveButtons_[static_cast<size_t>(index)], macro ? SW_SHOW : SW_HIDE);

        HWND field = reinterpret_cast<HWND>(GetPropW(
            commandDialogStepEdits_[static_cast<size_t>(index)], Ui::FieldFrameProperty));
        if (field) {
            RECT fieldRect{};
            RECT buttonRect{};
            GetWindowRect(field, &fieldRect);
            GetWindowRect(commandDialogRemoveButtons_[static_cast<size_t>(index)], &buttonRect);
            MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT*>(&fieldRect), 2);
            SetWindowPos(commandDialogRemoveButtons_[static_cast<size_t>(index)], nullptr,
                fieldRect.right + Ui::Space, fieldRect.top, buttonRect.right - buttonRect.left,
                fieldRect.bottom - fieldRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    const int addY = StepTop + stepCount * StepStride;
    MoveDialogItemDlu(dialog, IDC_COMMAND_ADD_STEP, Margin, addY, 72, 14);
    const int intervalLabelY = addY + 22;
    const int intervalFieldY = intervalLabelY + 12;
    ShowDialogItem(dialog, IDC_COMMAND_INTERVAL_LABEL, macro);
    ShowDialogItem(dialog, IDC_COMMAND_INTERVAL, macro);
    if (macro) {
        MoveDialogItemDlu(dialog, IDC_COMMAND_INTERVAL_LABEL, Margin, intervalLabelY, 110, 10);
        MoveDialogItemDlu(dialog, IDC_COMMAND_INTERVAL, Margin, intervalFieldY, 214, 14);
    }
    const int errorY = macro ? intervalFieldY + 18 : addY + 22;
    const int buttonY = errorY + 12;
    MoveDialogItemDlu(dialog, IDC_COMMAND_ERROR, Margin, errorY, 214, 10);
    MoveDialogItemDlu(dialog, IDCANCEL, DialogWidth - 156, buttonY, 68, 18);
    MoveDialogItemDlu(dialog, IDOK, DialogWidth - 80, buttonY, 68, 18);
    ResizeDialogClientDlu(dialog, DialogWidth, buttonY + 26);
    FitDialogLabels(dialog);
    UiDialogViewport::Fit(dialog, Ui::Dpi);
    EnableWindow(GetDlgItem(dialog, IDC_COMMAND_ADD_STEP),
        commandDialogStepEdits_.size() < MaximumCommandSteps);
    InvalidateRect(GetDlgItem(dialog, IDC_COMMAND_ADD_STEP), nullptr, TRUE);
    RedrawWindow(dialog, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::UpdateCommandDialogValidation(HWND dialog) {
    bool valid = !commandDialogStepEdits_.empty();
    for (HWND edit : commandDialogStepEdits_)
        valid = valid && !Trim(ControlText(edit)).empty();
    if (commandDialogStepEdits_.size() > 1) {
        const DWORD interval = ParsePositive(Trim(ControlText(GetDlgItem(dialog, IDC_COMMAND_INTERVAL))));
        valid = valid && interval > 0 && interval <= MaximumCommandIntervalMs;
    }
    EnableWindow(GetDlgItem(dialog, IDOK), valid);
    InvalidateRect(GetDlgItem(dialog, IDOK), nullptr, TRUE);
}

INT_PTR CALLBACK MainWindow::CommandDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        ApplyDarkTitleBar(dialog, self->darkMode_);
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont_), TRUE);
            wchar_t className[32]{};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (_wcsicmp(className, L"Edit") == 0) {
                PrepareDialogEditField(dialog, child, self);
                SetWindowSubclass(child, DialogControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
                SetWindowPos(child, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            } else if (_wcsicmp(className, L"Button") == 0) {
                SetWindowSubclass(child, ButtonSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
            }
        }
        SendDlgItemMessageW(dialog, IDC_COMMAND_NAME, EM_LIMITTEXT, 128, 0);
        SendDlgItemMessageW(dialog, IDC_COMMAND_INTERVAL, EM_LIMITTEXT, 5, 0);
        SetDlgItemTextW(dialog, IDC_COMMAND_NAME, self->pendingCommand_.name.c_str());
        SetDlgItemTextW(dialog, IDC_COMMAND_INTERVAL,
            std::to_wstring(self->pendingCommand_.intervalMs).c_str());
        SetDlgItemTextW(dialog, IDC_COMMAND_ERROR, L"");
        SetWindowTextW(dialog, self->editingCommandIndex_ >= 0 ? L"编辑常用命令" : L"添加常用命令");
        self->ResetCommandDialogSteps(dialog, self->pendingCommand_.commands);
        self->LayoutCommandDialog(dialog);
        self->UpdateCommandDialogValidation(dialog);
        CenterOnOwner(dialog, self->window_);
        return TRUE;
    }
    if (!self) return FALSE;
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(dialog, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, self->panelBrush_);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
        const Palette colors = Colors(self->darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        const HWND control = reinterpret_cast<HWND>(lParam);
        SetTextColor(dc, GetDlgCtrlID(control) == IDC_COMMAND_ERROR ? colors.danger : colors.text);
        if (message == WM_CTLCOLOREDIT) {
            SetBkColor(dc, colors.terminal);
            return reinterpret_cast<INT_PTR>(self->terminalBrush_);
        }
        SetBkColor(dc, colors.panel);
        return reinterpret_cast<INT_PTR>(self->panelBrush_);
    }
    if (message == WM_DRAWITEM) {
        self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        const HWND source = reinterpret_cast<HWND>(lParam);
        const bool stepEdit = std::find(self->commandDialogStepEdits_.begin(),
            self->commandDialogStepEdits_.end(), source) != self->commandDialogStepEdits_.end();
        if (notification == EN_CHANGE && (stepEdit || id == IDC_COMMAND_INTERVAL)) {
            SetDlgItemTextW(dialog, IDC_COMMAND_ERROR, L"");
            self->UpdateCommandDialogValidation(dialog);
            InvalidateRect(GetDlgItem(dialog, IDC_COMMAND_ERROR), nullptr, TRUE);
            return TRUE;
        }
        if (id == IDC_COMMAND_ADD_STEP && notification == BN_CLICKED) {
            if (self->commandDialogStepEdits_.size() >= MaximumCommandSteps) return TRUE;
            self->CreateCommandDialogStep(dialog, L"");
            self->LayoutCommandDialog(dialog);
            self->UpdateCommandDialogValidation(dialog);
            CenterOnOwner(dialog, self->window_);
            SetFocus(self->commandDialogStepEdits_.back());
            return TRUE;
        }
        if (id >= IDC_COMMAND_REMOVE_BASE &&
            id < IDC_COMMAND_REMOVE_BASE + static_cast<int>(MaximumCommandSteps) &&
            notification == BN_CLICKED) {
            const size_t index = static_cast<size_t>(id - IDC_COMMAND_REMOVE_BASE);
            if (self->commandDialogStepEdits_.size() <= 1 || index >= self->commandDialogStepEdits_.size())
                return TRUE;
            std::vector<std::wstring> commands;
            commands.reserve(self->commandDialogStepEdits_.size() - 1);
            for (size_t step = 0; step < self->commandDialogStepEdits_.size(); ++step) {
                if (step != index) commands.push_back(ControlText(self->commandDialogStepEdits_[step]));
            }
            self->ResetCommandDialogSteps(dialog, commands);
            self->LayoutCommandDialog(dialog);
            self->UpdateCommandDialogValidation(dialog);
            CenterOnOwner(dialog, self->window_);
            SetFocus(self->commandDialogStepEdits_[std::min(index, self->commandDialogStepEdits_.size() - 1)]);
            return TRUE;
        }
        if (id == IDOK) {
            std::vector<std::wstring> commands;
            commands.reserve(self->commandDialogStepEdits_.size());
            for (size_t index = 0; index < self->commandDialogStepEdits_.size(); ++index) {
                std::wstring command = Trim(ControlText(self->commandDialogStepEdits_[index]));
                if (command.empty()) {
                    SetDlgItemTextW(dialog, IDC_COMMAND_ERROR,
                        (L"第 " + std::to_wstring(index + 1) + L" 条指令不能为空。").c_str());
                    SetFocus(self->commandDialogStepEdits_[index]);
                    InvalidateRect(GetDlgItem(dialog, IDC_COMMAND_ERROR), nullptr, TRUE);
                    return TRUE;
                }
                commands.push_back(std::move(command));
            }
            std::uint32_t intervalMs = DefaultCommandIntervalMs;
            if (commands.size() > 1) {
                const DWORD value = ParsePositive(Trim(ControlText(GetDlgItem(dialog, IDC_COMMAND_INTERVAL))));
                if (value == 0 || value > MaximumCommandIntervalMs) {
                    SetDlgItemTextW(dialog, IDC_COMMAND_ERROR, L"指令间隔必须是 1 到 60000 毫秒。");
                    SetFocus(GetDlgItem(dialog, IDC_COMMAND_INTERVAL));
                    InvalidateRect(GetDlgItem(dialog, IDC_COMMAND_ERROR), nullptr, TRUE);
                    return TRUE;
                }
                intervalMs = value;
            }
            self->pendingCommand_.name = Trim(ControlText(GetDlgItem(dialog, IDC_COMMAND_NAME)));
            self->pendingCommand_.commands = std::move(commands);
            self->pendingCommand_.intervalMs = intervalMs;
            if (self->pendingCommand_.name.empty())
                self->pendingCommand_.name = self->pendingCommand_.commands.front();
            UiEndOwnedDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            UiEndOwnedDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_DESTROY) {
        self->commandDialogStepEdits_.clear();
        self->commandDialogRemoveButtons_.clear();
    }
    return FALSE;
}

void MainWindow::AddCommand() {
    if (runningCommandIndex_ >= 0) return;
    pendingCommand_ = {};
    pendingCommand_.commands.push_back(L"");
    pendingCommand_.intervalMs = DefaultCommandIntervalMs;
    editingCommandIndex_ = -1;
    if (UiDialogBoxOwned(instance_, MAKEINTRESOURCEW(IDD_COMMAND), window_, CommandDialogProc,
                         reinterpret_cast<LPARAM>(this)) == IDOK) {
        commands_.push_back(pendingCommand_);
        MarkCommandsDirty();
        RefreshCommandList();
        SendMessageW(commandList_, LB_SETCURSEL, commands_.size() - 1, 0);
        UpdateCommandActions();
    }
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::EditCommand(size_t index) {
    if (runningCommandIndex_ >= 0 || index >= commands_.size()) return;
    pendingCommand_ = commands_[index];
    editingCommandIndex_ = static_cast<int>(index);
    if (UiDialogBoxOwned(instance_, MAKEINTRESOURCEW(IDD_COMMAND), window_, CommandDialogProc,
                         reinterpret_cast<LPARAM>(this)) == IDOK) {
        commands_[index] = pendingCommand_;
        MarkCommandsDirty();
        RefreshCommandList();
        SendMessageW(commandList_, LB_SETCURSEL, index, 0);
        UpdateCommandActions();
    }
    editingCommandIndex_ = -1;
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void MainWindow::DeleteSelectedCommand() {
    if (runningCommandIndex_ >= 0) return;
    const LRESULT selected = SendMessageW(commandList_, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) return;
    commands_.erase(commands_.begin() + selected);
    MarkCommandsDirty();
    RefreshCommandList();
}

std::wstring MainWindow::ControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    GetWindowTextW(control, buffer.data(), static_cast<int>(buffer.size()));
    return std::wstring(buffer.data(), static_cast<size_t>(length));
}

void MainWindow::SetControlText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

} // namespace serialctl

namespace serialctl {
Json MainWindow::ApiResources() {
    auto resources = sessionService_.Resources();
    resources.push_back({{"id", "power-1"},
                         {"name", "IT6332A"},
                         {"kind", "power"},
                         {"connected", powerService_.State()["connected"]},
                         {"operations", {"power.get", "power.output"}}});
    return {{"resources", resources}};
}
Json MainWindow::ApiRequest(const std::string &op, const std::string &resource, const Json &params) {
    if (closing_)
        return {{"error", {{"code", "APPLICATION_CLOSING"}, {"message", "Application closing"}}}};
    if (op == "resources")
        return ApiResources();
    if (op == "session.get")
        return sessionService_.State(resource);
    if (op == "input")
        return sessionService_.Input(resource, Decode64(params.at("data").get<std::string>()),
                                     params.value("source", "network"));
    auto request = std::make_shared<ApiMessage>();
    request->method = op;
    request->path = resource;
    request->body = params;
    auto future = request->result.get_future();
    auto *pointer = new ApiRequestMessage(request);
    if (!PostMessageW(window_, MessageApi, 0, reinterpret_cast<LPARAM>(pointer))) {
        delete pointer;
        return {{"error", {{"code", "APPLICATION_CLOSING"}}}};
    }
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        int expected = 0;
        if (request->state.compare_exchange_strong(expected, 2))
            return {{"error", {{"code", "UI_BUSY_CANCELED"}, {"message", "Not executed"}}}};
        future.wait();
    }
    return future.get();
}
Json MainWindow::ExecuteApiRequest(const std::string &op, const std::string &resource, const Json &params) {
    if (closing_)
        return {{"error", {{"code", "APPLICATION_CLOSING"}}}};
    if (op == "action.get")
        return powerService_.Action(params.at("id"));
    if (op == "power.get" && resource == "power-1") {
        auto state = powerService_.State();
        state["selectedChannels"] = powerPane_.SelectedChannels();
        return state;
    }
    if (op == "power.output" && resource == "power-1") {
        Json command = params;
        command["type"] = "output";
        command["source"] = "network";
        command["channels"] = powerPane_.SelectedChannels();
        return powerService_.Submit(command);
    }
    return {{"error", {{"code", "OPERATION_DENIED"}}}};
}
} // namespace serialctl
