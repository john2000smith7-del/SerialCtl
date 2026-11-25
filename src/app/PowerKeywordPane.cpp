#include "PowerPane.h"
#include "UiPopupMenu.h"
#include "Win32Helpers.h"
#include <commctrl.h>
#include <commdlg.h>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace serialctl {
namespace {
constexpr int ReadyId = 301, RulesId = 302, AddId = 303, AddCmdId = 304, DeleteId = 305,
              ClearId = 306, LoadFileId = 307, SaveFileId = 308;
constexpr int RuleFieldId = 310;
constexpr int CmdFieldId = 320;
constexpr unsigned MaximumDelay = 604800000;
constexpr int TextLimit = 4096;

constexpr PowerKeywordAction kActions[] = {
    PowerKeywordAction::None,
    PowerKeywordAction::RunCommands,
    PowerKeywordAction::StopTest,
    PowerKeywordAction::StopTestPowerOff,
    PowerKeywordAction::PowerOffContinue,
    PowerKeywordAction::RunCommandsStopTest,
    PowerKeywordAction::PauseAll,
};

struct Palette {
    int dpi;
    bool dark;
    HFONT font;
    HBRUSH background, field;
    UiFieldStyle *style;
    int Scale(int value) const { return MulDiv(value, dpi, 96); }
};

struct ConfigRow {
    enum class Kind { Keyword, Command };
    Kind kind = Kind::Keyword;
    size_t ruleIndex = 0;
    size_t commandIndex = 0;
};

struct ConfigDialog {
    Palette ui;
    PowerKeywordConfig config;
    std::vector<bool> expanded;
    std::vector<ConfigRow> rows;
};

struct RuleDialog {
    Palette ui;
    PowerKeywordRule rule;
    int initialFocusField = 0;
};

struct CommandDialog {
    Palette ui;
    PowerKeywordCommand command;
};

struct DialogTemplate {
    DLGTEMPLATE dialog{};
    WORD menu = 0, cls = 0, title = 0;
    DialogTemplate() {
        dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        dialog.cx = 238;
        dialog.cy = 190;
    }
};

std::wstring WideText(const std::string &text) {
    return MultiByteToWide(reinterpret_cast<const std::uint8_t *>(text.data()), text.size(), CP_UTF8);
}

bool ValidField(const std::wstring &text) {
    return text.size() <= TextLimit && std::none_of(text.begin(), text.end(), [](wchar_t character) {
        return character < L' ' && character != L'\t';
    });
}

void Validate(const PowerKeywordRule &rule) {
    if (rule.keyword.empty() ||
        std::all_of(rule.keyword.begin(), rule.keyword.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; }))
        throw std::runtime_error("请输入关键字");
    for (const auto *field : {&rule.keyword, &rule.onHit, &rule.onMiss, &rule.begin, &rule.end})
        if (!ValidField(*field)) throw std::runtime_error("关键字、指令和检测标志须为单行文本");
    for (const auto &cmd : rule.commands) {
        if (cmd.text.empty() ||
            std::all_of(cmd.text.begin(), cmd.text.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; }))
            throw std::runtime_error("请输入子命令内容");
        if (!ValidField(cmd.text)) throw std::runtime_error("子命令须为单行文本");
    }
}

unsigned DelayValue(const Json &value) {
    if (!value.is_number_integer() || value < 0 || value > MaximumDelay)
        throw std::runtime_error("动作延迟超出范围");
    return value.get<unsigned>();
}

const char *TriggerKey(PowerKeywordCommandTrigger trigger) {
    switch (trigger) {
    case PowerKeywordCommandTrigger::OnHit: return "hit";
    case PowerKeywordCommandTrigger::OnMiss: return "miss";
    case PowerKeywordCommandTrigger::Both: return "both";
    }
    return "hit";
}

PowerKeywordCommandTrigger ParseTriggerKey(const std::string &key) {
    if (key == "miss") return PowerKeywordCommandTrigger::OnMiss;
    if (key == "both") return PowerKeywordCommandTrigger::Both;
    return PowerKeywordCommandTrigger::OnHit;
}

const wchar_t *TriggerLabel(PowerKeywordCommandTrigger trigger) {
    switch (trigger) {
    case PowerKeywordCommandTrigger::OnHit: return L"捕获时执行";
    case PowerKeywordCommandTrigger::OnMiss: return L"丢失时执行";
    case PowerKeywordCommandTrigger::Both: return L"捕获与丢失均执行";
    }
    return L"捕获时执行";
}

const wchar_t *TriggerShortLabel(PowerKeywordCommandTrigger trigger) {
    switch (trigger) {
    case PowerKeywordCommandTrigger::OnHit: return L"捕获指令";
    case PowerKeywordCommandTrigger::OnMiss: return L"丢失指令";
    case PowerKeywordCommandTrigger::Both: return L"双触发指令";
    }
    return L"捕获指令";
}

std::wstring FormatMsLabel(unsigned ms, bool alwaysSuffix = true) {
    if (ms == 0) return alwaysSuffix ? L"0ms" : L"0";
    if (ms >= 1000 && ms % 1000 == 0 && !alwaysSuffix) return std::to_wstring(ms / 1000) + L"s";
    return std::to_wstring(ms) + L"ms";
}

std::wstring DialogText(HWND control) {
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}

void SetTextIfChanged(HWND control, const std::wstring &text) {
    if (!control) return;
    if (text != DialogText(control)) SetWindowTextW(control, text.c_str());
}

void PaintKeywordHeader(HWND header, HDC dc, bool dark) {
    RECT bounds{};
    GetClientRect(header, &bounds);
    UiBufferedPaint buffer(dc, bounds);
    dc = buffer.Dc();
    const auto theme = UiTheme(dark);
    HBRUSH background = CreateSolidBrush(theme.raised);
    HBRUSH divider = CreateSolidBrush(theme.border);
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    SetTextColor(dc, theme.text);
    SetBkMode(dc, TRANSPARENT);
    const auto font = SendMessageW(header, WM_GETFONT, 0, 0);
    const auto previous = font ? SelectObject(dc, reinterpret_cast<HFONT>(font)) : nullptr;
    const int padding = MulDiv(6, GetDeviceCaps(dc, LOGPIXELSX), 96);
    for (int i = 0; i < Header_GetItemCount(header); ++i) {
        RECT item{};
        if (!Header_GetItemRect(header, i, &item)) continue;
        wchar_t title[64]{};
        HDITEMW column{};
        column.mask = HDI_TEXT | HDI_FORMAT;
        column.pszText = title;
        column.cchTextMax = static_cast<int>(std::size(title));
        SendMessageW(header, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&column));
        const RECT edge{item.right - 1, item.top, item.right, item.bottom};
        FillRect(dc, &edge, divider);
        item.left += padding;
        item.right -= padding;
        DrawTextW(dc, title, -1, &item, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
            ((column.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT));
    }
    UiSeparator(dc, 0, bounds.right, bounds.bottom - 1, theme.border);
    if (previous) SelectObject(dc, previous);
    DeleteObject(divider);
}

