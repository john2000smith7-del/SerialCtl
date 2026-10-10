#include <future>
#include <chrono>
#include "TerminalDecoder.h"
#include "MainWindow.h"

#include "PlinkConnection.h"
#include "QueuedConnection.h"
#include "CmdConnection.h"
#include "SerialConnection.h"
#include "SerialShareConnection.h"
#include "SharedSerialConnection.h"
#include "TcpConnection.h"
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
constexpr UINT_PTR DiscoveryTimerId = 4003;
constexpr int IdSharedAdvancedPort = 9010;
constexpr int IdSharedAutoPort = 9011;
constexpr UINT_PTR StatusTimerId = 4001;
constexpr UINT_PTR CommandTimerId = 4002;
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
    IdCmd = 300, IdPower, IdApi
};

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
    IdMenuEncodingGb2312
};

enum LineEndingMenuId {
    IdMenuEndingCr = 2101,
    IdMenuEndingLf,
    IdMenuEndingCrLf,
    IdMenuEndingNone
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
int Section = 16;
int ToolbarHeight = 72;
int ToolbarGroupWidth = 594;
int StandardHeight = 36;
int CompactHeight = 32;
int SegmentHeight = 32;
int SegmentWidth = 70;
int PanelPadding = 16;
int IconButtonSize = 28;
int MinLeftWidth = 196;
int MaxLeftWidth = 260;
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
    Ui::Section=Ui::Scale(Ui::Section);
    Ui::ToolbarHeight=Ui::Scale(Ui::ToolbarHeight);
    Ui::ToolbarGroupWidth=Ui::Scale(Ui::ToolbarGroupWidth);
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

MainLayoutMetrics GetMainLayoutMetrics(
    int width, int height, int preferredRightWidth, bool rightPanelCollapsed) {
    const int leftWidth = std::max(Ui::MinLeftWidth, std::min(Ui::MaxLeftWidth, width * 18 / 100));
    const int availableRight = std::max(Ui::MinRightWidth,
        width - leftWidth - Ui::Gap * 2 - Ui::MinCenterWidth);
    const int rightWidth = rightPanelCollapsed ? Ui::CollapsedRightWidth + Ui::Gap :
        std::max(Ui::MinRightWidth, std::min({Ui::MaxRightWidth,
            availableRight, preferredRightWidth}));
    return {leftWidth, rightWidth, width - rightWidth, leftWidth + Ui::Gap,
        width - rightWidth - Ui::Gap, height - Ui::Gap};
}

struct CommandActionRects {
    RECT run{};
    RECT edit{};
};

CommandActionRects GetCommandActionRects(const RECT& card) {
    const int top = card.top + ((card.bottom - card.top) - Ui::IconButtonSize) / 2;
    RECT edit{card.right - Ui::Space - Ui::IconButtonSize, top,
        card.right - Ui::Space, top + Ui::IconButtonSize};
    RECT run{edit.left - Ui::Space - Ui::IconButtonSize, top,
        edit.left - Ui::Space, top + Ui::IconButtonSize};
    return {run, edit};
}

bool PointInRect(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

struct DiscoveryMessage { HWND dialog; unsigned generation; std::uint16_t port = 0; std::vector<std::wstring> names; std::wstring error; bool success = false; };

struct ApiMessage { std::string method,path; Json body; std::promise<Json> result; std::atomic_int state{0}; };
using ApiRequestMessage = std::shared_ptr<ApiMessage>;
struct ApiDialogData { MainWindow* owner; Json resources; HWND list,info; };

struct ConnectionMessage {
    bool success = false;
    std::wstring error;
};
struct HostKeyRequest {
    std::wstring host;
    std::uint16_t port;
    std::wstring fingerprint;
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
    if (dark) {
        return {RGB(28, 28, 30), RGB(36, 36, 38), RGB(44, 44, 46), RGB(20, 20, 22),
            RGB(58, 58, 60), RGB(245, 245, 247), RGB(152, 152, 157), RGB(10, 132, 255),
            RGB(31, 55, 80), RGB(255, 69, 58)};
    }
    return {RGB(245, 245, 247), RGB(255, 255, 255), RGB(242, 242, 247), RGB(255, 255, 255),
        RGB(210, 210, 215), RGB(29, 29, 31), RGB(110, 110, 115), RGB(0, 122, 255),
        RGB(229, 241, 255), RGB(255, 59, 48)};
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
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, brush));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    // RoundRect expects the corner ellipse diameter, while the design token is a radius.
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
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
    SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_NOSIZE | SWP_NOACTIVATE);
}

void DrawToolbarIcon(HDC dc, int id, RECT rect, COLORREF color) {
    const int x = rect.left + 16, y = (rect.top + rect.bottom) / 2;
    LOGBRUSH brush{BS_SOLID,color,0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 2, &brush, 0, nullptr);
    auto oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    auto line=[&](int a,int b,int c,int d){MoveToEx(dc,x+a,y+b,nullptr);LineTo(dc,x+c,y+d);};
    if(id==IdSsh||id==IdCmd){RoundRect(dc,x-9,y-7,x+10,y+8,4,4);line(-5,-3,-1,0);line(-1,0,-5,3);line(2,3,6,3);if(id==IdSsh){RoundRect(dc,x+3,y-9,x+10,y-3,2,2);Arc(dc,x+4,y-12,x+9,y-5,x+4,y-8,x+9,y-8);}}
    else if(id==IdSerial){RoundRect(dc,x-8,y-6,x+9,y+6,4,4);for(int i=-4;i<=4;i+=4){line(i,-9,i,-6);line(i,6,i,9);}for(int i=-4;i<=4;i+=4)Ellipse(dc,x+i-1,y-1,x+i+1,y+1);}
    else if(id==IdShareSerial||id==IdApi){line(0,-5,0,1);line(-6,5,0,1);line(0,1,6,5);for(auto p:{POINT{0,-7},POINT{-7,7},POINT{7,7}})Ellipse(dc,x+p.x-3,y+p.y-3,x+p.x+3,y+p.y+3);}
    else if(id==IdTelnet){Ellipse(dc,x-9,y-9,x+10,y+10);Ellipse(dc,x-4,y-9,x+5,y+10);line(-8,0,9,0);}
    else if(id==IdPower){RoundRect(dc,x-9,y-8,x+10,y+9,4,4);line(-5,-4,4,-4);for(int i=-5;i<=5;i+=5)Ellipse(dc,x+i-1,y+2,x+i+2,y+5);}
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);
}