LRESULT CALLBACK KeywordHeaderProc(HWND header, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR dark) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(header, &paint);
        PaintKeywordHeader(header, dc, dark != 0);
        EndPaint(header, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        PaintKeywordHeader(header, reinterpret_cast<HDC>(w), dark != 0);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(header, KeywordHeaderProc, id);
    return DefSubclassProc(header, message, w, l);
}

void ThemeList(HWND list, bool dark) {
    if (!list) return;
    const auto theme = UiTheme(dark);
    ListView_SetBkColor(list, theme.surface);
    ListView_SetTextBkColor(list, theme.surface);
    ListView_SetTextColor(list, theme.text);
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    if (const auto header = ListView_GetHeader(list)) {
        SetWindowSubclass(header, KeywordHeaderProc, 1, dark);
        InvalidateRect(header, nullptr, FALSE);
    }
}

void Column(HWND list, int index, const wchar_t *text, int width, int alignment = LVCFMT_LEFT) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<wchar_t *>(text);
    column.cx = width;
    column.fmt = alignment;
    SendMessageW(list, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
}

void ListCell(HWND list, int row, int column, const std::wstring &text) {
    LVITEMW item{};
    item.iItem = row;
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t *>(text.c_str());
    if (column == 0 && row >= ListView_GetItemCount(list)) {
        item.mask = LVIF_TEXT;
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    } else {
        wchar_t current[TextLimit + 1]{};
        LVITEMW existing = item;
        existing.pszText = current;
        existing.cchTextMax = static_cast<int>(std::size(current));
        SendMessageW(list, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&existing));
        if (text != current) SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
    }
}

void PositionDialog(HWND dialog, const Palette &ui, int width, int height) {
    UiResizeDialog(dialog, ui.dpi, width, height);
    const int targetW = ui.Scale(width), targetH = ui.Scale(height);
    RECT client{};
    GetClientRect(dialog, &client);
    if (client.right != targetW || client.bottom != targetH) {
        RECT actual{};
        GetWindowRect(dialog, &actual);
        SetWindowPos(dialog, nullptr, 0, 0,
                     (actual.right - actual.left) + (targetW - client.right),
                     (actual.bottom - actual.top) + (targetH - client.bottom),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT bounds{}, owner{};
    const auto parent = GetWindow(dialog, GW_OWNER);
    GetWindowRect(dialog, &bounds);
    GetWindowRect(parent, &owner);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto horizontal = bounds.right - bounds.left, vertical = bounds.bottom - bounds.top;
    const auto &work = monitor.rcWork;
    const auto x = std::clamp((owner.left + owner.right - horizontal) / 2, work.left,
                              std::max(work.left, work.right - horizontal));
    const auto y = std::clamp((owner.top + owner.bottom - vertical) / 2, work.top,
                              std::max(work.top, work.bottom - vertical));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND AddControl(HWND parent, const Palette &ui, const wchar_t *cls, const wchar_t *title, int id, DWORD style,
                int x, int y, int width, int height) {
    HWND control = CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style, ui.Scale(x), ui.Scale(y),
                                  ui.Scale(width), ui.Scale(height), parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
    const auto theme = UiTheme(ui.dark);
    if (_wcsicmp(cls, L"BUTTON") == 0) {
        UiStyleButton(control);
        UiSetControlBackdrop(control, theme.surface);
    }
    if (_wcsicmp(cls, L"EDIT") == 0) {
        UiStyleField(control, ui.style);
        UiSetControlBackdrop(control, theme.surface);
        SendMessageW(control, EM_SETLIMITTEXT, (style & ES_NUMBER) ? 9 : TextLimit, 0);
    }
    if (_wcsicmp(cls, L"COMBOBOX") == 0) {
        UiStyleField(control, ui.style);
        UiSetControlBackdrop(control, theme.surface);
    }
    return control;
}

HWND AddButton(HWND parent, const Palette &ui, int id, const wchar_t *text, int x, int y, int width = 96, int height = 32) {
    return AddControl(parent, ui, L"BUTTON", text, id, WS_TABSTOP | BS_OWNERDRAW, x, y, width, height);
}

HWND AddField(HWND parent, const Palette &ui, int id, const wchar_t *label, const std::wstring &value,
              int x, int y, int width, bool number = false) {
    AddControl(parent, ui, L"STATIC", label, -1, SS_NOPREFIX, x, y, width, 20);
    return AddControl(parent, ui, L"EDIT", value.c_str(), id, WS_TABSTOP | ES_AUTOHSCROLL | (number ? ES_NUMBER : 0),
                      x, y + 22, width, 28);
}

HWND AddComboField(HWND parent, const Palette &ui, int id, const wchar_t *label, const std::vector<std::wstring> &items,
                   int selected, int x, int y, int width) {
    AddControl(parent, ui, L"STATIC", label, -1, SS_NOPREFIX, x, y, width, 20);
    HWND combo = AddControl(parent, ui, L"COMBOBOX", L"", id,
                            WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                            x, y + 22, width, 190);
    for (const auto &text : items)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(combo, CB_SETCURSEL, std::clamp(selected, 0, static_cast<int>(items.size()) - 1), 0);
    UiLayoutBatch layout;
    layout.ComboVisibleHeight(combo, ui.Scale(28));
    layout.Commit();
    return combo;
}

INT_PTR PaintDialog(HWND dialog, const Palette &ui, UINT message, WPARAM w, LPARAM l) {
    const auto theme = UiTheme(ui.dark);
    if (message == WM_MEASUREITEM) {
        auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            item.itemHeight = ui.Scale(28);
            return TRUE;
        }
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
        HDC dc = reinterpret_cast<HDC>(w);
        if (message == WM_CTLCOLORBTN) {
            SetBkMode(dc, TRANSPARENT);
        }
        SetTextColor(dc, theme.text);
        const bool isField = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        SetBkColor(dc, isField ? theme.field : theme.surface);
        return reinterpret_cast<INT_PTR>(isField ? ui.field : ui.background);
    }
    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(dialog, &rect);
        FillRect(reinterpret_cast<HDC>(w), &rect, ui.background);
        return TRUE;
    }
    if (message != WM_DRAWITEM) return FALSE;
    auto item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
    if (item.CtlType == ODT_COMBOBOX && ui.style) {
        UiDrawComboItem(item, *ui.style);
        return TRUE;
    }
    UiBufferedPaint paint(item.hDC, item.rcItem);
    item.hDC = paint.Dc();
    FillRect(item.hDC, &item.rcItem, ui.background);
    const bool enabled = !(item.itemState & ODS_DISABLED), pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = enabled && item.CtlID == IDOK;
    const auto visual = UiButtonVisual(theme, enabled, pressed,
                                       GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, primary);
    UiBox(item.hDC, item.rcItem, visual.fill, enabled ? visual.border : UiPanelBorder(ui.dark), UiCapsuleRadius(item.rcItem));
    UiButtonKeyboardFocus(item.hDC, item.rcItem, item.itemState, ui.dpi, UiFocusInk(ui.dark, enabled && (primary || pressed)));
    SetBkMode(item.hDC, TRANSPARENT);
    const bool danger = enabled && !pressed && (item.CtlID == DeleteId || item.CtlID == ClearId);
    SetTextColor(item.hDC, danger ? theme.danger : visual.ink);
    auto old = SelectObject(item.hDC, ui.font);
    wchar_t title[64]{};
    GetWindowTextW(item.hwndItem, title, static_cast<int>(std::size(title)));
    RECT textRect = UiCapsuleContentRect(item.rcItem, ui.dpi);
    DrawTextW(item.hDC, title, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, old);
    return TRUE;
}

std::wstring ChooseKeywordJsonFile(HWND owner, bool save) {
    wchar_t path[32768]{};
    OPENFILENAMEW file{};
    file.lStructSize = sizeof(file);
    file.hwndOwner = owner;
    file.lpstrFilter = L"关键字配置文件 (*.json)\0*.json\0\0";
    file.lpstrFile = path;
    file.nMaxFile = 32768;
    file.lpstrDefExt = L"json";
    file.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&file) : GetOpenFileNameW(&file)) ? path : L"";
}

PowerKeywordConfig ReadKeywordFile(const std::wstring &path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) throw std::runtime_error("无法读取关键字配置文件");
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024) throw std::runtime_error("关键字配置文件过大");
    file.seekg(0);
    Json parsed;
    file >> parsed;
    return PowerPane::DecodeKeywords(parsed);
}

void WriteKeywordFile(const std::wstring &path, const PowerKeywordConfig &config) {
    const auto encoded = PowerPane::EncodeKeywords(config).dump(2);
    if (encoded.size() > 1024 * 1024) throw std::runtime_error("关键字配置文件过大");
    const auto temporary = path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("无法写入关键字配置文件");
        file << encoded;
        file.flush();
        if (!file) {
            DeleteFileW(temporary.c_str());
            throw std::runtime_error("关键字配置文件写入失败");
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("无法保存关键字配置文件");
    }
}

void UpdateConfigButtonStates(HWND dialog, const ConfigDialog &data, int selectedRow) {
    const bool validRow = selectedRow >= 0 && static_cast<size_t>(selectedRow) < data.rows.size();
    EnableWindow(GetDlgItem(dialog, AddCmdId), validRow);
    EnableWindow(GetDlgItem(dialog, DeleteId), validRow);
    EnableWindow(GetDlgItem(dialog, ClearId), !data.config.rules.empty());
}

void PopulateRules(HWND dialog, ConfigDialog &data, int targetRuleIndex = -1, int targetCmdIndex = -1) {
    HWND list = GetDlgItem(dialog, RulesId);
    UiRedrawScope redraw(list);
    ListView_DeleteAllItems(list);
    data.rows.clear();
    data.expanded.resize(data.config.rules.size(), true);

    int selectVisualRow = -1;
    for (size_t r = 0; r < data.config.rules.size(); ++r) {
        const auto &rule = data.config.rules[r];
        const bool expanded = data.expanded[r];
        const int keywordRow = static_cast<int>(data.rows.size());
        data.rows.push_back({ConfigRow::Kind::Keyword, r, 0});
        if (static_cast<int>(r) == targetRuleIndex && targetCmdIndex < 0) selectVisualRow = keywordRow;

        const std::wstring prefix = rule.commands.empty() ? L"  " : (expanded ? L"▾ " : L"▸ ");
        ListCell(list, keywordRow, 0, prefix + rule.keyword);
        ListCell(list, keywordRow, 1, PowerKeywordActionLabel(rule.hitAction));
        ListCell(list, keywordRow, 2, rule.hitAction == PowerKeywordAction::None ? L"0" : FormatMsLabel(rule.hitDelayMs));
        ListCell(list, keywordRow, 3, PowerKeywordActionLabel(rule.missAction));
        ListCell(list, keywordRow, 4, rule.missAction == PowerKeywordAction::None ? L"0" : FormatMsLabel(rule.missDelayMs));
        ListCell(list, keywordRow, 5, rule.begin.empty() ? L"请设置关键字开始检测字符" : rule.begin);
        ListCell(list, keywordRow, 6, rule.end.empty() ? L"请设置关键字结束检测字符" : rule.end);

        if (expanded) {
            for (size_t c = 0; c < rule.commands.size(); ++c) {
                const auto &cmd = rule.commands[c];
                const int cmdRow = static_cast<int>(data.rows.size());
                data.rows.push_back({ConfigRow::Kind::Command, r, c});
                if (static_cast<int>(r) == targetRuleIndex && static_cast<int>(c) == targetCmdIndex)
                    selectVisualRow = cmdRow;

                const std::wstring label = L"    └ 命令" + std::to_wstring(c) + L" · " + cmd.text;
                const std::wstring triggerAndInterval = std::wstring(TriggerShortLabel(cmd.trigger)) +
                    (cmd.intervalMs ? L" · " + FormatMsLabel(cmd.intervalMs, false) : L"");
                const bool runsOnHit = cmd.trigger != PowerKeywordCommandTrigger::OnMiss;
                const bool runsOnMiss = cmd.trigger != PowerKeywordCommandTrigger::OnHit;
                ListCell(list, cmdRow, 0, label);
                ListCell(list, cmdRow, 1, triggerAndInterval);
                ListCell(list, cmdRow, 2, runsOnHit ? FormatMsLabel(cmd.delayMs) : L"—");
                ListCell(list, cmdRow, 3, runsOnMiss ? cmd.text : L"—");
                ListCell(list, cmdRow, 4, runsOnMiss ? FormatMsLabel(cmd.delayMs) : L"—");
                ListCell(list, cmdRow, 5, L"—");
                ListCell(list, cmdRow, 6, L"—");
            }
        }
    }

    if (selectVisualRow >= 0 && selectVisualRow < ListView_GetItemCount(list)) {
        ListView_SetItemState(list, selectVisualRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selectVisualRow, FALSE);
    } else if (!data.rows.empty() && targetRuleIndex < 0) {
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        selectVisualRow = 0;
    } else {
        selectVisualRow = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    }
    UpdateConfigButtonStates(dialog, data, selectVisualRow);
}