void DrawChevronDown(HDC dc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, cx - 4, cy - 2, nullptr);
    LineTo(dc, cx, cy + 2);
    LineTo(dc, cx + 4, cy - 2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawChevronHorizontal(HDC dc, const RECT& rect, COLORREF color, bool right) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const int direction = right ? 1 : -1;
    MoveToEx(dc, cx - 2 * direction, cy - 4, nullptr);
    LineTo(dc, cx + 2 * direction, cy);
    LineTo(dc, cx - 2 * direction, cy + 4);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawSortChevron(HDC dc, const RECT& rect, COLORREF color, bool ascending) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    const int cx = rect.right - 12;
    const int cy = (rect.top + rect.bottom) / 2;
    const int direction = ascending ? -1 : 1;
    MoveToEx(dc, cx - 3, cy + direction * 2, nullptr);
    LineTo(dc, cx, cy - direction * 2);
    LineTo(dc, cx + 3, cy + direction * 2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawSftpEntryIcon(HDC dc, const RECT& rect, COLORREF color, bool directory, bool symlink) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    if (directory) {
        POINT outline[] = {{cx - 8, cy - 5}, {cx - 2, cy - 5}, {cx, cy - 2},
            {cx + 8, cy - 2}, {cx + 8, cy + 6}, {cx - 8, cy + 6}, {cx - 8, cy - 5}};
        Polyline(dc, outline, static_cast<int>(std::size(outline)));
    } else {
        POINT page[] = {{cx - 6, cy - 7}, {cx + 3, cy - 7}, {cx + 7, cy - 3},
            {cx + 7, cy + 7}, {cx - 6, cy + 7}, {cx - 6, cy - 7}};
        Polyline(dc, page, static_cast<int>(std::size(page)));
        MoveToEx(dc, cx + 3, cy - 7, nullptr); LineTo(dc, cx + 3, cy - 3);
        LineTo(dc, cx + 7, cy - 3);
    }
    if (symlink) {
        MoveToEx(dc, cx - 3, cy + 2, nullptr); LineTo(dc, cx + 4, cy + 2);
        MoveToEx(dc, cx + 1, cy - 1, nullptr); LineTo(dc, cx + 4, cy + 2);
        LineTo(dc, cx + 1, cy + 5);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawThemeIcon(HDC dc, const RECT& rect, bool darkMode, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    if (darkMode) {
        Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
        const POINT rays[][2] = {
            {{cx, cy - 9}, {cx, cy - 7}}, {{cx, cy + 7}, {cx, cy + 9}},
            {{cx - 9, cy}, {cx - 7, cy}}, {{cx + 7, cy}, {cx + 9, cy}},
            {{cx - 6, cy - 6}, {cx - 5, cy - 5}}, {{cx + 5, cy + 5}, {cx + 6, cy + 6}},
            {{cx + 5, cy - 5}, {cx + 6, cy - 6}}, {{cx - 6, cy + 6}, {cx - 5, cy + 5}}};
        for (const auto& ray : rays) {
            MoveToEx(dc, ray[0].x, ray[0].y, nullptr);
            LineTo(dc, ray[1].x, ray[1].y);
        }
    } else {
        POINT moon[] = {
            {cx + 3, cy - 8},
            {cx - 7, cy - 7}, {cx - 8, cy + 6}, {cx + 3, cy + 8},
            {cx - 1, cy + 4}, {cx - 1, cy - 4}, {cx + 3, cy - 8}};
        PolyBezier(dc, moon, static_cast<DWORD>(std::size(moon)));
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawPlusIcon(HDC dc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    const int cx = rect.left + 18;
    const int cy = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, cx - 4, cy, nullptr); LineTo(dc, cx + 5, cy);
    MoveToEx(dc, cx, cy - 4, nullptr); LineTo(dc, cx, cy + 5);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawEditIcon(HDC dc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, cx - 6, cy + 6, nullptr);
    LineTo(dc, cx - 4, cy + 1);
    LineTo(dc, cx + 4, cy - 7);
    LineTo(dc, cx + 7, cy - 4);
    LineTo(dc, cx - 1, cy + 4);
    LineTo(dc, cx - 6, cy + 6);
    MoveToEx(dc, cx - 4, cy + 1, nullptr);
    LineTo(dc, cx - 1, cy + 4);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawCommandProgress(HDC dc, const RECT& rect, size_t completed, size_t total,
    COLORREF trackColor, COLORREF progressColor) {
    RECT ring = rect;
    InflateRect(&ring, -2, -2);
    HPEN trackPen = CreatePen(PS_SOLID, 1, trackColor);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, trackPen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    Ellipse(dc, ring.left, ring.top, ring.right, ring.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(trackPen);

    if (total > 0 && completed > 0) {
        constexpr double Pi = 3.14159265358979323846;
        constexpr int FullCircleSegments = 48;
        const double fraction = std::min(1.0,
            static_cast<double>(completed) / static_cast<double>(total));
        const int segments = std::max(1, static_cast<int>(FullCircleSegments * fraction));
        const double centerX = (ring.left + ring.right) / 2.0;
        const double centerY = (ring.top + ring.bottom) / 2.0;
        const double radius = std::min(ring.right - ring.left, ring.bottom - ring.top) / 2.0 - 1.0;
        std::vector<POINT> points;
        points.reserve(static_cast<size_t>(segments) + 1);
        for (int index = 0; index <= segments; ++index) {
            const double angle = -Pi / 2.0 + 2.0 * Pi * index / FullCircleSegments;
            points.push_back({static_cast<LONG>(std::lround(centerX + std::cos(angle) * radius)),
                static_cast<LONG>(std::lround(centerY + std::sin(angle) * radius))});
        }
        HPEN progressPen = CreatePen(PS_SOLID, 2, progressColor);
        oldPen = reinterpret_cast<HPEN>(SelectObject(dc, progressPen));
        Polyline(dc, points.data(), static_cast<int>(points.size()));
        SelectObject(dc, oldPen);
        DeleteObject(progressPen);
    }
    SelectObject(dc, oldBrush);
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
    const int pixelHeight = rect.bottom - rect.top;
    if (frame) {
        MoveWindow(frame, rect.left, rect.top, pixelWidth, pixelHeight, TRUE);
        const int horizontalInset = Ui::Space;
        const int verticalInset = 4;
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

bool IsComboDropList(HWND window) {
    wchar_t className[32]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return _wcsicmp(className, L"ComboLBox") == 0;
}

void DrawOverlayListFrame(HWND list, bool darkMode) {
    RECT windowRect{};
    GetWindowRect(list, &windowRect);
    RECT frame{0, 0, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top};
    HDC dc = GetWindowDC(list);
    if (!dc) return;
    const Palette colors = Colors(darkMode);
    HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
    RoundRect(dc, frame.left, frame.top, frame.right, frame.bottom, Ui::Radius * 2, Ui::Radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
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

void ApplyRoundedControlRegion(HWND control) {
    RECT rect{};
    GetWindowRect(control, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
        Ui::Radius * 2, Ui::Radius * 2);
    SetWindowRgn(control, region, TRUE);
}

void PrepareDialogField(HWND control) {
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_BORDER);
    const LONG_PTR exStyle = GetWindowLongPtrW(control, GWL_EXSTYLE) &
        ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrW(control, GWL_STYLE, style);
    SetWindowLongPtrW(control, GWL_EXSTYLE, exStyle);
    SetWindowPos(control, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ApplyRoundedControlRegion(control);
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

    window_ = CreateWindowExW(0, WindowClassName, L"SerialCtl", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance, this);
    if (!window_) return false;
    ApplyDarkTitleBar(window_, darkMode_);
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
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
    } else if (message == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            RECT rect{};
            GetWindowRect(window, &rect);
            point = {rect.left + 32, rect.top + 32};
        }
        self->ShowTerminalContextMenu(point);
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
        if (shift && wParam == VK_PRIOR) {
            self->ScrollTerminal(std::max(1, self->terminalVisibleRows_ - 1));
            return 0;
        }
        if (shift && wParam == VK_NEXT) {
            self->ScrollTerminal(-std::max(1, self->terminalVisibleRows_ - 1));
            return 0;
        }
        if (shift && wParam == VK_HOME) {
            if (self->activeSession_) {
                const int maximum = std::max(0,
                    static_cast<int>(self->activeSession_->terminal.DisplayLineCount()) -
                        self->terminalVisibleRows_);
                self->activeSession_->terminalScrollOffset = maximum;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (shift && wParam == VK_END) {
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
        if ((keys & MK_CONTROL) != 0) {
            self->terminalZoomWheelRemainder_ += delta;
            const int steps = self->terminalZoomWheelRemainder_ / WHEEL_DELTA;
            self->terminalZoomWheelRemainder_ %= WHEEL_DELTA;
            if (steps != 0) self->ZoomTerminalFont(steps);
        } else if ((keys & MK_SHIFT) != 0) {
            self->terminalHorizontalWheelRemainder_ += delta;
            const int steps = self->terminalHorizontalWheelRemainder_ / WHEEL_DELTA;
            self->terminalHorizontalWheelRemainder_ %= WHEEL_DELTA;
            if (steps != 0) self->ScrollTerminalHorizontal(-steps * 4);
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
        const int timestampWidth = self->timestampEnabled_ && !model.AlternateScreen() ? 112 : 0;
        const TerminalHorizontalScrollMetrics horizontalMetrics =
            GetTerminalHorizontalScrollMetrics(window, 8 + timestampWidth,
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
        if (self->activeSession_->terminalHasNewOutput &&
            point.x >= client.right - 122 && point.y >= client.bottom - 40) {
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
                return iswalnum(character) || wcschr(L"_./~:+-%@", character) != nullptr;
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
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_MOUSEMOVE && self->activeSession_) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (self->terminalHorizontalScrollbarDragging_) {
            const TerminalModel& model = self->activeSession_->terminal;
            const int timestampWidth = self->timestampEnabled_ && !model.AlternateScreen() ? 112 : 0;
            const TerminalHorizontalScrollMetrics metrics =
                GetTerminalHorizontalScrollMetrics(window, 8 + timestampWidth,
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
        self->terminalSelecting_ = false;
        self->terminalScrollbarDragging_ = false;
        self->terminalScrollbarThumbOffset_ = 0;
        self->terminalHorizontalScrollbarDragging_ = false;
        self->terminalHorizontalScrollbarThumbOffset_ = 0;
        if (GetCapture() == window) ReleaseCapture();
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
    (void)referenceData;
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
    } else if (message == WM_NCDESTROY) {
        RemovePropW(window, Ui::HoverProperty);
        RemoveWindowSubclass(window, ButtonSubclassProc, subclassId);
    }
    return DefSubclassProc(window, message, wParam, lParam);
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
    MoveWindow(edit, rect.left + Ui::Space, rect.top + 4,
        std::max(1, width - Ui::Space * 2), std::max(1, height - 8), TRUE);
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
        DrawRoundedBox(dc, client, Ui::Radius,
            enabled ? colors.terminal : colors.panel,
            edit && GetFocus() == edit ? colors.accent : colors.border);
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
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    wchar_t className[32]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    const bool editControl = _wcsicmp(className, L"Edit") == 0;
    if (message == WM_PAINT && _wcsicmp(className, WC_COMBOBOXW) == 0) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const Palette colors = Colors(self->darkMode_);
        const bool enabled = IsWindowEnabled(window) != FALSE;
        DrawRoundedBox(dc, client, Ui::Radius, colors.terminal,
            GetFocus() == window ? colors.accent : colors.border);
        const int selected = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
        std::wstring value;
        if (selected >= 0) {
            const LRESULT length = SendMessageW(window, CB_GETLBTEXTLEN, selected, 0);
            if (length >= 0) {
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
                SendMessageW(window, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(buffer.data()));
                value.assign(buffer.data());
            }
        }
        RECT textRect = client;
        textRect.left += 10;
        textRect.right -= 30;
        DrawTextSimple(dc, value, textRect, enabled ? colors.text : colors.muted,
            self->uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        HPEN arrowPen = CreatePen(PS_SOLID, 2, enabled ? colors.muted : colors.border);
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, arrowPen));
        const int cx = client.right - 14;
        const int cy = client.bottom / 2;
        MoveToEx(dc, cx - 4, cy - 2, nullptr); LineTo(dc, cx, cy + 2);
        LineTo(dc, cx + 4, cy - 2);
        SelectObject(dc, oldPen);
        DeleteObject(arrowPen);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        HWND frame = reinterpret_cast<HWND>(GetPropW(window, Ui::FieldFrameProperty));
        InvalidateRect(frame ? frame : window, nullptr, TRUE);
        return result;
    }
    if (message == WM_SIZE && !editControl) ApplyRoundedControlRegion(window);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, Ui::FieldFrameProperty);
        RemoveWindowSubclass(window, DialogControlSubclassProc, subclassId);
    }
    return result;
}

LRESULT CALLBACK MainWindow::CommandListSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* self = reinterpret_cast<MainWindow*>(referenceData);
    if (message == WM_LBUTTONDOWN) {
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
            RECT card{itemRect.left, itemRect.top + 4, cardRight, itemRect.bottom - 4};
            const CommandActionRects actions = GetCommandActionRects(card);
            SendMessageW(window, LB_SETCURSEL, index, 0);
            self->UpdateCommandActions();
            if (PointInRect(actions.run, point)) {
                if (self->runningCommandIndex_ == index) {
                    self->StopCommandSequence(true);
                    return 0;
                }
                const bool writable = self->connection_ && self->connection_->IsConnected();
                if (writable && self->runningCommandIndex_ < 0)
                    self->SendCommand(static_cast<size_t>(index));
                return 0;
            }
            if (PointInRect(actions.edit, point)) {
                if (self->runningCommandIndex_ < 0)
                    self->EditCommand(static_cast<size_t>(index));
                return 0;
            }
            if (point.x <= itemRect.left + 30 && self->runningCommandIndex_ < 0) {
                self->draggingCommandIndex_ = index;
                SetCapture(window);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }
        }
    } else if (message == WM_MOUSEMOVE) {
        if (self->draggingCommandIndex_ >= 0) {
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
            RECT card{itemRect.left, itemRect.top + 4, cardRight, itemRect.bottom - 4};
            const CommandActionRects actions = GetCommandActionRects(card);
            if (PointInRect(actions.run, point)) { hoverIndex = LOWORD(hit); hoverAction = 1; }
            else if (PointInRect(actions.edit, point)) { hoverIndex = LOWORD(hit); hoverAction = 2; }
        }
        if (hoverIndex != self->hoveredCommandIndex_ || hoverAction != self->hoveredCommandAction_) {
            self->hoveredCommandIndex_ = hoverIndex;
            self->hoveredCommandAction_ = hoverAction;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE) {
        self->hoveredCommandIndex_ = -1;
        self->hoveredCommandAction_ = 0;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_LBUTTONUP && self->draggingCommandIndex_ >= 0) {
        const int from = self->draggingCommandIndex_;
        self->draggingCommandIndex_ = -1;
        if (GetCapture() == window) ReleaseCapture();
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const DWORD hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y)));
        if (!HIWORD(hit) && LOWORD(hit) < self->commands_.size())
            self->MoveCommand(static_cast<size_t>(from), static_cast<size_t>(LOWORD(hit)));
        return 0;
    } else if (message == WM_CAPTURECHANGED) {
        self->draggingCommandIndex_ = -1;
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

    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_PAINT) {
        DrawOverlayScrollbar(window, self->darkMode_);
        if (IsComboDropList(window)) DrawOverlayListFrame(window, self->darkMode_);
    } else if (message == WM_KEYDOWN || message == WM_SIZE || message == LB_SETTOPINDEX) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == LB_ADDSTRING || message == LB_DELETESTRING || message == LB_RESETCONTENT) {
        InvalidateRect(window, nullptr, TRUE);
    }
    if (message == WM_SIZE && IsComboDropList(window)) ApplyRoundedControlRegion(window);
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
    if (message.hwnd != terminal_) { return message.message == WM_KEYDOWN && message.wParam == VK_TAB && IsDialogMessageW(window_, &message); }
    if (message.message != WM_KEYDOWN) return false;
    const WPARAM key = message.wParam;
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
    switch (message) {
    case MessageApi: {
        std::unique_ptr<ApiRequestMessage> pointer(reinterpret_cast<ApiRequestMessage*>(lParam));
        auto request=*pointer; int expected=0;
        if (request->state.compare_exchange_strong(expected,1)) {
            try { request->result.set_value(ExecuteApiRequest(request->method,request->path,request->body)); }
            catch (...) { request->result.set_value({{"error","API operation failed"}}); }
        }
        return 0;
    }
    case WM_CREATE:
        LoadUiState();
        CreateControls();
        LoadCommands();
        RefreshCommandList();
        RefreshConnectionList();
        ApplyTheme();
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(window_, nullptr, TRUE);
        UpdateWindow(window_);
        return 0;
    case WM_SETCURSOR:
        if (!(rightPanelCollapsed_ || rightPanelAutoCollapsed_) && LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_, &point);
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, rightPanelWidth_, (rightPanelCollapsed_ || rightPanelAutoCollapsed_));
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (!(rightPanelCollapsed_ || rightPanelAutoCollapsed_)) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, rightPanelWidth_, (rightPanelCollapsed_ || rightPanelAutoCollapsed_));
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                rightPanelDragging_ = true;
                SetCapture(window_);
                return 0;
            }
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (!(rightPanelCollapsed_ || rightPanelAutoCollapsed_)) {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window_, &client);
            const MainLayoutMetrics layout = GetMainLayoutMetrics(
                client.right, client.bottom, rightPanelWidth_, (rightPanelCollapsed_ || rightPanelAutoCollapsed_));
            if (point.x >= layout.centerRight && point.x < layout.rightLeft) {
                rightPanelWidth_ = 360;
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
            rightPanelWidth_ = std::max(Ui::MinRightWidth,
                std::min(Ui::MaxRightWidth,
                    static_cast<int>(client.right) - GET_X_LPARAM(lParam)));
            LayoutControls(client.right, client.bottom);
            InvalidateRect(window_, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (rightPanelDragging_) {
            rightPanelDragging_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            SaveUiState();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        rightPanelDragging_ = false;
        break;
    case WM_TIMER:
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
        PaintWindow(dc);
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
            measure->itemHeight = measure->itemData == 0 ? 9 : 32;
            return TRUE;
        }
        measure->itemHeight = measure->CtlID == IdCommandList ? Ui::Scale(66) :
            (measure->CtlID == IdSftpList ? Ui::SftpTableRowHeight :
            (measure->CtlID == IdSftpTransferList ? Ui::SftpTransferRowHeight : 58));
        return TRUE;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == terminal_) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(terminal_, &rect);
                point = {rect.left + 32, rect.top + 32};
            }
            ShowTerminalContextMenu(point);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IdApi && HIWORD(wParam) == BN_CLICKED) { ShowApiDialog(); return 0; }
        if (LOWORD(wParam) == IdCmd && HIWORD(wParam) == BN_CLICKED) {
            if (pendingSession_) { AppendStatus(L"请等待当前连接完成", true); return 0; }
            if (connectionThread_.joinable()) connectionThread_.join();
            pendingMode_ = 4; powerVisible_ = false; ConnectFromDialog(); return 0;
        }
        if (LOWORD(wParam) == IdPower && HIWORD(wParam) == BN_CLICKED) {
            powerVisible_ = true; RECT r{}; GetClientRect(window_, &r); LayoutControls(r.right, r.bottom); return 0;
        } {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (notification == BN_CLICKED) {
            if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdApi) {
                OpenConnectionDialog(id - IdSsh);
            }
            else if (id == IdTheme) { darkMode_ = !darkMode_; ApplyTheme(); }
            else if (id == IdDisconnect) Disconnect();
            else if (id == IdSaveCommands) SaveCommands();
            else if (id == IdImport) ImportCommands();
            else if (id == IdExport) ExportCommands();
            else if (id == IdAddCommand) AddCommand();
            else if (id == IdDeleteCommand) DeleteSelectedCommand();
            else if (id == IdCommandTab) ShowSftpPanel(false);
            else if (id == IdSftpTab) { ShowSftpPanel(true); RefreshSftp(); }
            else if (id == IdRightPanelToggle) SetRightPanelCollapsed(!rightPanelCollapsed_);
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
            if (selected != LB_ERR) SwitchSession(static_cast<size_t>(selected));
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
        std::map<std::uint64_t, Bytes> received;
        std::set<std::uint64_t> overflow;
        { std::lock_guard<std::mutex> lock(receivedMutex_); received.swap(received_); overflow.swap(receiveOverflow_); receivePosted_ = false; }
        for (auto& item : received) {
            if (pendingSession_ && item.first == pendingSession_->id) {
                if (pendingConnectionBytes_ + item.second.size() <= 4 * 1024 * 1024) {
                    pendingConnectionBytes_ += item.second.size(); pendingConnectionData_.push_back(std::move(item.second));
                } else { connectionCancel_ = true; pendingSession_->connection->CancelStart(); }
            } else AppendData(item.first, item.second);
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
            if (session == activeSession_) {
                SetConnectedUi(connection_ && connection_->IsConnected());
                AppendStatus(status->text, status->isError);
            } else if (session && session->logger) {
                session->logger->WriteStatus(status->text);
            }
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
        if (result->success) SetDlgItemTextW(result->dialog, IDC_PORT, std::to_wstring(result->port).c_str());
        SetDlgItemTextW(result->dialog, IDC_DIALOG_ERROR, result->success ? (L"服务端口 " + std::to_wstring(result->port) + L" · 选择串口连接").c_str() : result->error.c_str());
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
                const std::wstring prompt = L"首次连接该 SSH 主机：" + request.host + L":" +
                    std::to_wstring(request.port) + L"\n\n主机指纹：\n" + request.fingerprint +
                    L"\n\n请通过服务器管理员等独立渠道核对指纹。确认后将保存此指纹；密钥变化时会拒绝连接。\n\n是否信任此主机？";
                accepted = MessageBoxW(window_, prompt.c_str(), L"确认 SSH 主机身份",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        const Palette colors = Colors(darkMode_);
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, colors.text);
        if (control == terminal_) {
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
        apiServer_.Stop();
        powerService_.Shutdown();
        connectionCancel_.store(true);
        if (pendingSession_) pendingSession_->connection->CancelStart();
        if (connectionThread_.joinable()) connectionThread_.join();
        pendingSession_.reset();
        StopCommandSequence(false);
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
    titleFont_ = CreateFontW(-Ui::Scale(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, uiFace);
    terminalFont_ = CreateFontW(-terminalFontHeight_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    const wchar_t* toolbarText[] = {L"SSH", L"串口", L"Telnet", L"共享串口"};
    for (int index = 0; index < 4; ++index) {
        toolbarButtons_.push_back(CreateChild(L"BUTTON", toolbarText[index], BS_OWNERDRAW, IdSsh + index));
    }
    cmdButton_ = CreateChild(L"BUTTON", L"CMD", BS_OWNERDRAW, IdCmd);
    powerButton_ = CreateChild(L"BUTTON", L"电源", BS_OWNERDRAW, IdPower);
    apiButton_ = CreateChild(L"BUTTON", L"AI API", BS_OWNERDRAW, IdApi);
    toolbarButtons_.push_back(cmdButton_); toolbarButtons_.push_back(powerButton_); toolbarButtons_.push_back(apiButton_);
    themeButton_ = CreateChild(L"BUTTON", L"浅色", BS_OWNERDRAW, IdTheme);
    disconnectButton_ = CreateChild(L"BUTTON", L"断开", BS_OWNERDRAW, IdDisconnect);

    connectionHeader_ = CreateChild(L"STATIC", L"连接", SS_LEFT, -1);
    SendMessageW(connectionHeader_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
    connectionList_ = CreateChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IdConnectionList);

    terminal_ = CreateChild(L"STATIC", L"终端", SS_NOTIFY | WS_TABSTOP, IdTerminal);
    SendMessageW(terminal_, WM_SETFONT, reinterpret_cast<WPARAM>(terminalFont_), TRUE);
    SetWindowSubclass(terminal_, TerminalSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    powerPane_.Create(window_, uiFont_, darkMode_, &powerService_);
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
}

void MainWindow::LayoutControls(int width, int height) {
    if (width <= 0 || height <= 0) return;
    rightPanelAutoCollapsed_ = width < Ui::MinLeftWidth + Ui::MinRightWidth + Ui::MinCenterWidth + Ui::Gap * 3;
    const int toolbarFrameTop = Ui::Section;
    const int toolbarButtonTop = toolbarFrameTop + 4;
    int x = Ui::Gap + 4;
    const int buttonWidths[] = {68, 76, 80, 106, 78, 84, 98};
    for (size_t index = 0; index < toolbarButtons_.size(); ++index) {
        MoveWindow(toolbarButtons_[index], x, toolbarButtonTop,
            Ui::Scale(buttonWidths[index]), Ui::SegmentHeight, TRUE);
        x += Ui::Scale(buttonWidths[index]);
    }
    MoveWindow(themeButton_, width - Ui::Gap - Ui::StandardHeight,
        (Ui::ToolbarHeight - Ui::StandardHeight) / 2,
        Ui::StandardHeight, Ui::StandardHeight, TRUE);

    const int contentTop = Ui::ToolbarHeight;
    const MainLayoutMetrics layout = GetMainLayoutMetrics(
        width, height, rightPanelWidth_, (rightPanelCollapsed_ || rightPanelAutoCollapsed_));
    const int cardTop = contentTop + Ui::Gap;
    const int centerCardBottom = layout.contentBottom;
    const int sideCardBottom = height - Ui::Gap;
    const int leftInnerLeft = Ui::Gap + Ui::PanelPadding;
    const int leftInnerRight = layout.leftWidth - Ui::PanelPadding;
    const int headerTop = cardTop + Ui::PanelPadding;
    const int leftListTop = headerTop + Ui::StandardHeight + 4;
    const int rightListTop = headerTop + Ui::StandardHeight + 4;

    MoveWindow(connectionHeader_, leftInnerLeft, headerTop + 4,
        leftInnerRight - leftInnerLeft - Ui::Scale(64), Ui::Scale(24), TRUE);
    MoveWindow(disconnectButton_, leftInnerRight - Ui::Scale(56), headerTop,
        Ui::Scale(56), Ui::CompactHeight, TRUE);
    MoveWindow(connectionList_, leftInnerLeft, leftListTop, leftInnerRight - leftInnerLeft,
        std::max(Ui::Scale(20), sideCardBottom - Ui::PanelPadding - leftListTop - (statusHeight_ ? statusHeight_ + Ui::Space : 0)), TRUE);

    MoveWindow(terminal_, layout.centerLeft + Ui::PanelPadding, cardTop + Ui::PanelPadding,
        layout.centerRight - layout.centerLeft - Ui::PanelPadding * 2,
        centerCardBottom - cardTop - Ui::PanelPadding * 2, TRUE);

    MoveWindow(powerPane_.Handle(), layout.centerLeft + Ui::PanelPadding, cardTop + Ui::PanelPadding,
        layout.centerRight - layout.centerLeft - Ui::PanelPadding * 2,
        centerCardBottom - cardTop - Ui::PanelPadding * 2, TRUE);
    ShowWindow(terminal_, powerVisible_ ? SW_HIDE : SW_SHOW);
    ShowWindow(powerPane_.Handle(), powerVisible_ ? SW_SHOW : SW_HIDE);
    const int rightLeft = layout.rightLeft;
    const int rightInnerLeft = rightLeft + Ui::PanelPadding;
    const int actualRightInnerRight = width - Ui::Gap - Ui::PanelPadding;
    const int rightInnerRight = (rightPanelCollapsed_ || rightPanelAutoCollapsed_)
        ? rightInnerLeft + Ui::MinRightWidth - Ui::PanelPadding * 2
        : actualRightInnerRight;
    const int toggleLeft = (rightPanelCollapsed_ || rightPanelAutoCollapsed_)
        ? rightLeft + (layout.rightWidth - Ui::Gap - Ui::IconButtonSize) / 2
        : actualRightInnerRight - Ui::IconButtonSize;
    MoveWindow(rightPanelToggleButton_, toggleLeft, headerTop + 2,
        Ui::IconButtonSize, Ui::IconButtonSize, TRUE);
    MoveWindow(commandHeader_, rightInnerLeft + 2, headerTop + 2,
        Ui::SegmentWidth, Ui::SegmentHeight, TRUE);
    MoveWindow(sftpTabButton_, rightInnerLeft + 2 + Ui::SegmentWidth, headerTop + 2,
        Ui::SegmentWidth, Ui::SegmentHeight, TRUE);
    const int addCommandRight = rightInnerRight - Ui::IconButtonSize - Ui::Space;
    const int addCommandLeftLimit = rightInnerLeft + 4 + Ui::SegmentWidth * 2 + Ui::Space;
    const int addCommandWidth = std::max(Ui::IconButtonSize,
        std::min(104, addCommandRight - addCommandLeftLimit));
    SetWindowTextW(addCommandButton_, addCommandWidth >= 80 ? L"添加命令" : L"");
    MoveWindow(addCommandButton_, addCommandRight - addCommandWidth, headerTop + 2,
        addCommandWidth, Ui::SegmentHeight, TRUE);
    const int footerTop = sideCardBottom - Ui::PanelPadding - Ui::CompactHeight;
    const int listBottom = footerTop - Ui::Gap;
    MoveWindow(commandList_, rightInnerLeft, rightListTop, rightInnerRight - rightInnerLeft,
        std::max(20, listBottom - rightListTop), TRUE);
    const int commandWidth = (rightInnerRight - rightInnerLeft - Ui::Space * 3) / 4;
    int commandX = rightInnerLeft;
    for (HWND button : {importButton_, exportButton_, deleteCommandButton_, saveCommandsButton_}) {
        MoveWindow(button, commandX, footerTop, commandWidth, Ui::CompactHeight, TRUE);
        commandX += commandWidth + Ui::Space;
    }
    const int sftpContentWidth = std::max(1, rightInnerRight - rightInnerLeft);
    MoveWindow(sftpPath_, rightInnerLeft, rightListTop,
        sftpContentWidth, Ui::CompactHeight, TRUE);
    const int sftpHeaderTop = rightListTop + Ui::CompactHeight + Ui::Space;
    const bool showModifiedColumn = sftpContentWidth - Ui::OverlayScrollLaneWidth >= Ui::Scale(300);
    int nameWidth, sizeWidth, modifiedWidth;
    SftpColumnWidths(sftpContentWidth - Ui::OverlayScrollLaneWidth, nameWidth, sizeWidth, modifiedWidth);
    MoveWindow(sftpNameHeader_, rightInnerLeft, sftpHeaderTop,
        nameWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpSizeHeader_, rightInnerLeft + nameWidth, sftpHeaderTop,
        sizeWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpModifiedHeader_, rightInnerLeft + nameWidth + sizeWidth,
        sftpHeaderTop, modifiedWidth, Ui::CompactHeight, TRUE);
    ShowWindow(sftpModifiedHeader_, !(rightPanelCollapsed_ || rightPanelAutoCollapsed_) && sftpPanelVisible_ &&
        showModifiedColumn ? SW_SHOW : SW_HIDE);
    const int sftpListTop = sftpHeaderTop + Ui::CompactHeight;
    const int transferListHeight = sftpTransferExpanded_ ? Ui::SftpTransferDrawerHeight : 0;
    const int transferHeaderTop = footerTop - Ui::Gap - Ui::CompactHeight -
        (sftpTransferExpanded_ ? Ui::Gap + transferListHeight : 0);
    const int sftpListBottom = transferHeaderTop - Ui::Gap;
    MoveWindow(sftpList_, rightInnerLeft, sftpListTop, sftpContentWidth,
        std::max(Ui::SftpTableRowHeight, sftpListBottom - sftpListTop), TRUE);
    const int clearWidth = 64;
    MoveWindow(sftpTransferToggleButton_, rightInnerLeft, transferHeaderTop,
        std::max(80, sftpContentWidth - clearWidth - Ui::Space), Ui::CompactHeight, TRUE);
    MoveWindow(sftpClearTransfersButton_, rightInnerRight - clearWidth, transferHeaderTop,
        clearWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpTransferList_, rightInnerLeft,
        transferHeaderTop + Ui::CompactHeight + Ui::Gap, sftpContentWidth,
        transferListHeight, TRUE);
    ShowWindow(sftpTransferList_, !(rightPanelCollapsed_ || rightPanelAutoCollapsed_) && sftpPanelVisible_ &&
        sftpTransferExpanded_ ? SW_SHOW : SW_HIDE);
    const int sftpWidth = (rightInnerRight - rightInnerLeft - Ui::Space * 3) / 4;
    MoveWindow(sftpUpButton_, rightInnerLeft, footerTop, sftpWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpRefreshButton_, rightInnerLeft + sftpWidth + Ui::Space, footerTop, sftpWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpUploadButton_, rightInnerLeft + (sftpWidth + Ui::Space) * 2, footerTop, sftpWidth, Ui::CompactHeight, TRUE);
    MoveWindow(sftpDownloadButton_, rightInnerLeft + (sftpWidth + Ui::Space) * 3, footerTop,
        rightInnerRight - (rightInnerLeft + (sftpWidth + Ui::Space) * 3), Ui::CompactHeight, TRUE);

    MoveWindow(status_, leftInnerLeft, sideCardBottom - Ui::PanelPadding - statusHeight_,
        leftInnerRight - leftInnerLeft, statusHeight_, TRUE);
    ShowWindow(rightPanelToggleButton_, SW_SHOW);
    if (rightPanelCollapsed_ || rightPanelAutoCollapsed_) {
        for (HWND control : {commandHeader_, sftpTabButton_, commandList_, importButton_, exportButton_,
                 addCommandButton_, deleteCommandButton_, saveCommandsButton_, sftpPath_, sftpNameHeader_, sftpSizeHeader_,
                 sftpModifiedHeader_, sftpList_, sftpTransferToggleButton_, sftpTransferList_,
                 sftpClearTransfersButton_, sftpUpButton_, sftpRefreshButton_, sftpUploadButton_,
                 sftpDownloadButton_})
            ShowWindow(control, SW_HIDE);
    } else {
        ShowWindow(commandHeader_, SW_SHOW);
        ShowWindow(sftpTabButton_, SW_SHOW);
        ShowSftpPanel(sftpPanelVisible_);
    }
    UpdateTerminalDimensions();
}

void MainWindow::PaintWindow(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    const Palette colors = Colors(darkMode_);
    FillRect(dc, &client, windowBrush_);
    RECT toolbar{0, 0, client.right, Ui::ToolbarHeight};
    FillRect(dc, &toolbar, panelBrush_);
    RECT connectionMethodFrame{Ui::Gap, Ui::Section,
        Ui::Gap + Ui::ToolbarGroupWidth + 4, Ui::ToolbarHeight - Ui::Section};
    DrawRoundedBox(dc, connectionMethodFrame, Ui::Radius, colors.panelAlt, colors.border);

    const MainLayoutMetrics layout = GetMainLayoutMetrics(
        client.right, client.bottom, rightPanelWidth_, (rightPanelCollapsed_ || rightPanelAutoCollapsed_));
    RECT leftCard{Ui::Gap, Ui::ToolbarHeight + Ui::Gap, layout.leftWidth, client.bottom - Ui::Gap};
    RECT terminalFrame{layout.centerLeft, Ui::ToolbarHeight + Ui::Gap,
        layout.centerRight, layout.contentBottom};
    RECT rightCard{layout.rightLeft, Ui::ToolbarHeight + Ui::Gap,
        client.right - Ui::Gap, client.bottom - Ui::Gap};
    DrawRoundedBox(dc, leftCard, Ui::CardRadius, colors.panel, colors.border);
    DrawRoundedBox(dc, terminalFrame, Ui::CardRadius, colors.terminal, colors.border);
    DrawRoundedBox(dc, rightCard, Ui::CardRadius, colors.panel, colors.border);
    if (!(rightPanelCollapsed_ || rightPanelAutoCollapsed_)) {
        RECT segmentFrame{layout.rightLeft + Ui::PanelPadding, Ui::ToolbarHeight + Ui::Gap + Ui::PanelPadding,
            layout.rightLeft + Ui::PanelPadding + 4 + Ui::SegmentWidth * 2,
            Ui::ToolbarHeight + Ui::Gap + Ui::PanelPadding + Ui::StandardHeight};
        DrawRoundedBox(dc, segmentFrame, Ui::Radius, colors.panelAlt, colors.border);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
    MoveToEx(dc, 0, Ui::ToolbarHeight, nullptr); LineTo(dc, client.right, Ui::ToolbarHeight);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void MainWindow::DrawOwnerItem(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1) && item.CtlType != ODT_BUTTON &&
        item.CtlType != ODT_COMBOBOX && item.CtlType != ODT_MENU) return;
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
            DrawRoundedBox(dc, highlight, Ui::Radius, colors.accentSoft, colors.accentSoft);
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
            (item.itemState & ODS_DISABLED) ? colors.muted : menuText,
            uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    if (item.CtlType == ODT_COMBOBOX) {
        HBRUSH background = CreateSolidBrush(selected ? colors.accentSoft : colors.terminal);
        FillRect(dc, &rect, background);
        DeleteObject(background);
        int index = static_cast<int>(item.itemID);
        if (index < 0) index = static_cast<int>(SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0));
        std::wstring value;
        if (index >= 0) {
            const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, index, 0);
            if (length >= 0) {
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
                SendMessageW(item.hwndItem, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(buffer.data()));
                value.assign(buffer.data());
            }
        }
        RECT textRect = rect;
        textRect.left += 9;
        textRect.right -= 27;
        DrawTextSimple(dc, value, textRect, colors.text, uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
                    hovered ? colors.accent : colors.text, smallFont_,
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
        const bool focused = (item.itemState & ODS_FOCUS) != 0;
        COLORREF outside = colors.panel;
        if (id == IdCommandTab || id == IdSftpTab) outside = colors.panelAlt;
        if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdApi) outside = colors.panelAlt;
        HBRUSH outsideBrush = CreateSolidBrush(outside);
        FillRect(dc, &rect, outsideBrush);
        DeleteObject(outsideBrush);
        if ((id >= IdSsh && id <= IdShareSerial) || id == IdCmd || id == IdPower || id == IdApi) {
            const bool active = id == IdPower ? powerVisible_ : id == IdCmd ? (!powerVisible_ && selectedMode_ == 4) : id == IdApi ? apiServer_.Running() : (!powerVisible_ && id - IdSsh == selectedMode_);
            const COLORREF fill = active ? colors.panel :
                (hovered ? colors.accentSoft : colors.panelAlt);
            DrawRoundedBox(dc, rect, Ui::Radius, pressed ? colors.accentSoft : fill,
                focused ? colors.accent : (active ? colors.border : fill));
            DrawToolbarIcon(dc, id, rect, active ? colors.accent : colors.muted);
            RECT textRect = rect;
            textRect.left += 34;
            DrawTextSimple(dc, ControlText(item.hwndItem), textRect, active ? colors.text : colors.muted,
                uiFont_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        if (id == IdTheme) {
            DrawRoundedBox(dc, rect, Ui::Radius,
                pressed ? colors.accentSoft : (hovered ? colors.panelAlt : colors.panel),
                focused ? colors.accent : colors.border);
            DrawThemeIcon(dc, rect, darkMode_, colors.text);
            return;
        }
        if (id == IdRightPanelToggle) {
            DrawRoundedBox(dc, rect, Ui::Radius,
                pressed ? colors.accentSoft : (hovered ? colors.accentSoft : colors.panelAlt),
                focused ? colors.accent : colors.border);
            DrawChevronHorizontal(dc, rect, enabled ? colors.text : colors.muted,
                rightPanelCollapsed_ || rightPanelAutoCollapsed_);
            return;
        }
        if (id == IdDisconnect) {
            const COLORREF fill = enabled ? colors.panelAlt : colors.panel;
            DrawRoundedBox(dc, rect, Ui::Radius,
                pressed ? colors.accentSoft : (hovered && enabled ? colors.accentSoft : fill),
                focused ? colors.accent : colors.border);
            DrawTextSimple(dc, L"断开", rect, enabled ? colors.danger : colors.muted,
                smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        if (id == IdCommandTab || id == IdSftpTab) {
            const bool active = (id == IdSftpTab) == sftpPanelVisible_;
            const COLORREF fill = active ? colors.panel :
                (hovered && enabled ? colors.accentSoft : colors.panelAlt);
            DrawRoundedBox(dc, rect, Ui::Radius, pressed ? colors.accentSoft : fill,
                focused ? colors.accent : (active ? colors.border : fill));
            DrawTextSimple(dc, ControlText(item.hwndItem), rect,
                active ? colors.accent : (enabled ? colors.text : colors.muted),
                uiFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
                active ? colors.accent : colors.muted, smallFont_,
                id == IdSftpNameHeader ? DT_LEFT | DT_VCENTER | DT_SINGLELINE :
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            if (active) DrawSortChevron(dc, rect, colors.accent, sftpSortAscending_);
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
        if (id == IdAddCommand || id == IDC_COMMAND_ADD_STEP) {
            DrawRoundedBox(dc, rect, Ui::Radius,
                pressed ? colors.accentSoft : (hovered ? colors.accentSoft : colors.panel),
                focused ? colors.accent : colors.panel);
            DrawPlusIcon(dc, rect, colors.accent);
            RECT textRect = rect;
            textRect.left += 28;
            textRect.right -= 8;
            DrawTextSimple(dc, ControlText(item.hwndItem), textRect, colors.accent,
                uiFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }

        COLORREF fill = pressed ? colors.accentSoft : (hovered ? colors.accentSoft : colors.panelAlt);
        COLORREF border = focused ? colors.accent : colors.border;
        if (id == IDOK) {
            fill = enabled ? (pressed ? RGB(0, 94, 204) :
                (hovered ? RGB(38, 147, 255) : colors.accent)) : colors.panelAlt;
            border = fill;
        }
        if (!enabled) fill = colors.panel;
        DrawRoundedBox(dc, rect, Ui::Radius, fill, border);
        DrawTextSimple(dc, ControlText(item.hwndItem), rect,
            enabled ? (id == IDOK ? RGB(255, 255, 255) : colors.text) : colors.muted,
            uiFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    HBRUSH background = CreateSolidBrush(colors.panel);
    FillRect(dc, &rect, background);
    DeleteObject(background);

    if (item.CtlID == IdConnectionList) {
        const bool valid = item.itemID < sessions_.size();
        const SessionState* session = valid ? sessions_[item.itemID].get() : nullptr;
        const bool connected = session && session->connection && session->connection->IsConnected();
        const bool active = session == activeSession_;
        RECT card{rect.left, rect.top + 4, rect.right, rect.bottom - 4};
        DrawRoundedBox(dc, card, Ui::CardRadius, active ? colors.accentSoft : colors.panelAlt,
            active ? colors.accent : colors.panelAlt);
        HBRUSH dot = CreateSolidBrush(connected ? colors.accent : colors.muted);
        RECT dotRect{card.left + 14, card.top + 17, card.left + 22, card.top + 25};
        HBRUSH oldDot = reinterpret_cast<HBRUSH>(SelectObject(dc, dot));
        Ellipse(dc, dotRect.left, dotRect.top, dotRect.right, dotRect.bottom);
        SelectObject(dc, oldDot);
        DeleteObject(dot);
        RECT title{card.left + 32, card.top + 6, card.right - 8, card.top + 27};
        RECT subtitle{card.left + 32, card.top + 27, card.right - 8, card.bottom - 4};
        DrawTextSimple(dc, session ? session->name : L"暂无连接", title, colors.text, uiFont_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring state = connected ? L"● 已连接" : L"○ 已断开";
        COLORREF stateColor = colors.muted;
        if (connected) {
            state = L"● 已连接";
            stateColor = colors.accent;
        }
        if (connected && session && session->mode == 1) {
            state = session->port ? L"TCP " + std::to_wstring(session->port) + L" · 共享" : L"本地可用 · 未共享";
            if (!session->port) stateColor = colors.danger;
        }
        DrawTextSimple(dc, state, subtitle, stateColor, smallFont_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (item.CtlID == IdCommandList && item.itemID < commands_.size()) {
        const CommandItem& command = commands_[item.itemID];
        OverlayScrollMetrics scrollMetrics;
        const int rightInset = GetOverlayScrollMetrics(item.hwndItem, scrollMetrics) ?
            Ui::OverlayScrollLaneWidth : 0;
        RECT card{rect.left, rect.top + 4, rect.right - rightInset, rect.bottom - 4};
        DrawRoundedBox(dc, card, Ui::CardRadius, selected ? colors.accentSoft : colors.panelAlt,
            selected ? colors.accent : colors.panelAlt);
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 2; ++col) {
                HBRUSH dotBrush = CreateSolidBrush(colors.muted);
                HBRUSH oldDotBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, dotBrush));
                Ellipse(dc, card.left + 12 + col * 5, card.top + 20 + row * 5,
                    card.left + 14 + col * 5, card.top + 22 + row * 5);
                SelectObject(dc, oldDotBrush);
                DeleteObject(dotBrush);
            }
        }
        const CommandActionRects actions = GetCommandActionRects(card);
        RECT title{card.left + 32, card.top + 9, actions.run.left - Ui::Space, card.top + 30};
        RECT value{card.left + 32, card.top + 31, actions.run.left - Ui::Space, card.bottom - 6};
        DrawTextSimple(dc, command.name, title, colors.text, uiFont_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        const bool macro = command.commands.size() > 1;
        const std::wstring summary = macro ?
            L"宏 · " + std::to_wstring(command.commands.size()) + L" 条指令 · " +
                std::to_wstring(command.intervalMs) + L" ms" :
            (command.commands.empty() ? std::wstring() : command.commands.front());
        DrawTextSimple(dc, summary, value, colors.muted, smallFont_,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        const bool connected = connection_ && connection_->IsConnected();
        const bool sequenceRunning = runningCommandIndex_ >= 0;
        const bool isRunning = runningCommandIndex_ == static_cast<int>(item.itemID);
        const bool runnable = connected && (!sequenceRunning || isRunning);
        const bool editable = !sequenceRunning;
        const bool hoverRun = hoveredCommandIndex_ == static_cast<int>(item.itemID) && hoveredCommandAction_ == 1;
        const bool hoverEdit = hoveredCommandIndex_ == static_cast<int>(item.itemID) && hoveredCommandAction_ == 2;
        const COLORREF cardFill = selected ? colors.accentSoft : colors.panelAlt;
        DrawRoundedBox(dc, actions.run, Ui::Radius,
            hoverRun && runnable ? colors.accentSoft : cardFill,
            hoverRun && runnable ? colors.accent : cardFill);
        DrawRoundedBox(dc, actions.edit, Ui::Radius,
            hoverEdit && editable ? colors.accentSoft : cardFill,
            hoverEdit && editable ? colors.accent : cardFill);

        const int runCx = (actions.run.left + actions.run.right) / 2;
        const int runCy = (actions.run.top + actions.run.bottom) / 2;
        const COLORREF runColor = runnable ? colors.accent : colors.muted;
        HBRUSH runBrush = CreateSolidBrush(runColor);
        HPEN runPen = CreatePen(PS_SOLID, 1, runColor);
        HBRUSH oldRunBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, runBrush));
        HPEN oldRunPen = reinterpret_cast<HPEN>(SelectObject(dc, runPen));
        if (isRunning) {
            Rectangle(dc, runCx - 4, runCy - 4, runCx + 5, runCy + 5);
        } else {
            POINT triangle[] = {{runCx - 4, runCy - 6}, {runCx + 6, runCy}, {runCx - 4, runCy + 6}};
            Polygon(dc, triangle, 3);
        }
        SelectObject(dc, oldRunPen); SelectObject(dc, oldRunBrush);
        DeleteObject(runPen); DeleteObject(runBrush);
        if (isRunning)
            DrawCommandProgress(dc, actions.run, runningCommandStep_, command.commands.size(),
                colors.border, colors.accent);
        DrawEditIcon(dc, actions.edit,
            editable ? (hoverEdit ? colors.accent : colors.muted) : colors.border);
    } else if (item.CtlID == IdSftpList && item.itemID < sftpEntries_.size()) {
        const SftpEntry& entry = sftpEntries_[item.itemID];
        OverlayScrollMetrics scrollMetrics;
        const int rightInset = Ui::OverlayScrollLaneWidth;
        RECT row{rect.left, rect.top, rect.right - rightInset, rect.bottom};
        HBRUSH rowBrush = CreateSolidBrush(selected ? colors.accentSoft : colors.panel);
        FillRect(dc, &row, rowBrush);
        DeleteObject(rowBrush);
        RECT tableClient{}; GetClientRect(sftpList_, &tableClient);
        const int contentWidth = std::max(1, static_cast<int>(tableClient.right) - rightInset);
        const bool showModified = contentWidth >= 300;
        int nameWidth, sizeWidth, modifiedWidth;
        SftpColumnWidths(contentWidth, nameWidth, sizeWidth, modifiedWidth);
        RECT icon{row.left + Ui::Space, row.top, row.left + 32, row.bottom};
        DrawSftpEntryIcon(dc, icon, entry.directory ? colors.accent : colors.muted,
            entry.directory, entry.symlink);
        RECT name{row.left + 36, row.top, row.left + nameWidth - Ui::Space, row.bottom};
        DrawTextSimple(dc, entry.name, name, colors.text, uiFont_,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!entry.directory) {
            RECT size{row.left + nameWidth, row.top,
                row.left + nameWidth + sizeWidth - Ui::Space, row.bottom};
            DrawTextSimple(dc, FileSizeText(entry.size), size, colors.muted, smallFont_,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (showModified) {
            RECT modified{row.left + nameWidth + sizeWidth, row.top,
                row.right - Ui::Space, row.bottom};
            DrawTextSimple(dc, entry.modifiedText, modified, colors.muted, smallFont_,
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
    for (HWND control : {window_, connectionHeader_, connectionList_, terminal_, status_, commandList_, sftpPath_, sftpList_, sftpTransferList_}) {
        InvalidateRect(control, nullptr, TRUE);
    }
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);
    for (HWND button : {themeButton_, disconnectButton_, saveCommandsButton_, commandHeader_, sftpTabButton_,
             rightPanelToggleButton_, importButton_, exportButton_, addCommandButton_, deleteCommandButton_,
             sftpNameHeader_, sftpSizeHeader_, sftpModifiedHeader_, sftpTransferToggleButton_,
             sftpClearTransfersButton_, sftpUpButton_, sftpRefreshButton_, sftpUploadButton_,
             sftpDownloadButton_})
        InvalidateRect(button, nullptr, TRUE);
    UpdateWindow(window_);
}

void MainWindow::OpenConnectionDialog(int mode) {
    if (pendingSession_) {
        AppendStatus(L"正在连接，请等待当前连接完成。", false);
        return;
    }
    pendingMode_ = mode;
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_CONNECTION), window_, ConnectionDialogProc,
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
                if (_wcsicmp(className, L"Edit") == 0)
                    PrepareDialogEditField(dialog, child, self);
                else
                    PrepareDialogField(child);
                SetWindowSubclass(child, DialogControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(self));
                if (_wcsicmp(className, WC_COMBOBOXW) == 0) {
                    COMBOBOXINFO comboInfo{sizeof(comboInfo)};
                    if (GetComboBoxInfo(child, &comboInfo) && comboInfo.hwndList) {
                        const LONG_PTR listStyle = GetWindowLongPtrW(comboInfo.hwndList, GWL_STYLE) &
                            ~static_cast<LONG_PTR>(WS_VSCROLL | WS_BORDER);
                        const LONG_PTR listExStyle = GetWindowLongPtrW(comboInfo.hwndList, GWL_EXSTYLE) &
                            ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
                        SetWindowLongPtrW(comboInfo.hwndList, GWL_STYLE, listStyle);
                        SetWindowLongPtrW(comboInfo.hwndList, GWL_EXSTYLE, listExStyle);
                        SetWindowSubclass(comboInfo.hwndList, OverlayListSubclassProc, 2,
                            reinterpret_cast<DWORD_PTR>(self));
                        ApplyRoundedControlRegion(comboInfo.hwndList);
                    }
                }
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
        SetTextColor(dc, GetDlgCtrlID(control) == IDC_DIALOG_ERROR ? colors.danger : colors.text);
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
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 24;
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
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message == WM_COMMAND) {
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
        if (LOWORD(wParam) == IDOK) {
            std::wstring error;
            if (!self->ReadConnectionDialog(dialog, error)) {
                SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, error.c_str());
                InvalidateRect(GetDlgItem(dialog, IDC_DIALOG_ERROR), nullptr, TRUE);
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

void MainWindow::ConfigureConnectionDialog(HWND dialog) {
    const bool serial = pendingMode_ == 1;
    const bool ssh = pendingMode_ == 0;
    const bool share = pendingMode_ == 3;
    discoveryExplicitPort_ = 0;
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
        MoveDialogItemDlu(dialog, IDC_SERIAL, margin, 22, 61, 154);
        MoveDialogItemDlu(dialog, IDC_SERIAL_REFRESH, 81, 22, 32, 14);
        MoveDialogItemDlu(dialog, IDC_BAUD_LABEL, rightColumn, 10, 64, 10);
        MoveDialogItemDlu(dialog, IDC_BAUD, rightColumn, 22, columnWidth, 140);
        MoveDialogItemDlu(dialog, IDC_DATABITS_LABEL, margin, 44, 64, 10);
        MoveDialogItemDlu(dialog, IDC_DATABITS, margin, 56, columnWidth, 84);
        MoveDialogItemDlu(dialog, IDC_PARITY_LABEL, rightColumn, 44, 64, 10);
        MoveDialogItemDlu(dialog, IDC_PARITY, rightColumn, 56, columnWidth, 72);
        MoveDialogItemDlu(dialog, IDC_STOPBITS_LABEL, margin, 78, 64, 10);
        MoveDialogItemDlu(dialog, IDC_STOPBITS, margin, 90, columnWidth, 72);
        MoveDialogItemDlu(dialog, IDC_FLOW_LABEL, rightColumn, 78, 64, 10);
        MoveDialogItemDlu(dialog, IDC_FLOW, rightColumn, 90, columnWidth, 72);
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
            SetDlgItemTextW(dialog, IDC_HOST_LABEL, L"来源 IP");
            MoveDialogItemDlu(dialog, IDC_HOST, margin, 22, fullWidth, 14);
            SetDlgItemTextW(dialog, IDC_SERIAL_LABEL, L"远端串口");
            MoveDialogItemDlu(dialog, IDC_SERIAL_LABEL, margin, 44, columnWidth, 10);
            MoveDialogItemDlu(dialog, IDC_SERIAL, margin, 56, 174, 154);
            MoveDialogItemDlu(dialog, IDC_SERIAL_REFRESH, 194, 56, 32, 14);
        }
    }

    int errorY = 106;
    int buttonY = 118;
    int dialogHeight = 144;
    if (share || (!serial && ssh)) {
        errorY = 72;
        buttonY = 84;
        dialogHeight = 110;
    } else if (!serial) {
        errorY = 38;
        buttonY = 50;
        dialogHeight = 76;
    }
    MoveDialogItemDlu(dialog, IDC_DIALOG_ERROR, margin, errorY, fullWidth, 12);
    MoveDialogItemDlu(dialog, IDCANCEL, dialogWidth - 156, buttonY, 68, 18);
    MoveDialogItemDlu(dialog, IDOK, dialogWidth - 80, buttonY, 68, 18);
    ResizeDialogClientDlu(dialog, dialogWidth, dialogHeight);
    if (serial || share) MatchDialogControlHeight(dialog, IDC_SERIAL, IDC_SERIAL_REFRESH);
    FitDialogLabels(dialog);

    const wchar_t* titles[] = {L"新建 SSH 连接", L"新建串口连接", L"新建 Telnet 连接", L"连接共享串口"};
    SetWindowTextW(dialog, titles[pendingMode_]);
    ShowWindow(GetDlgItem(dialog, IDC_DIALOG_NOTE), SW_HIDE);
    SetDlgItemTextW(dialog, IDC_HOST, L"127.0.0.1");
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
}

void MainWindow::RefreshSerialPorts(HWND dialog) {
    HWND ports = GetDlgItem(dialog, IDC_SERIAL);
    SendMessageW(ports, CB_RESETCONTENT, 0, 0);
    int count = 0;
    for (int index = 1; index <= 256; ++index) {
        const std::wstring name = L"COM" + std::to_wstring(index);
        wchar_t target[256]{};
        if (QueryDosDeviceW(name.c_str(), target, static_cast<DWORD>(std::size(target)))) {
            SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            ++count;
        }
    }
    if (count == 0) {
        SendMessageW(ports, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未检测到串口"));
    }
    SetDlgItemTextW(dialog, IDC_DIALOG_ERROR, L"");
    SendMessageW(ports, CB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), !((pendingMode_ == 1 || pendingMode_ == 3) && count == 0));
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
        auto result = std::make_unique<DiscoveryMessage>(); result->dialog = dialog; result->generation = generation;
        try { std::vector<std::wstring> descriptions; result->success = SharedSerialConnection::DiscoverAuto(host, result->port, result->names, result->error, &discoveryCancel_, explicitPort, &descriptions);
            if (result->success) result->names = std::move(descriptions); }
        catch (...) { result->error = L"串口查询失败"; }
        discoveryFinished_ = true;
        if (PostMessageW(window_, MessageDiscovery, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    });
}

bool MainWindow::ReadConnectionDialog(HWND dialog, std::wstring& error) {
    auto text = [dialog](int id) { return Trim(ControlText(GetDlgItem(dialog, id))); };
    if (pendingMode_ == 1) {
        pendingSerial_.portName = text(IDC_SERIAL);
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
            [this](const std::wstring& host, std::uint16_t port, const std::wstring& fingerprint) {
                if (connectionCancel_.load()) return false;
                auto request = std::make_shared<HostKeyRequest>();
                request->host = host;
                request->port = port;
                request->fingerprint = fingerprint;
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
            });
        sessionName = pendingUsername_ + L"@" + pendingHost_;
    } else if (pendingMode_ == 1) {
        connection = std::make_unique<SerialShareConnection>(pendingSerial_, DefaultSharePort);
        sessionName = pendingSerial_.portName + L" 本地串口";
    } else if (pendingMode_ == 2) {
        connection = std::make_unique<TcpConnection>(pendingHost_, pendingPort_, true);
        sessionName = pendingHost_ + L":" + std::to_wstring(pendingPort_);
    } else if (pendingMode_ == 4) {
        connection = std::make_unique<CmdConnection>(); sessionName = L"本地 CMD";
    } else {
        connection = std::make_unique<SharedSerialConnection>(pendingHost_, pendingPort_, pendingRemoteSerial_);
        sessionName = pendingRemoteSerial_ + L" @ " + pendingHost_;
    }

    auto session = std::make_unique<SessionState>();
    session->terminal.Resize(terminalVisibleColumns_, terminalVisibleRows_);
    session->id = nextSessionId_++;
    session->mode = pendingMode_;
    if (pendingMode_ == 4) { session->codePage = GetOEMCP(); session->lineEndingIndex = 2; }
    session->lineEndingIndex = pendingMode_ == 0 ? 1 : (pendingMode_ == 4 ? 2 : 0);
    session->name = sessionName;
    session->host = pendingHost_;
    session->username = pendingUsername_;
    session->password = pendingPassword_;
    session->port = pendingPort_;
    session->logger = std::make_unique<SessionLogger>();
    const std::uint64_t sessionId = session->id;

    session->connection = std::make_unique<QueuedConnection>(std::move(connection));
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
                        [this, sessionId](const Bytes& data) { PostData(sessionId, data); },
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
        pendingSession_.reset();
        AppendStatus(L"无法启动连接工作线程。", true);
    }
}

void MainWindow::CompleteConnection(bool success, const std::wstring& error) {
    if (connectionThread_.joinable()) connectionThread_.join();
    if (!pendingSession_) return;
    if (!success || closing_ || connectionCancel_.load()) {
        pendingSession_.reset();
        pendingConnectionData_.clear();
        pendingConnectionBytes_ = 0;
        if (!closing_) AppendStatus(error.empty() ? L"连接已取消。" : error, true);
        return;
    }
    if (pendingSession_->mode == 1) pendingSession_->port = static_cast<SerialShareConnection*>(static_cast<QueuedConnection*>(pendingSession_->connection.get())->Transport())->SharedPort();
    const std::uint64_t id = pendingSession_->id;
    sessions_.push_back(std::move(pendingSession_));
    RefreshConnectionList();
    SwitchSession(sessions_.size() - 1);
    for (const Bytes& data : pendingConnectionData_) AppendData(id, data);
    pendingConnectionData_.clear();
    pendingConnectionBytes_ = 0;
    if (activeSession_->mode == 1) {
        AppendStatus(activeSession_->port ? L"已连接：" + activeSession_->name + L" · 共享 TCP " + std::to_wstring(activeSession_->port) :
            L"本地串口已连接；7000–7015 均不可用，TCP 共享未启用", activeSession_->port == 0);
    } else AppendStatus(L"已连接：" + activeSession_->name, false);
    SetFocus(terminal_);
}

void MainWindow::Disconnect() {
    if (!activeSession_) return;
    StopCommandSequence(false);
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
    (*found)->connection->Stop();
    (*found)->logger->Stop();
    sessions_.erase(found);
    activeSession_ = nullptr;
    connection_ = nullptr;
    logger_ = nullptr;
    activeConnectionName_.clear();
    selectedMode_ = -1;
    RefreshConnectionList();
    if (!sessions_.empty()) SwitchSession(std::min(index, sessions_.size() - 1));
    else {
        ClearTerminalSelection();
        InvalidateRect(terminal_, nullptr, TRUE);
        ShowSftpPanel(false);
        sftpEntries_.clear();
        sftpDirectory_ = L".";
        SetConnectedUi(false);
    }
    AppendStatus(L"已断开", false);
}

MainWindow::SessionState* MainWindow::FindSession(std::uint64_t id) {
    const auto found = std::find_if(sessions_.begin(), sessions_.end(),
        [id](const std::unique_ptr<SessionState>& item) { return item->id == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

void MainWindow::SwitchSession(size_t index) {
    if (index >= sessions_.size()) return;
    powerVisible_ = false; ShowWindow(powerPane_.Handle(), SW_HIDE); ShowWindow(terminal_, SW_SHOW);
    if (activeSession_ && activeSession_ != sessions_[index].get())
        StopCommandSequence(false);
    activeSession_ = sessions_[index].get();
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
    SendMessageW(connectionList_, LB_SETCURSEL, index, 0);
    SetConnectedUi(connection_ && connection_->IsConnected());
    if (selectedMode_ == 0 && SftpClient::IsAvailable()) {
        ShowSftpPanel(true);
        RefreshSftpList();
        if (!sftpBusy_) RefreshSftp();
    } else {
        ShowSftpPanel(false);
    }
    RefreshConnectionList();
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);
    SetFocus(terminal_);
}

bool MainWindow::SendBytesToActive(const Bytes& data, bool localEcho, const std::wstring& echoedText) {
    if (!activeSession_ || !connection_ || !connection_->IsConnected()) {
        AppendStatus(L"请先建立连接。", true);
        return false;
    }
    std::wstring error;
    if (!connection_->Send(data, error)) {
        AppendStatus(error, true);
        return false;
    }
    apiServer_.Publish("session-" + std::to_string(activeSession_->id), data, "gui", "input");
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
    std::wstring text;
    if (character == L'\r') text = selectedMode_ == 0 ? L"\r" : SelectedLineEnding();
    else if (character == L'\b' && selectedMode_ == 0) text.assign(1, static_cast<wchar_t>(0x7f));
    else text.assign(1, character);
    const std::string encoded = WideToMultiByte(text, SelectedCodePage());
    if (encoded.empty() && character != 0) return;
    SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text);
}

void MainWindow::SendTerminalKey(WPARAM key, bool shift, bool control, bool alt) {
    if (runningCommandIndex_ >= 0) return;
    if (!activeSession_) return;
    std::string sequence;
    const int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (control ? 4 : 0);
    const bool modified = modifier != 1;
    const bool application = activeSession_->terminal.ApplicationCursorKeys() && !modified;
    const auto cursorSequence = [&](char finalCharacter) {
        if (application) return std::string("\x1bO") + finalCharacter;
        if (modified) return std::string("\x1b[1;") + std::to_string(modifier) + finalCharacter;
        return std::string("\x1b[") + finalCharacter;
    };
    switch (key) {
    case VK_UP: sequence = cursorSequence('A'); break;
    case VK_DOWN: sequence = cursorSequence('B'); break;
    case VK_RIGHT: sequence = cursorSequence('C'); break;
    case VK_LEFT: sequence = cursorSequence('D'); break;
    case VK_HOME: sequence = cursorSequence('H'); break;
    case VK_END: sequence = cursorSequence('F'); break;
    case VK_INSERT: sequence = modified ? "\x1b[2;" + std::to_string(modifier) + "~" : "\x1b[2~"; break;
    case VK_DELETE: sequence = modified ? "\x1b[3;" + std::to_string(modifier) + "~" : "\x1b[3~"; break;
    case VK_PRIOR: sequence = modified ? "\x1b[5;" + std::to_string(modifier) + "~" : "\x1b[5~"; break;
    case VK_NEXT: sequence = modified ? "\x1b[6;" + std::to_string(modifier) + "~" : "\x1b[6~"; break;
    case VK_ESCAPE: sequence = "\x1b"; break;
    case VK_TAB: sequence = shift ? "\x1b[Z" : "\t"; break;
    case VK_F1: sequence = "\x1bOP"; break;
    case VK_F2: sequence = "\x1bOQ"; break;
    case VK_F3: sequence = "\x1bOR"; break;
    case VK_F4: sequence = "\x1bOS"; break;
    case VK_F5: sequence = "\x1b[15~"; break;
    case VK_F6: sequence = "\x1b[17~"; break;
    case VK_F7: sequence = "\x1b[18~"; break;
    case VK_F8: sequence = "\x1b[19~"; break;
    case VK_F9: sequence = "\x1b[20~"; break;
    case VK_F10: sequence = "\x1b[21~"; break;
    case VK_F11: sequence = "\x1b[23~"; break;
    case VK_F12: sequence = "\x1b[24~"; break;
    default: return;
    }
    SendBytesToActive(Bytes(sequence.begin(), sequence.end()), false);
}

void MainWindow::SendCommand(size_t index) {
    if (index >= commands_.size() || commands_[index].commands.empty() || runningCommandIndex_ >= 0) return;
    if (!connection_ || !connection_->IsConnected() || !activeSession_) {
        AppendStatus(L"请先建立连接，再执行常用命令。", true);
        return;
    }
    const CommandItem& command = commands_[index];
    if (command.commands.size() == 1) {
        const std::wstring text = command.commands.front() + SelectedLineEnding();
        const std::string encoded = WideToMultiByte(text, SelectedCodePage());
        if (SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text))
            AppendStatus(L"已发送命令：" + command.commands.front(), false);
        return;
    }

    runningCommandIndex_ = static_cast<int>(index);
    runningCommandStep_ = 0;
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
        !connection_->IsConnected() || runningCommandStep_ >= command.commands.size()) {
        StopCommandSequence(false);
        return;
    }

    const std::wstring& step = command.commands[runningCommandStep_];
    const std::wstring text = step + runningCommandLineEnding_;
    const std::string encoded = WideToMultiByte(text, runningCommandCodePage_);
    if (!SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, text)) {
        StopCommandSequence(false);
        return;
    }

    ++runningCommandStep_;
    InvalidateRect(commandList_, nullptr, FALSE);
    if (runningCommandStep_ >= command.commands.size()) {
        const std::wstring name = command.name;
        StopCommandSequence(false);
        AppendStatus(L"宏“" + name + L"”执行完成。", false);
        return;
    }

    AppendStatus(L"正在执行宏“" + command.name + L"”（" +
        std::to_wstring(runningCommandStep_) + L"/" + std::to_wstring(command.commands.size()) +
        L"）：" + step, false);
    KillTimer(window_, StatusTimerId);
    if (!SetTimer(window_, CommandTimerId, command.intervalMs, nullptr)) {
        StopCommandSequence(false);
        AppendStatus(L"无法继续执行宏，计时器创建失败。", true);
    }
}

void MainWindow::StopCommandSequence(bool showStatus) {
    if (runningCommandIndex_ < 0) return;
    std::wstring name;
    if (runningCommandIndex_ < static_cast<int>(commands_.size()))
        name = commands_[static_cast<size_t>(runningCommandIndex_)].name;
    KillTimer(window_, CommandTimerId);
    runningCommandIndex_ = -1;
    runningCommandStep_ = 0;
    runningCommandSessionId_ = 0;
    runningCommandLineEnding_.clear();
    UpdateCommandActions();
    InvalidateRect(commandList_, nullptr, FALSE);
    if (showStatus) AppendStatus(L"宏“" + name + L"”已停止。", false);
}

void MainWindow::PostData(std::uint64_t sessionId, const Bytes& data) {
    apiServer_.Publish("session-" + std::to_string(sessionId), data);
    std::lock_guard<std::mutex> lock(receivedMutex_);
    Bytes& pending = received_[sessionId];
    if (data.size() + pending.size() > 4 * 1024 * 1024 || received_.size() > 64) receiveOverflow_.insert(sessionId);
    else pending.insert(pending.end(), data.begin(), data.end());
    if (!receivePosted_) {
        receivePosted_ = PostMessageW(window_, MessageData, 0, 0) != FALSE;
        if (!receivePosted_) { received_.clear(); receiveOverflow_.insert(sessionId); }
    }
}

void MainWindow::PostStatus(std::uint64_t sessionId, const std::wstring& text, bool isError) {
    auto* status = new StatusMessage{sessionId, text, isError};
    if (!PostMessageW(window_, MessageStatus, 0, reinterpret_cast<LPARAM>(status))) delete status;
}

void MainWindow::AppendData(std::uint64_t sessionId, const Bytes& data) {
    SessionState* session = FindSession(sessionId);
    if (!session) return;
    const std::wstring decoded = DecodeTerminalData(*session, data);
    const std::wstring timestamp = timestampEnabled_ ? CurrentTerminalTimestamp() : std::wstring();
    const bool wasAlternateScreen = session->terminal.AlternateScreen();
    const TerminalFeedResult result = session->terminal.Feed(decoded, timestamp);
    if (session->logger && !result.logText.empty()) session->logger->WriteText(result.logText);
    if (!result.response.empty() && session->connection && session->connection->IsConnected()) {
        std::wstring ignored;
        session->connection->Send(Bytes(result.response.begin(), result.response.end()), ignored);
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
    session.pendingDecodeBytes.insert(session.pendingDecodeBytes.end(), data.begin(), data.end());
    if (session.pendingDecodeBytes.empty()) return {};

    size_t pendingTail = 0;
    const UINT codePage = session.codePage == 20936 && !IsValidCodePage(20936) ? 936 : session.codePage;
    if (codePage == CP_UTF8) {
        const Bytes& bytes = session.pendingDecodeBytes;
        size_t lead = bytes.size();
        while (lead > 0 && (bytes[lead - 1] & 0xC0) == 0x80 && bytes.size() - lead < 3) --lead;
        if (lead > 0) {
            const size_t index = lead - 1;
            const std::uint8_t first = bytes[index];
            size_t expected = 1;
            if ((first & 0xE0) == 0xC0) expected = 2;
            else if ((first & 0xF0) == 0xE0) expected = 3;
            else if ((first & 0xF8) == 0xF0) expected = 4;
            if (expected > 1 && bytes.size() - index < expected) pendingTail = bytes.size() - index;
        }
    } else {
        pendingTail = IncompleteDbcsTail(session.pendingDecodeBytes,
            [codePage](std::uint8_t value) { return IsDBCSLeadByteEx(codePage, value) != FALSE; });
    }

    const size_t decodeSize = session.pendingDecodeBytes.size() - pendingTail;
    std::wstring decoded;
    if (decodeSize)
        decoded = MultiByteToWide(session.pendingDecodeBytes.data(), decodeSize, codePage);
    Bytes tail;
    if (pendingTail)
        tail.assign(session.pendingDecodeBytes.end() - static_cast<std::ptrdiff_t>(pendingTail),
            session.pendingDecodeBytes.end());
    session.pendingDecodeBytes = std::move(tail);
    return decoded;
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
        const auto layout = GetMainLayoutMetrics(client.right,client.bottom,rightPanelWidth_,(rightPanelCollapsed_ || rightPanelAutoCollapsed_));
        HDC dc=GetDC(status_); auto old=SelectObject(dc,smallFont_);
        RECT measured{0,0,layout.leftWidth-Ui::Gap-Ui::PanelPadding*2,0};
        DrawTextW(dc,text.c_str(),-1,&measured,DT_WORDBREAK|DT_CALCRECT);
        SelectObject(dc,old);ReleaseDC(status_,dc);
        int height = text.empty() ? 0 : std::clamp(static_cast<int>(measured.bottom),Ui::Scale(20),Ui::Scale(120));
        statusHeight_ = height;
        MoveWindow(status_,Ui::Gap+Ui::PanelPadding,client.bottom-Ui::Gap-Ui::PanelPadding-height,
            measured.right,height,TRUE);
        RECT list{};GetWindowRect(connectionList_,&list);MapWindowPoints(HWND_DESKTOP,window_,reinterpret_cast<POINT*>(&list),2);
        MoveWindow(connectionList_,list.left,list.top,list.right-list.left,
            std::max(Ui::Scale(20),static_cast<int>(client.bottom)-Ui::Gap-Ui::PanelPadding-static_cast<int>(list.top)-(height?height+Ui::Space:0)),TRUE);
    }
    ShowWindow(status_, text.empty() ? SW_HIDE : SW_SHOW);
    InvalidateRect(status_, nullptr, TRUE);
    KillTimer(window_, StatusTimerId);
    if (!text.empty() && !isError) SetTimer(window_, StatusTimerId, 3500, nullptr);
    if (logger_ && !text.empty()) logger_->WriteStatus(text);
}

void MainWindow::SaveCurrentLog() {
    if (!logger_ || logger_->Path().empty()) {
        AppendStatus(L"当前没有可保存的会话日志。", true);
        return;
    }
    wchar_t path[MAX_PATH] = L"SerialCtl-会话日志.log";
    const size_t slash = logger_->Path().find_last_of(L"\\/");
    const std::wstring fileName = slash == std::wstring::npos ? logger_->Path() : logger_->Path().substr(slash + 1);
    if (!fileName.empty() && fileName.size() < std::size(path)) wcscpy_s(path, fileName.c_str());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"日志文件 (*.log)\0*.log\0文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"log";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;
    std::wstring error;
    if (!logger_->SaveCopy(path, error)) {
        AppendStatus(error, true);
        return;
    }
    AppendStatus(L"日志已保存", false);
}

void MainWindow::ShowTerminalContextMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HBRUSH menuBrush = CreateSolidBrush(Colors(darkMode_).panel);
    MENUINFO menuInfo{sizeof(menuInfo)};
    menuInfo.fMask = MIM_BACKGROUND;
    menuInfo.hbrBack = menuBrush;
    SetMenuInfo(menu, &menuInfo);

    static const wchar_t localEchoLabel[] = L"本地回显";
    static const wchar_t timestampLabel[] = L"显示毫秒时间戳";
    static const wchar_t saveLabel[] = L"保存当前日志…";
    static const wchar_t clearLabel[] = L"清空终端";
    static const wchar_t copyLabel[] = L"复制";
    static const wchar_t pasteLabel[] = L"粘贴";
    static const wchar_t selectAllLabel[] = L"全选";
    static const wchar_t encodingUtf8Label[] = L"编码 · UTF-8";
    static const wchar_t encodingGbkLabel[] = L"编码 · GBK";
    static const wchar_t encodingGb2312Label[] = L"编码 · GB2312";
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuLocalEcho, localEchoLabel);
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuTimestamp, timestampLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuEncodingUtf8, encodingUtf8Label);
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuEncodingGbk, encodingGbkLabel);
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuEncodingGb2312, encodingGb2312Label);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (!logger_ || logger_->Path().empty() ? MF_GRAYED : 0), IdMenuSaveLog, saveLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (!activeSession_ ? MF_GRAYED : 0), IdMenuClear, clearLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!HasTerminalSelection() ? MF_GRAYED : 0), IdMenuCopy, copyLabel);
    AppendMenuW(menu, MF_OWNERDRAW | (!connection_ || !connection_->IsConnected() || runningCommandIndex_ >= 0 || !IsClipboardFormatAvailable(CF_UNICODETEXT) ? MF_GRAYED : 0), IdMenuPaste, pasteLabel);
    AppendMenuW(menu, MF_OWNERDRAW, IdMenuSelectAll, selectAllLabel);
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, nullptr);
    static const wchar_t* endings[] = {L"Enter · CR", L"Enter · LF", L"Enter · CRLF", L"Enter · 不追加换行"};
    for (UINT index = 0; index < 4; ++index)
        AppendMenuW(menu, MF_OWNERDRAW | (selectedMode_ == 0 ? MF_GRAYED : 0) | (lineEndingIndex_ == static_cast<int>(index) ? MF_CHECKED : 0), IdMenuEndingCr + index, endings[index]);

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    DeleteObject(menuBrush);
    PostMessageW(window_, WM_NULL, 0, 0);
    InvalidateRect(terminal_, nullptr, TRUE);
    InvalidateRect(window_, nullptr, FALSE);

    switch (command) {
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
            activeSession_->codePage = selectedCodePage_;
            activeSession_->pendingDecodeBytes.clear();
        }
        const wchar_t* name = selectedCodePage_ == CP_UTF8 ? L"UTF-8" :
            (selectedCodePage_ == 936 ? L"GBK" : L"GB2312");
        AppendStatus(std::wstring(L"终端编码已切换为 ") + name + L"，仅影响后续收发", false);
        break;
    }
    case IdMenuSaveLog:
        SaveCurrentLog();
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
    if (runningCommandIndex_ >= 0 || !activeSession_) return;
    if (!OpenClipboard(terminal_)) return;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const auto* value = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    std::wstring pasted;
    if (value) {
        pasted = value;
        GlobalUnlock(handle);
    }
    CloseClipboard();
    if (pasted.empty()) return;

    std::wstring transmitted = pasted;
    if (activeSession_->terminal.BracketedPaste())
        transmitted = std::wstring(1, 0x1b) + L"[200~" + pasted +
            std::wstring(1, 0x1b) + L"[201~";
    const std::string encoded = WideToMultiByte(transmitted, SelectedCodePage());
    if (!encoded.empty()) {
        SendBytesToActive(Bytes(encoded.begin(), encoded.end()), localEchoEnabled_ || selectedMode_ == 4, pasted);
        ScrollTerminalToBottom();
        ClearTerminalSelection();
    }
}

void MainWindow::UpdateTerminalDimensions() {
    if (!terminal_ || !terminalFont_) return;
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
    terminalCellWidth_ = std::max(7, static_cast<int>(characterSize.cx));
    terminalLineHeight_ = std::max(15, static_cast<int>(metrics.tmHeight) + 2);
    const bool showTimestamps = timestampEnabled_ && activeSession_ &&
        !activeSession_->terminal.AlternateScreen();
    const int timestampWidth = showTimestamps ? 112 : 0;
    const int contentWidth = std::max(1,
        static_cast<int>(client.right) - 16 - timestampWidth - Ui::OverlayScrollLaneWidth);
    const int contentHeight = std::max(1, static_cast<int>(client.bottom) - 16);
    const int oldColumns = terminalVisibleColumns_, oldRows = terminalVisibleRows_;
    terminalVisibleColumns_ = std::max(20, contentWidth / terminalCellWidth_);
    terminalVisibleRows_ = std::max(2, contentHeight / terminalLineHeight_);
    for (const auto& session : sessions_) {
        session->terminal.Resize(terminalVisibleColumns_, terminalVisibleRows_);
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
        const bool showTimestamps = timestampEnabled_ && !model.AlternateScreen();
        const int padding = 8;
        const int timestampWidth = showTimestamps ? 112 : 0;
        const int textLeft = padding + timestampWidth;
        const size_t totalLines = model.DisplayLineCount();
        const size_t totalColumns = model.MaximumDisplayColumns();
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
            const TerminalLine& line = model.DisplayLine(lineIndex);
            const int y = padding + viewRow * terminalLineHeight_;
            if (showTimestamps && !line.timestamp.empty()) {
                RECT timestampRect{padding, y, textLeft - 12, y + terminalLineHeight_};
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
            const size_t cursorLine = model.CursorDisplayLine();
            const int cursorColumn = model.CursorColumn() - horizontalOffset;
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
            DrawRoundedBox(buffer, scroll.thumb, 3, colors.border, colors.border);
        const TerminalHorizontalScrollMetrics horizontalScroll =
            GetTerminalHorizontalScrollMetrics(terminal_, textLeft, totalColumns,
                terminalVisibleColumns_, activeSession_->terminalHorizontalOffset);
        if (horizontalScroll.visible)
            DrawRoundedBox(buffer, horizontalScroll.thumb, 3, colors.border, colors.border);

        if (activeSession_->terminalHasNewOutput && activeSession_->terminalScrollOffset > 0) {
            const int bottom = horizontalScroll.visible
                ? client.bottom - Ui::OverlayScrollLaneWidth - 4
                : client.bottom - 8;
            RECT notification{client.right - 126, bottom - 30,
                client.right - Ui::OverlayScrollLaneWidth - 8, bottom};
            DrawRoundedBox(buffer, notification, Ui::Radius, colors.accentSoft, colors.accent);
            DrawTextSimple(buffer, L"↓  新输出", notification, colors.accent,
                smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
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
    const int timestampWidth = timestampEnabled_ && !model.AlternateScreen() ? 112 : 0;
    const int textLeft = 8 + timestampWidth;
    const size_t totalLines = model.DisplayLineCount();
    const size_t topLine = totalLines > static_cast<size_t>(terminalVisibleRows_ +
        activeSession_->terminalScrollOffset)
        ? totalLines - static_cast<size_t>(terminalVisibleRows_ +
            activeSession_->terminalScrollOffset)
        : 0;
    const int row = std::max(0, std::min(terminalVisibleRows_ - 1,
        (static_cast<int>(point.y) - 8) / std::max(1, terminalLineHeight_)));
    if (totalLines == 0 || point.x < textLeft ||
        point.x >= client.right - Ui::OverlayScrollLaneWidth) return false;
    line = std::min(totalLines - 1, topLine + static_cast<size_t>(row));
    const TerminalLine& terminalLine = model.DisplayLine(line);
    if (terminalLine.cells.empty()) return false;
    const int viewColumn = (static_cast<int>(point.x) - textLeft) /
        std::max(1, terminalCellWidth_);
    column = std::max(0, std::min(static_cast<int>(terminalLine.cells.size()) - 1,
        viewColumn + activeSession_->terminalHorizontalOffset));
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

    size_t startLine = terminalSelectionAnchorLine_;
    size_t endLine = terminalSelectionFocusLine_;
    int startColumn = terminalSelectionAnchorColumn_;
    int endColumn = terminalSelectionFocusColumn_;
    if (startLine > endLine || (startLine == endLine && startColumn > endColumn)) {
        std::swap(startLine, endLine);
        std::swap(startColumn, endColumn);
    }
    startLine = std::min(startLine, model.DisplayLineCount() - 1);
    endLine = std::min(endLine, model.DisplayLineCount() - 1);
    std::wstring text;
    for (size_t lineIndex = startLine; lineIndex <= endLine; ++lineIndex) {
        const TerminalLine& line = model.DisplayLine(lineIndex);
        const int lineColumns = static_cast<int>(line.cells.size());
        const int first = lineIndex == startLine ? std::max(0, startColumn) : 0;
        const int last = lineIndex == endLine
            ? std::min(lineColumns, endColumn + 1)
            : lineColumns;
        std::wstring row;
        for (int column = std::min(first, lineColumns); column < last; ++column) {
            const TerminalCell& cell = line.cells[static_cast<size_t>(column)];
            if (!cell.continuation) row += cell.character;
        }
        while (!row.empty() && row.back() == L' ') row.pop_back();
        text += row;
        if (lineIndex < endLine && !line.wrapped) text += L"\r\n";
    }
    return text;
}

void MainWindow::CopyTerminalSelection(bool selectAll) {
    const std::wstring text = TerminalSelectionText(selectAll);
    if (text.empty() || !OpenClipboard(terminal_)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

UINT MainWindow::SelectedCodePage() const {
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
    ShowWindow(disconnectButton_, activeSession_ ? SW_SHOW : SW_HIDE);
    EnableWindow(disconnectButton_, activeSession_ != nullptr);

    EnableWindow(sftpTabButton_, connected && selectedMode_ == 0 && SftpClient::IsAvailable());


    SetSftpBusy(sftpBusy_);
    UpdateCommandActions();
    for (HWND button : toolbarButtons_) InvalidateRect(button, nullptr, TRUE);



    InvalidateRect(window_, nullptr, TRUE);
}

void MainWindow::RefreshConnectionList() {
    size_t selectedIndex = 0;
    if (activeSession_) {
        const auto selected = std::find_if(sessions_.begin(), sessions_.end(),
            [this](const std::unique_ptr<SessionState>& item) { return item.get() == activeSession_; });
        if (selected != sessions_.end()) selectedIndex = static_cast<size_t>(selected - sessions_.begin());
    }
    SendMessageW(connectionList_, LB_RESETCONTENT, 0, 0);
    for (const auto& session : sessions_)
        SendMessageW(connectionList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(session->name.c_str()));
    if (!sessions_.empty()) SendMessageW(connectionList_, LB_SETCURSEL, selectedIndex, 0);
    InvalidateRect(connectionList_, nullptr, TRUE);
}

void MainWindow::LoadUiState() {
    std::wstring directory = DirectoryOf(DefaultCommandsPath());
    if (directory.empty()) {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        directory = DirectoryOf(modulePath);
    }
    const std::wstring path = JoinLocalPath(directory, L"settings.ini");
    rightPanelWidth_ = std::max(Ui::MinRightWidth,
        std::min(Ui::MaxRightWidth, static_cast<int>(GetPrivateProfileIntW(
            L"Layout", L"RightPanelWidth", 360, path.c_str()))));
    sftpNameWidth_ = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Layout", L"SftpNameWidth", 180, path.c_str())), 80, 600);
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
    WritePrivateProfileStringW(L"Layout", L"SftpNameWidth", std::to_wstring(sftpNameWidth_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"RightPanelWidth",
        std::to_wstring(rightPanelWidth_).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Layout", L"RightPanelCollapsed",
        rightPanelCollapsed_ ? L"1" : L"0", path.c_str());
}

void MainWindow::SetRightPanelCollapsed(bool collapsed) {
    if (rightPanelCollapsed_ == collapsed) return;
    rightPanelCollapsed_ = collapsed;
    SetWindowTextW(rightPanelToggleButton_, collapsed ? L"展开侧栏" : L"折叠侧栏");
    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
    SaveUiState();
    InvalidateRect(window_, nullptr, TRUE);
}

void MainWindow::ShowSftpPanel(bool show) {
    if (show && (!connection_ || selectedMode_ != 0 || !SftpClient::IsAvailable())) show = false;
    sftpPanelVisible_ = show;
    if (rightPanelCollapsed_ || rightPanelAutoCollapsed_) {
        for (HWND control : {commandHeader_, sftpTabButton_, commandList_, importButton_, exportButton_,
                 addCommandButton_, deleteCommandButton_, saveCommandsButton_, sftpPath_, sftpNameHeader_, sftpSizeHeader_,
                 sftpModifiedHeader_, sftpList_, sftpTransferToggleButton_, sftpTransferList_,
                 sftpClearTransfersButton_, sftpUpButton_, sftpRefreshButton_, sftpUploadButton_,
                 sftpDownloadButton_})
            ShowWindow(control, SW_HIDE);
        ShowWindow(rightPanelToggleButton_, SW_SHOW);
        InvalidateRect(window_, nullptr, TRUE);
        return;
    }

    ShowWindow(commandHeader_, SW_SHOW);
    ShowWindow(sftpTabButton_, SW_SHOW);
    for (HWND control : {commandList_, importButton_, exportButton_, addCommandButton_, deleteCommandButton_, saveCommandsButton_})
        ShowWindow(control, show ? SW_HIDE : SW_SHOW);
    for (HWND control : {sftpPath_, sftpNameHeader_, sftpSizeHeader_, sftpList_,
             sftpTransferToggleButton_, sftpClearTransfersButton_, sftpUpButton_, sftpRefreshButton_,
             sftpUploadButton_, sftpDownloadButton_})
        ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    RECT listRect{};
    GetClientRect(sftpList_, &listRect);
    ShowWindow(sftpModifiedHeader_, show && listRect.right >= 300 ? SW_SHOW : SW_HIDE);
    ShowWindow(sftpTransferList_, show && sftpTransferExpanded_ ? SW_SHOW : SW_HIDE);
    InvalidateRect(commandHeader_, nullptr, TRUE);
    InvalidateRect(sftpTabButton_, nullptr, TRUE);
    InvalidateRect(window_, nullptr, TRUE);
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
        self->sftpNameWidth_ = std::max(80, self->sftpColumnDragWidth_ + (static_cast<int>(p.x) - self->sftpColumnDragX_) * (self->sftpColumnDragging_ == 1 ? 1 : -1));
        RECT list{}, header{}; GetWindowRect(self->sftpList_, &list);
        GetWindowRect(self->sftpNameHeader_, &header);
        MapWindowPoints(HWND_DESKTOP, self->window_, reinterpret_cast<POINT*>(&list), 2);
        MapWindowPoints(HWND_DESKTOP, self->window_, reinterpret_cast<POINT*>(&header), 2);
        int name, size, modified; self->SftpColumnWidths(list.right - list.left - Ui::OverlayScrollLaneWidth, name, size, modified);
        HDWP batch = BeginDeferWindowPos(3);
        batch = DeferWindowPos(batch, self->sftpNameHeader_, nullptr, list.left, header.top, name, Ui::CompactHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        batch = DeferWindowPos(batch, self->sftpSizeHeader_, nullptr, list.left + name, header.top, size, Ui::CompactHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        batch = DeferWindowPos(batch, self->sftpModifiedHeader_, nullptr, list.left + name + size, header.top, modified, Ui::CompactHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        if (batch) EndDeferWindowPos(batch);
        RECT redraw{list.left, header.top, list.right, header.bottom};
        InvalidateRect(self->window_, &redraw, FALSE);
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
    const std::string encoded = WideToMultiByte(text, SelectedCodePage());
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
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SFTP_INPUT), owner ? owner : window_,
            SftpInputDialogProc, reinterpret_cast<LPARAM>(this)) != IDOK)
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
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
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
    if (commandsDirty_) {
        const int choice = MessageBoxW(window_, L"当前命令尚未保存。导入前是否保存？", L"导入命令", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL || (choice == IDYES && !SaveCommands())) return;
    }
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
    const int DialogWidth = commandDialogStepEdits_.size() > 5 ? 460 : 238;
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
        const int y = StepTop + (index % 5) * StepStride;
        const int columnLeft = index / 5 * 222;
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

    const int addY = StepTop + std::min(stepCount, 5) * StepStride;
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
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
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
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_COMMAND), window_, CommandDialogProc,
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
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_COMMAND), window_, CommandDialogProc,
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
 Json resources=Json::array();
 for(const auto& session:sessions_) {
  Json resource={{"id","session-"+std::to_string(session->id)},{"name",WideToMultiByte(session->name,CP_UTF8)},{"kind",session->mode==0?"ssh":session->mode==1?"serial":session->mode==4?"cmd":"terminal"},{"connected",session->connection->IsConnected()},{"port",session->port}};
  if(session->mode==1){auto end=session->name.find(L' ');resource["com"]=WideToMultiByte(session->name.substr(0,end),CP_UTF8);}
  resources.push_back(resource);
 }
 resources.push_back({{"id","power-1"},{"name","IT6332A"},{"kind","power"},{"connected",powerService_.State()["connected"]}});
 return {{"resources",resources}};
}
Json MainWindow::ApiRequest(const std::string& method,const std::string& path,const Json& body) {
 if(path=="/api/v1/power-supplies/power-1/usb-resources"&&method=="GET")return {{"resources",PowerService::UsbResources()},{"visaAvailable",PowerService::VisaAvailable()}};
 auto request=std::make_shared<ApiMessage>();request->method=method;request->path=path;request->body=body;auto future=request->result.get_future();
 auto* pointer=new ApiRequestMessage(request);
 if(!PostMessageW(window_,MessageApi,0,reinterpret_cast<LPARAM>(pointer))){delete pointer;return {{"error","Application closing"}};}
 if(future.wait_for(std::chrono::seconds(2))!=std::future_status::ready){int expected=0;if(request->state.compare_exchange_strong(expected,2))return {{"error","UI busy; request canceled"}};future.wait();}
 return future.get();
}
Json MainWindow::ExecuteApiRequest(const std::string& method,const std::string& path,const Json& body) {
 if(closing_)return {{"error","Application closing"}};
 if(path=="/api/v1/resources"&&method=="GET")return ApiResources();
 const std::string powerPrefix="/api/v1/power-supplies/power-1";
 if(path.rfind("/api/v1/actions/",0)==0&&method=="GET")return powerService_.Action(path.substr(16));
 if(path==powerPrefix&&method=="GET")return powerService_.State();
 if(path.rfind(powerPrefix+"/",0)==0&&method=="POST"){
  Json command=body;std::string type=path.substr(powerPrefix.size()+1);if(type=="channels/output")type="output";if(type=="channels/parameters")type="parameters";if(type=="channels/protection")type="protection";
  if(type!="connect"&&type!="disconnect"&&type!="output"&&type!="parameters"&&type!="protection"&&type!="task"&&type!="stop")return {{"error","Unknown endpoint"}};
  command["type"]=type;return powerService_.Submit(command);
 }
 const std::string prefix="/api/v1/sessions/";
 if(path.rfind(prefix,0)==0){auto rest=path.substr(prefix.size());auto slash=rest.find('/');std::string id=rest.substr(0,slash);
  if(id.rfind("session-",0)!=0)return {{"error","Invalid session ID"}};auto idText=id.substr(8);if(idText.empty()||idText.find_first_not_of("0123456789")!=std::string::npos)return {{"error","Invalid session ID"}};
  SessionState* session=FindSession(std::stoull(idText));if(!session)return {{"error","Session no longer exists"}};
  if(method=="GET"&&slash==std::string::npos)return {{"id",id},{"connected",session->connection->IsConnected()},{"codePage",session->codePage},{"lineEnding",session->lineEndingIndex}};
  if(method=="POST"&&rest.substr(slash+1)=="input"){
   Bytes bytes=Decode64(body.at("data").get<std::string>());if(bytes.empty()||bytes.size()>65536)return {{"error","Input must contain 1..65536 bytes"}};
   std::wstring error;if(!session->connection->Send(bytes,error))return {{"error",WideToMultiByte(error,CP_UTF8)}};
   apiServer_.Publish(id,bytes,"ai","input");
   if(session->mode==4){ // Pipe-based CMD does not echo input; display it and keep the device output stream clean.
    auto text=MultiByteToWide(bytes.data(),bytes.size(),session->codePage);session->terminal.Feed(text,CurrentTerminalTimestamp());if(session==activeSession_)InvalidateRect(terminal_,nullptr,FALSE);
   }
   if(session->logger)session->logger->WriteStatus(L"AI 输入："+MultiByteToWide(bytes.data(),bytes.size(),session->codePage));
   AppendStatus(L"AI → "+session->name,false);return {{"accepted",bytes.size()}};
  }
 }
 return {{"error","Endpoint not found"},{"httpStatus",404}};
}
void MainWindow::ShowApiDialog(){
 ApiDialogData data{this,ApiResources()["resources"],nullptr,nullptr};struct Template{DLGTEMPLATE dialog;WORD menu=0,cls=0,title=0;};Template t{};t.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;t.dialog.cx=270;t.dialog.cy=240;
 DialogBoxIndirectParamW(instance_,&t.dialog,window_,ApiDialogProc,reinterpret_cast<LPARAM>(&data));
}
INT_PTR CALLBACK MainWindow::ApiDialogProc(HWND h,UINT message,WPARAM w,LPARAM l){
 auto* data=reinterpret_cast<ApiDialogData*>(GetWindowLongPtrW(h,DWLP_USER));
 if(message==WM_INITDIALOG){data=reinterpret_cast<ApiDialogData*>(l);SetWindowLongPtrW(h,DWLP_USER,reinterpret_cast<LONG_PTR>(data));SetWindowTextW(h,L"AI API");HDC dc=GetDC(h);int dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(h,dc);auto d=[dpi](int x){return MulDiv(x,dpi,96);};SetWindowPos(h,nullptr,0,0,d(520),d(548),SWP_NOMOVE|SWP_NOZORDER);
  auto add=[&](const wchar_t* cls,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style=0){auto c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,d(x),d(y),d(width),d(height),h,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(data->owner->uiFont_),TRUE);return c;};
  add(L"STATIC",L"授权资源",-1,16,12,472,28);data->list=add(L"LISTBOX",L"",200,16,44,472,144,LBS_MULTIPLESEL|LBS_NOINTEGRALHEIGHT|WS_TABSTOP);
  for(const auto& resource:data->resources){std::string name=resource["name"];std::wstring text=MultiByteToWide(reinterpret_cast<const std::uint8_t*>(name.data()),name.size(),CP_UTF8);SendMessageW(data->list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
  add(L"BUTTON",L"读取数据",201,16,200,128,28,BS_AUTOCHECKBOX|WS_TABSTOP);SendDlgItemMessageW(h,201,BM_SETCHECK,BST_CHECKED,0);add(L"BUTTON",L"终端输入",202,160,200,144,28,BS_AUTOCHECKBOX|WS_TABSTOP);add(L"BUTTON",L"加电 / 掉电",203,320,200,160,28,BS_AUTOCHECKBOX|WS_TABSTOP);
  add(L"BUTTON",L"参数 / 保护",204,16,236,144,28,BS_AUTOCHECKBOX|WS_TABSTOP);add(L"BUTTON",L"循环测试",205,176,236,128,28,BS_AUTOCHECKBOX|WS_TABSTOP);add(L"BUTTON",L"电源连接 / 断开",206,320,236,168,28,BS_AUTOCHECKBOX|WS_TABSTOP);
  for(int i=0;i<3;++i){add(L"BUTTON",(L"CH"+std::to_wstring(i+1)).c_str(),210+i,16+i*96,276,80,28,BS_AUTOCHECKBOX|WS_TABSTOP);SendDlgItemMessageW(h,210+i,BM_SETCHECK,BST_CHECKED,0);}
  add(L"BUTTON",L"兼容串口 TCP（无密钥）",240,304,276,184,28,BS_AUTOCHECKBOX|WS_TABSTOP);SendDlgItemMessageW(h,240,BM_SETCHECK,SerialShareService::LegacyEnabled()?BST_CHECKED:BST_UNCHECKED,0);
  data->info=add(L"EDIT",L"",220,16,320,472,60,ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL);
  add(L"BUTTON",L"启用 / 更新",230,16,400,112,36,WS_TABSTOP|BS_OWNERDRAW);add(L"BUTTON",L"关闭 API",231,144,400,96,36,WS_TABSTOP|BS_OWNERDRAW);add(L"BUTTON",L"复制连接信息",232,256,400,128,36,WS_TABSTOP|BS_OWNERDRAW);add(L"BUTTON",L"完成",IDCANCEL,392,448,96,36,WS_TABSTOP|BS_OWNERDRAW);
  ApplyDarkTitleBar(h,data->owner->darkMode_);
  for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
   wchar_t cls[32]{};GetClassNameW(child,cls,32);
   if(_wcsicmp(cls,L"EDIT")==0){PrepareDialogEditField(h,child,data->owner);SetWindowSubclass(child,DialogControlSubclassProc,1,reinterpret_cast<DWORD_PTR>(data->owner));}
   else if(_wcsicmp(cls,L"LISTBOX")==0)SetWindowSubclass(child,OverlayListSubclassProc,2,reinterpret_cast<DWORD_PTR>(data->owner));
   else if(_wcsicmp(cls,L"BUTTON")==0)SetWindowSubclass(child,ButtonSubclassProc,1,reinterpret_cast<DWORD_PTR>(data->owner));
  }
  RECT owner{},rect{};GetWindowRect(data->owner->window_,&owner);GetWindowRect(h,&rect);SetWindowPos(h,nullptr,owner.left+(owner.right-owner.left-(rect.right-rect.left))/2,owner.top+(owner.bottom-owner.top-(rect.bottom-rect.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
  SendMessageW(h,WM_COMMAND,233,0);return TRUE;
 }
 if(!data)return FALSE;auto* self=data->owner;
 if(message==WM_DRAWITEM){self->DrawOwnerItem(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;}
 if(message==WM_CTLCOLORDLG||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX){auto colors=Colors(self->darkMode_);SetTextColor(reinterpret_cast<HDC>(w),colors.text);SetBkColor(reinterpret_cast<HDC>(w),message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX?colors.terminal:colors.panel);return reinterpret_cast<INT_PTR>(message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX?self->terminalBrush_:self->panelBrush_);}
 if(message==WM_COMMAND){int id=LOWORD(w);if(id==IDCANCEL){EndDialog(h,IDCANCEL);return TRUE;}
  if(id==240){if(self->pendingSession_){SetWindowTextW(data->info,L"请等待串口连接完成后切换兼容服务");return TRUE;}std::wstring error;if(!SerialShareService::SetLegacyEnabled(SendDlgItemMessageW(h,240,BM_GETCHECK,0,0)==BST_CHECKED,error))SetWindowTextW(data->info,error.c_str());
   for(auto& session:self->sessions_)if(session->mode==1){auto* queue=dynamic_cast<QueuedConnection*>(session->connection.get());auto* serial=queue?dynamic_cast<SerialShareConnection*>(queue->Transport()):nullptr;if(serial)session->port=serial->SharedPort();}
   InvalidateRect(self->connectionList_,nullptr,FALSE);return TRUE;}
  if(id==230){std::map<std::string,ApiGrant> grants;unsigned rights=0,channels=0;for(int i=0;i<6;++i)if(SendDlgItemMessageW(h,201+i,BM_GETCHECK,0,0)==BST_CHECKED)rights|=1u<<i;for(int i=0;i<3;++i)if(SendDlgItemMessageW(h,210+i,BM_GETCHECK,0,0)==BST_CHECKED)channels|=1u<<i;
   for(size_t i=0;i<data->resources.size();++i)if(SendMessageW(data->list,LB_GETSEL,i,0)>0)grants[data->resources[i]["id"]]={rights,channels};
   if(grants.empty()||!(rights&1)){SetWindowTextW(data->info,L"请选择资源并授权读取数据");return TRUE;}
   std::wstring error;if(!self->apiServer_.Start([self](const auto& method,const auto& path,const auto& body){return self->ApiRequest(method,path,body);},grants,error)){SetWindowTextW(data->info,error.c_str());return TRUE;}
  }else if(id==231)self->apiServer_.Stop();
  if(id==230||id==231||id==233){std::wstring info=self->apiServer_.Running()?L"端口："+std::to_wstring(self->apiServer_.Port())+L"\r\nToken："+MultiByteToWide(reinterpret_cast<const std::uint8_t*>(self->apiServer_.Token().data()),self->apiServer_.Token().size(),CP_UTF8):L"API 已关闭";SetWindowTextW(data->info,info.c_str());return TRUE;}
  if(id==232&&self->apiServer_.Running()){std::wstring info=L"API_PORT="+std::to_wstring(self->apiServer_.Port())+L"\r\nTOKEN="+MultiByteToWide(reinterpret_cast<const std::uint8_t*>(self->apiServer_.Token().data()),self->apiServer_.Token().size(),CP_UTF8);SetClipboardText(h,info);return TRUE;}
 }
 return FALSE;
}
}