COLORREF ActionCellColor(const UiColors &theme, PowerKeywordAction action) {
    switch (action) {
    case PowerKeywordAction::None: return theme.muted;
    case PowerKeywordAction::RunCommands:
    case PowerKeywordAction::PowerOffContinue: return theme.accent;
    case PowerKeywordAction::StopTest:
    case PowerKeywordAction::StopTestPowerOff:
    case PowerKeywordAction::RunCommandsStopTest:
    case PowerKeywordAction::PauseAll: return theme.danger;
    }
    return theme.text;
}

int ActionToIndex(PowerKeywordAction action) {
    for (int i = 0; i < static_cast<int>(std::size(kActions)); ++i)
        if (kActions[i] == action) return i;
    return 0;
}

PowerKeywordAction IndexToAction(int index) {
    if (index >= 0 && index < static_cast<int>(std::size(kActions))) return kActions[index];
    return PowerKeywordAction::None;
}

} // namespace

Json PowerPane::EncodeKeywords(const PowerKeywordConfig &config) {
    auto rules = Json::array();
    for (const auto &rule : config.rules) {
        auto commands = Json::array();
        for (const auto &cmd : rule.commands) {
            commands.push_back({{"text", WideToMultiByte(cmd.text, CP_UTF8)},
                                {"delayMs", cmd.delayMs},
                                {"intervalMs", cmd.intervalMs},
                                {"trigger", TriggerKey(cmd.trigger)}});
        }
        rules.push_back({{"keyword", WideToMultiByte(rule.keyword, CP_UTF8)},
                         {"onHit", WideToMultiByte(rule.onHit, CP_UTF8)},
                         {"hitDelayMs", rule.hitDelayMs},
                         {"onMiss", WideToMultiByte(rule.onMiss, CP_UTF8)},
                         {"missDelayMs", rule.missDelayMs},
                         {"begin", WideToMultiByte(rule.begin, CP_UTF8)},
                         {"end", WideToMultiByte(rule.end, CP_UTF8)},
                         {"hitAction", PowerKeywordActionKey(rule.hitAction)},
                         {"missAction", PowerKeywordActionKey(rule.missAction)},
                         {"commands", std::move(commands)}});
    }
    return {{"ready", WideToMultiByte(config.ready, CP_UTF8)}, {"rules", std::move(rules)}};
}

PowerKeywordConfig PowerPane::DecodeKeywords(const Json &config) {
    PowerKeywordConfig result;
    result.ready = WideText(config.at("ready").get<std::string>());
    if (!ValidField(result.ready) || !config.at("rules").is_array())
        throw std::runtime_error("关键字配置格式无效");
    for (const auto &item : config.at("rules")) {
        PowerKeywordRule rule{
            WideText(item.at("keyword").get<std::string>()),
            WideText(item.value("onHit", std::string())),
            WideText(item.value("onMiss", std::string())),
            WideText(item.value("begin", std::string())),
            WideText(item.value("end", std::string())),
            DelayValue(item.at("hitDelayMs")),
            DelayValue(item.at("missDelayMs"))};
        if (item.contains("hitAction") && item["hitAction"].is_string())
            rule.hitAction = ParsePowerKeywordAction(item["hitAction"].get<std::string>());
        else if (!rule.onHit.empty())
            rule.hitAction = PowerKeywordAction::RunCommands;

        if (item.contains("missAction") && item["missAction"].is_string())
            rule.missAction = ParsePowerKeywordAction(item["missAction"].get<std::string>());
        else if (!rule.onMiss.empty())
            rule.missAction = PowerKeywordAction::RunCommands;

        if (item.contains("commands")) {
            if (!item["commands"].is_array()) throw std::runtime_error("关键字命令列表格式无效");
            for (const auto &entry : item["commands"]) {
                PowerKeywordCommand cmd{
                    WideText(entry.at("text").get<std::string>()),
                    entry.contains("delayMs") ? DelayValue(entry["delayMs"]) : 0,
                    entry.contains("intervalMs") ? DelayValue(entry["intervalMs"]) : 0,
                    entry.contains("trigger") && entry["trigger"].is_string()
                        ? ParseTriggerKey(entry["trigger"].get<std::string>())
                        : PowerKeywordCommandTrigger::OnHit};
                rule.commands.push_back(std::move(cmd));
            }
        } else {
            if (!rule.onHit.empty())
                rule.commands.push_back({rule.onHit, 0, 0, PowerKeywordCommandTrigger::OnHit});
            if (!rule.onMiss.empty())
                rule.commands.push_back({rule.onMiss, 0, 0, PowerKeywordCommandTrigger::OnMiss});
        }
        Validate(rule);
        result.rules.push_back(std::move(rule));
    }
    return result;
}

void PowerPane::ObserveKeywords(std::uint64_t session, const Bytes &bytes) {
    if (keywordSession_.load(std::memory_order_relaxed) != session) return;
    const auto run = std::atomic_load(&keywordRun_);
    if (run && run->Session() == session) run->Feed(bytes);
}

bool PowerPane::KeywordListening(std::uint64_t session) const {
    if (keywordSession_.load(std::memory_order_relaxed) != session) return false;
    const auto run = std::atomic_load(&keywordRun_);
    return run && run->Session() == session && run->Active();
}

void PowerPane::KeywordTargetDisconnected(std::uint64_t session) {
    const auto run = std::atomic_load(&keywordRun_);
    if (run && run->Session() == session) run->Cancel(L"关联终端已断开，旧任务已停止");
}

void PowerPane::ThemeKeywords() {
    ThemeList(Control(KeywordStatsId), dark_);
    const auto c = UiTheme(dark_);
    UiSetControlBackdrop(Control(KeywordConfigId), c.surface);
    UiSetControlBackdrop(Control(KeywordResumeId), c.surface);
}

void PowerPane::LayoutKeywords(int width) {
    HWND list = Control(KeywordStatsId);
    if (!list) return;
    auto d = [this](int value) { return MulDiv(value, dpi_, 96); };
    const int result = std::clamp(width / 4, 68, 120);
    const int counter = std::clamp(width / 9, 44, 66);
    const int written = std::max(56, counter);
    const int first = std::max(60, width - result - counter * 2 - written - 20);
    const int widths[]{first, counter, counter, written, result};
    const wchar_t *headers[]{L"关键字列表", L"捕获", L"丢失", L"已写入", L"本轮结果"};
    for (int i = 0; i < 5; ++i) {
        if (Header_GetItemCount(ListView_GetHeader(list)) <= i)
            Column(list, i, headers[i], d(widths[i]), i >= 1 && i <= 3 ? LVCFMT_RIGHT : LVCFMT_LEFT);
        else ListView_SetColumnWidth(list, i, d(widths[i]));
    }
}

void PowerPane::RefreshKeywords() {
    HWND list = Control(KeywordStatsId);
    if (!list) return;
    const auto run = std::atomic_load(&keywordRun_);
    const auto snapshot = run ? run->Status() : PowerKeywordRun::Snapshot{};
    const auto &terminal = run ? snapshot.target : recoveryLabel_;
    const wchar_t *label = run && !run->Active() && snapshot.round ? L"上次监听：" : L"监听终端：";
    std::wstring summary = label + (terminal.empty() ? std::wstring(L"请在右侧选择") : terminal);
    if (snapshot.round) summary += L" · 第 " + std::to_wstring(snapshot.round) + L" 轮";
    if (run && snapshot.state == PowerKeywordRun::State::Paused) summary += L" · 已暂停";
    else if (run && snapshot.ready) summary += L" · 上电完成";
    else if (run && snapshot.timedOut) summary += L" · 上电标志超时";
    else if (run && snapshot.readyExpected && run->Active()) summary += L" · 等待上电标志";
    if (!snapshot.reason.empty()) summary += L"\r\n" + snapshot.reason;
    else if (snapshot.state == PowerKeywordRun::State::Draining) summary += L"\r\n等待动作写入后掉电";
    else if (remote_.Active()) summary += L"\r\n关键字检测使用本机 USB 或 RS232 电源";
    else if (!keywordConfigError_.empty()) summary += L"\r\n" + keywordConfigError_;
    else if (!keywordConfig_.ready.empty()) summary += L"\r\n上电完成标志：" + keywordConfig_.ready;
    SetTextIfChanged(Control(KeywordStatusId), summary);
    const auto count = static_cast<int>(keywordConfig_.rules.size());
    while (ListView_GetItemCount(list) > count) ListView_DeleteItem(list, ListView_GetItemCount(list) - 1);
    for (int i = 0; i < count; ++i) {
        const bool hasCount = static_cast<size_t>(i) < snapshot.counts.size() && snapshot.round;
        const auto result = hasCount ? snapshot.counts[i] : PowerKeywordRun::Count{};
        std::wstring state = L"—";
        using Window = PowerKeywordRun::Window;
        if (hasCount) {
            if (result.failed) state = L"发送失败";
            else if (result.canceled) state = L"有动作取消";
            else if (snapshot.state == PowerKeywordRun::State::Paused) state = L"已暂停";
            else if (result.round) state = L"已捕获";
            else if (result.window == Window::Closed) state = L"未捕获";
            else if (result.window == Window::Interrupted) state = L"检测中断";
            else if (result.window == Window::Open) state = L"检测中";
            else state = snapshot.state == PowerKeywordRun::State::Listening ? L"等待开始" : L"未进入检测";
        }
        ListCell(list, i, 0, keywordConfig_.rules[i].keyword);
        ListCell(list, i, 1, std::to_wstring(result.total));
        ListCell(list, i, 2, std::to_wstring(result.missTotal));
        ListCell(list, i, 3, std::to_wstring(result.written));
        ListCell(list, i, 4, state);
    }
}

void PowerPane::ConfigureKeywords() {
    if (const auto run = std::atomic_load(&keywordRun_); run && run->Active()) return;
    ConfigDialog data{{dpi_, dark_, font_, surfaceBrush_, brush_, &fieldStyle_}, keywordConfig_, {}, {}};
    data.expanded.assign(data.config.rules.size(), true);
    DialogTemplate resource;
    if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &resource.dialog, GetAncestor(window_, GA_ROOT),
                                 KeywordConfigProc, reinterpret_cast<LPARAM>(&data)) == IDOK)
        SaveKeywords(std::move(data.config));
}

INT_PTR CALLBACK PowerPane::KeywordConfigProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    auto *data = reinterpret_cast<ConfigDialog *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<ConfigDialog *>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetWindowTextW(dialog, L"关键字配置");
        const auto &ui = data->ui;
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
        const int maxWidth = MulDiv(monitor.rcWork.right - monitor.rcWork.left, 96, ui.dpi) - 40;
        const int width = std::clamp(maxWidth, 680, 980);
        const int height = 438;
        PositionDialog(dialog, ui, width, height);

        AddControl(dialog, ui, L"STATIC", L"上电完成标志信息", -1, SS_CENTERIMAGE | SS_NOPREFIX, 16, 16, 128, 28);
        AddControl(dialog, ui, L"EDIT", data->config.ready.c_str(), ReadyId, WS_TABSTOP | ES_AUTOHSCROLL,
                   152, 16, width - 168, 28);

        auto list = AddControl(dialog, ui, WC_LISTVIEWW, L"关键字规则", RulesId,
                               WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               16, 56, width - 32, 266);
        ThemeList(list, ui.dark);
        const wchar_t *headers[]{
            L"关键字 (双击编辑)", L"捕获动作", L"捕获延迟(ms)",
            L"丢失动作", L"丢失延迟(ms)", L"开始检测标志", L"结束检测标志"
        };
        const int tableInner = std::max(648, width - 36);
        const int w0 = std::max(168, tableInner * 22 / 100);
        const int w1 = std::max(100, tableInner * 12 / 100);
        const int w2 = std::max(114, tableInner * 13 / 100);
        const int w3 = std::max(100, tableInner * 12 / 100);
        const int w4 = std::max(114, tableInner * 13 / 100);
        const int w5 = std::max(120, tableInner * 14 / 100);
        const int w6 = std::max(120, tableInner - w0 - w1 - w2 - w3 - w4 - w5);
        const int widths[]{w0, w1, w2, w3, w4, w5, w6};
        for (int i = 0; i < 7; ++i) Column(list, i, headers[i], ui.Scale(widths[i]));

        const int toolsY = 336;
        struct ToolButton { int id; const wchar_t *text; int w; };
        const ToolButton tools[]{
            {AddId, L"添加关键字", 112},
            {AddCmdId, L"添加命令", 96},
            {DeleteId, L"移除", 76},
            {ClearId, L"移除所有", 96},
            {LoadFileId, L"加载关键字", 112},
            {SaveFileId, L"保存关键字", 112},
        };
        int toolX = 16;
        for (const auto &btn : tools) {
            AddButton(dialog, ui, btn.id, btn.text, toolX, toolsY, btn.w, 32);
            toolX += btn.w + 8;
        }
        const int footerY = 386;
        AddButton(dialog, ui, IDCANCEL, L"取消", width - 212, footerY, 92, 34);
        AddButton(dialog, ui, IDOK, L"确定", width - 108, footerY, 92, 34);

        PopulateRules(dialog, *data, 0, -1);
        return TRUE;
    }
    if (!data) return FALSE;
    HWND list = GetDlgItem(dialog, RulesId);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    int action = message == WM_COMMAND ? LOWORD(w) : 0;

    if (message == WM_NOTIFY) {
        const auto &notice = *reinterpret_cast<NMHDR *>(l);
        if (notice.idFrom == RulesId && notice.code == NM_CUSTOMDRAW) {
            auto &draw = *reinterpret_cast<NMLVCUSTOMDRAW *>(l);
            const auto theme = UiTheme(data->ui.dark);
            if (draw.nmcd.dwDrawStage == CDDS_PREPAINT) {
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            }
            if (draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                return TRUE;
            }
            if (draw.nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                const auto rowIdx = static_cast<size_t>(draw.nmcd.dwItemSpec);
                const bool isCmd = rowIdx < data->rows.size() && data->rows[rowIdx].kind == ConfigRow::Kind::Command;
                draw.clrTextBk = isCmd ? theme.raised : ((rowIdx % 2 == 1) ? theme.field : theme.surface);
                draw.clrText = theme.text;
                if (rowIdx < data->rows.size()) {
                    const auto &row = data->rows[rowIdx];
                    const auto &rule = data->config.rules[row.ruleIndex];
                    const int col = draw.iSubItem;
                    if (row.kind == ConfigRow::Kind::Keyword) {
                        if (col == 1) draw.clrText = ActionCellColor(theme, rule.hitAction);
                        else if (col == 3) draw.clrText = ActionCellColor(theme, rule.missAction);
                        else if ((col == 5 && rule.begin.empty()) || (col == 6 && rule.end.empty()))
                            draw.clrText = theme.muted;
                    } else {
                        if (col == 0) draw.clrText = theme.text;
                        else if (col == 1 || col == 2 || col == 3 || col == 4) draw.clrText = theme.accent;
                        else draw.clrText = theme.muted;
                    }
                }
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, CDRF_NEWFONT);
                return TRUE;
            }
        }
        if (notice.idFrom == RulesId && notice.code == LVN_ITEMCHANGED) {
            const int nextSel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            UpdateConfigButtonStates(dialog, *data, nextSel);
        }
        if (notice.idFrom == RulesId && notice.code == NM_CLICK) {
            const auto &hit = *reinterpret_cast<NMITEMACTIVATE *>(l);
            if (hit.iItem >= 0 && static_cast<size_t>(hit.iItem) < data->rows.size()) {
                const auto &row = data->rows[static_cast<size_t>(hit.iItem)];
                if (row.kind == ConfigRow::Kind::Keyword && hit.iSubItem == 0 &&
                    !data->config.rules[row.ruleIndex].commands.empty()) {
                    RECT itemRect{};
                    ListView_GetSubItemRect(list, hit.iItem, 0, LVIR_BOUNDS, &itemRect);
                    if (hit.ptAction.x <= itemRect.left + data->ui.Scale(24)) {
                        data->expanded[row.ruleIndex] = !data->expanded[row.ruleIndex];
                        PopulateRules(dialog, *data, static_cast<int>(row.ruleIndex), -1);
                        return TRUE;
                    }
                }
            }
        }
        if (notice.idFrom == RulesId && notice.code == NM_DBLCLK) {
            const auto &hit = *reinterpret_cast<NMITEMACTIVATE *>(l);
            if (hit.iItem >= 0 && static_cast<size_t>(hit.iItem) < data->rows.size()) {
                const auto row = data->rows[static_cast<size_t>(hit.iItem)];
                if (row.kind == ConfigRow::Kind::Keyword && (hit.iSubItem == 1 || hit.iSubItem == 3)) {
                    const bool isMiss = hit.iSubItem == 3;
                    HMENU menu = CreatePopupMenu();
                    const PowerKeywordAction current = isMiss ? data->config.rules[row.ruleIndex].missAction
                                                              : data->config.rules[row.ruleIndex].hitAction;
                    for (int i = 0; i < static_cast<int>(std::size(kActions)); ++i) {
                        const UINT flags = MF_STRING | (kActions[i] == current ? MF_CHECKED : 0);
                        AppendMenuW(menu, flags, static_cast<UINT_PTR>(i + 1), PowerKeywordActionLabel(kActions[i]));
                    }
                    RECT cellRect{};
                    ListView_GetSubItemRect(list, hit.iItem, hit.iSubItem, LVIR_BOUNDS, &cellRect);
                    POINT anchor{cellRect.left, cellRect.bottom};
                    ClientToScreen(list, &anchor);
                    const UINT picked = UiPopupMenu::Track(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                           anchor.x, anchor.y, dialog, *data->ui.style);
                    DestroyMenu(menu);
                    if (picked >= 1 && picked <= static_cast<UINT>(std::size(kActions))) {
                        auto &targetRule = data->config.rules[row.ruleIndex];
                        const auto chosen = kActions[picked - 1];
                        if (isMiss) targetRule.missAction = chosen;
                        else targetRule.hitAction = chosen;
                        PopulateRules(dialog, *data, static_cast<int>(row.ruleIndex), -1);
                    }
                    return TRUE;
                }
                if (row.kind == ConfigRow::Kind::Keyword) {
                    RuleDialog dlg{data->ui, data->config.rules[row.ruleIndex], hit.iSubItem};
                    DialogTemplate res;
                    if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &res.dialog, dialog, KeywordRuleProc,
                                                 reinterpret_cast<LPARAM>(&dlg)) == IDOK) {
                        data->config.rules[row.ruleIndex] = std::move(dlg.rule);
                        PopulateRules(dialog, *data, static_cast<int>(row.ruleIndex), -1);
                    }
                    return TRUE;
                }
                if (row.kind == ConfigRow::Kind::Command) {
                    CommandDialog dlg{data->ui, data->config.rules[row.ruleIndex].commands[row.commandIndex]};
                    DialogTemplate res;
                    if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &res.dialog, dialog, KeywordCommandProc,
                                                 reinterpret_cast<LPARAM>(&dlg)) == IDOK) {
                        data->config.rules[row.ruleIndex].commands[row.commandIndex] = std::move(dlg.command);
                        PopulateRules(dialog, *data, static_cast<int>(row.ruleIndex),
                                      static_cast<int>(row.commandIndex));
                    }
                    return TRUE;
                }
            }
        }
    }

    if (action == AddId) {
        RuleDialog rule{data->ui, PowerKeywordRule{}, 0};
        DialogTemplate resource;
        if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &resource.dialog, dialog, KeywordRuleProc,
                                     reinterpret_cast<LPARAM>(&rule)) == IDOK) {
            data->config.rules.push_back(std::move(rule.rule));
            data->expanded.push_back(true);
            PopulateRules(dialog, *data, static_cast<int>(data->config.rules.size()) - 1, -1);
        }
        return TRUE;
    }
    if (action == AddCmdId && selected >= 0 && static_cast<size_t>(selected) < data->rows.size()) {
        const size_t ruleIndex = data->rows[static_cast<size_t>(selected)].ruleIndex;
        PowerKeywordCommand draft{};
        if (data->config.rules[ruleIndex].missAction == PowerKeywordAction::RunCommands &&
            data->config.rules[ruleIndex].hitAction == PowerKeywordAction::None)
            draft.trigger = PowerKeywordCommandTrigger::OnMiss;
        CommandDialog cmd{data->ui, std::move(draft)};
        DialogTemplate resource;
        if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &resource.dialog, dialog, KeywordCommandProc,
                                     reinterpret_cast<LPARAM>(&cmd)) == IDOK) {
            data->config.rules[ruleIndex].commands.push_back(std::move(cmd.command));
            if (ruleIndex < data->expanded.size()) data->expanded[ruleIndex] = true;
            if (cmd.command.trigger == PowerKeywordCommandTrigger::OnHit &&
                data->config.rules[ruleIndex].hitAction == PowerKeywordAction::None)
                data->config.rules[ruleIndex].hitAction = PowerKeywordAction::RunCommands;
            else if (cmd.command.trigger == PowerKeywordCommandTrigger::OnMiss &&
                     data->config.rules[ruleIndex].missAction == PowerKeywordAction::None)
                data->config.rules[ruleIndex].missAction = PowerKeywordAction::RunCommands;
            PopulateRules(dialog, *data, static_cast<int>(ruleIndex),
                          static_cast<int>(data->config.rules[ruleIndex].commands.size()) - 1);
        }
        return TRUE;
    }
    if (action == DeleteId && selected >= 0 && static_cast<size_t>(selected) < data->rows.size()) {
        const auto row = data->rows[static_cast<size_t>(selected)];
        if (row.kind == ConfigRow::Kind::Command) {
            auto &cmds = data->config.rules[row.ruleIndex].commands;
            cmds.erase(cmds.begin() + static_cast<ptrdiff_t>(row.commandIndex));
            const int nextCmd = cmds.empty() ? -1 : std::min(static_cast<int>(row.commandIndex),
                                                             static_cast<int>(cmds.size()) - 1);
            PopulateRules(dialog, *data, static_cast<int>(row.ruleIndex), nextCmd);
        } else {
            data->config.rules.erase(data->config.rules.begin() + static_cast<ptrdiff_t>(row.ruleIndex));
            if (row.ruleIndex < data->expanded.size())
                data->expanded.erase(data->expanded.begin() + static_cast<ptrdiff_t>(row.ruleIndex));
            const int nextRule = data->config.rules.empty() ? -1
                : std::min(static_cast<int>(row.ruleIndex), static_cast<int>(data->config.rules.size()) - 1);
            PopulateRules(dialog, *data, nextRule, -1);
        }
        return TRUE;
    }
    if (action == ClearId && !data->config.rules.empty()) {
        data->config.rules.clear();
        data->expanded.clear();
        PopulateRules(dialog, *data, -1, -1);
        return TRUE;
    }
    if (action == LoadFileId) {
        const auto path = ChooseKeywordJsonFile(dialog, false);
        if (!path.empty()) {
            try {
                data->config = ReadKeywordFile(path);
                data->expanded.assign(data->config.rules.size(), true);
                SetTextIfChanged(GetDlgItem(dialog, ReadyId), data->config.ready);
                PopulateRules(dialog, *data, 0, -1);
            } catch (const std::exception &error) {
                MessageBoxW(dialog, WideText(error.what()).c_str(), L"加载关键字配置", MB_OK | MB_ICONWARNING);
            }
        }
        return TRUE;
    }
    if (action == SaveFileId) {
        data->config.ready = DialogText(GetDlgItem(dialog, ReadyId));
        if (!ValidField(data->config.ready)) {
            MessageBoxW(dialog, L"上电完成标志须为单行文本", L"保存关键字配置", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        const auto path = ChooseKeywordJsonFile(dialog, true);
        if (!path.empty()) {
            try {
                WriteKeywordFile(path, data->config);
            } catch (const std::exception &error) {
                MessageBoxW(dialog, WideText(error.what()).c_str(), L"保存关键字配置", MB_OK | MB_ICONWARNING);
            }
        }
        return TRUE;
    }
    if (action == IDOK) {
        data->config.ready = DialogText(GetDlgItem(dialog, ReadyId));
        if (!ValidField(data->config.ready)) {
            MessageBoxW(dialog, L"上电完成标志须为单行文本", L"关键字配置", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        UiEndOwnedDialog(dialog, IDOK);
        return TRUE;
    }
    if (action == IDCANCEL || message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return PaintDialog(dialog, data->ui, message, w, l);
}

INT_PTR CALLBACK PowerPane::KeywordRuleProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    auto *data = reinterpret_cast<RuleDialog *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<RuleDialog *>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetWindowTextW(dialog, L"关键字规则");
        const auto &ui = data->ui;
        const auto &rule = data->rule;
        PositionDialog(dialog, ui, 504, 336);

        std::vector<std::wstring> actionLabels;
        actionLabels.reserve(std::size(kActions));
        for (const auto item : kActions) actionLabels.emplace_back(PowerKeywordActionLabel(item));

        AddField(dialog, ui, RuleFieldId, L"关键字", rule.keyword, 16, 14, 472);
        AddComboField(dialog, ui, RuleFieldId + 1, L"捕获动作", actionLabels, ActionToIndex(rule.hitAction), 16, 78, 312);
        AddField(dialog, ui, RuleFieldId + 2, L"捕获动作延迟 (ms)", std::to_wstring(rule.hitDelayMs), 340, 78, 148, true);
        AddComboField(dialog, ui, RuleFieldId + 3, L"丢失动作", actionLabels, ActionToIndex(rule.missAction), 16, 142, 312);
        AddField(dialog, ui, RuleFieldId + 4, L"丢失动作延迟 (ms)", std::to_wstring(rule.missDelayMs), 340, 142, 148, true);
        AddField(dialog, ui, RuleFieldId + 5, L"关键字检测开始标志（可留空）", rule.begin, 16, 206, 230);
        AddField(dialog, ui, RuleFieldId + 6, L"关键字检测结束标志（可留空）", rule.end, 258, 206, 230);
        AddButton(dialog, ui, IDCANCEL, L"取消", 280, 282, 98, 34);
        AddButton(dialog, ui, IDOK, L"确定", 390, 282, 98, 34);

        const int focusId = RuleFieldId + std::clamp(data->initialFocusField, 0, 6);
        if (HWND target = GetDlgItem(dialog, focusId)) SetFocus(target);
        return FALSE;
    }
    if (!data) return FALSE;
    const int action = message == WM_COMMAND ? LOWORD(w) : 0;
    if (action == IDOK) {
        try {
            auto field = [&](int index) { return DialogText(GetDlgItem(dialog, RuleFieldId + index)); };
            auto delay = [&](int index) {
                const auto text = field(index);
                if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error("请输入有效的动作延迟毫秒数");
                const auto value = std::stoul(text);
                if (value > MaximumDelay) throw std::runtime_error("动作延迟超出范围");
                return static_cast<unsigned>(value);
            };
            PowerKeywordRule rule = data->rule;
            rule.keyword = field(0);
            rule.hitAction = IndexToAction(static_cast<int>(SendMessageW(GetDlgItem(dialog, RuleFieldId + 1), CB_GETCURSEL, 0, 0)));
            rule.hitDelayMs = delay(2);
            rule.missAction = IndexToAction(static_cast<int>(SendMessageW(GetDlgItem(dialog, RuleFieldId + 3), CB_GETCURSEL, 0, 0)));
            rule.missDelayMs = delay(4);
            rule.begin = field(5);
            rule.end = field(6);
            Validate(rule);
            data->rule = std::move(rule);
            UiEndOwnedDialog(dialog, IDOK);
        } catch (const std::exception &error) {
            MessageBoxW(dialog, WideText(error.what()).c_str(), L"关键字规则", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    if (action == IDCANCEL || message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return PaintDialog(dialog, data->ui, message, w, l);
}

INT_PTR CALLBACK PowerPane::KeywordCommandProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    auto *data = reinterpret_cast<CommandDialog *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<CommandDialog *>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetWindowTextW(dialog, L"子命令配置");
        const auto &ui = data->ui;
        const auto &cmd = data->command;
        PositionDialog(dialog, ui, 436, 266);

        AddField(dialog, ui, CmdFieldId, L"发送命令内容", cmd.text, 16, 14, 404);
        AddComboField(dialog, ui, CmdFieldId + 1, L"触发时机",
                      {TriggerLabel(PowerKeywordCommandTrigger::OnHit),
                       TriggerLabel(PowerKeywordCommandTrigger::OnMiss),
                       TriggerLabel(PowerKeywordCommandTrigger::Both)},
                      static_cast<int>(cmd.trigger), 16, 78, 196);
        AddField(dialog, ui, CmdFieldId + 2, L"命令延迟 (ms)", std::to_wstring(cmd.delayMs), 224, 78, 92, true);
        AddField(dialog, ui, CmdFieldId + 3, L"发送间隔 (ms)", std::to_wstring(cmd.intervalMs), 328, 78, 92, true);
        AddButton(dialog, ui, IDCANCEL, L"取消", 212, 212, 98, 34);
        AddButton(dialog, ui, IDOK, L"确定", 322, 212, 98, 34);
        if (HWND target = GetDlgItem(dialog, CmdFieldId)) SetFocus(target);
        return FALSE;
    }
    if (!data) return FALSE;
    const int action = message == WM_COMMAND ? LOWORD(w) : 0;
    if (action == IDOK) {
        try {
            auto field = [&](int index) { return DialogText(GetDlgItem(dialog, CmdFieldId + index)); };
            auto parseMs = [&](int index, const char *err) {
                const auto text = field(index);
                if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error(err);
                const auto value = std::stoul(text);
                if (value > MaximumDelay) throw std::runtime_error("时间超出范围");
                return static_cast<unsigned>(value);
            };
            PowerKeywordCommand cmd;
            cmd.text = field(0);
            if (cmd.text.empty() ||
                std::all_of(cmd.text.begin(), cmd.text.end(), [](wchar_t ch) { return std::iswspace(ch) != 0; }))
                throw std::runtime_error("请输入命令内容");
            if (!ValidField(cmd.text)) throw std::runtime_error("子命令须为单行文本");
            const int trig = static_cast<int>(SendMessageW(GetDlgItem(dialog, CmdFieldId + 1), CB_GETCURSEL, 0, 0));
            cmd.trigger = trig == 1 ? PowerKeywordCommandTrigger::OnMiss
                        : trig == 2 ? PowerKeywordCommandTrigger::Both
                                    : PowerKeywordCommandTrigger::OnHit;
            cmd.delayMs = parseMs(2, "请输入有效的命令延迟毫秒数");
            cmd.intervalMs = parseMs(3, "请输入有效的发送间隔毫秒数");
            data->command = std::move(cmd);
            UiEndOwnedDialog(dialog, IDOK);
        } catch (const std::exception &error) {
            MessageBoxW(dialog, WideText(error.what()).c_str(), L"子命令配置", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    if (action == IDCANCEL || message == WM_CLOSE) {
        UiEndOwnedDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return PaintDialog(dialog, data->ui, message, w, l);
}

} // namespace serialctl
