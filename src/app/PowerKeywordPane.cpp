#include "PowerPane.h"
#include "Win32Helpers.h"
#include <commctrl.h>
#include <cwctype>

namespace serialctl {
namespace {
constexpr int ReadyId = 301, RulesId = 302, AddId = 303, EditId = 304, DeleteId = 305;
constexpr int RuleFieldId = 310;
constexpr unsigned MaximumDelay = 604800000;
constexpr int TextLimit = 4096;

struct Palette {
    int dpi;
    bool dark;
    HFONT font;
    HBRUSH background, field;
    UiFieldStyle *style;
    int Scale(int value) const { return MulDiv(value, dpi, 96); }
};
struct ConfigDialog { Palette ui; PowerKeywordConfig config; };
struct RuleDialog { Palette ui; PowerKeywordRule rule; };

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
}

unsigned DelayValue(const Json &value) {
    if (!value.is_number_integer() || value < 0 || value > MaximumDelay)
        throw std::runtime_error("动作延迟超出范围");
    return value.get<unsigned>();
}

void SetTextIfChanged(HWND control, const std::wstring &text) {
    if (!control) return;
    std::wstring current(GetWindowTextLengthW(control) + 1, L'\0');
    current.resize(GetWindowTextW(control, current.data(), static_cast<int>(current.size())));
    if (text != current) SetWindowTextW(control, text.c_str());
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
    const int padding = MulDiv(4, GetDeviceCaps(dc, LOGPIXELSX), 96);
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
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
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
    if (_wcsicmp(cls, L"BUTTON") == 0) UiStyleButton(control);
    if (_wcsicmp(cls, L"EDIT") == 0) {
        UiStyleField(control, ui.style);
        SendMessageW(control, EM_SETLIMITTEXT, (style & ES_NUMBER) ? 9 : TextLimit, 0);
    }
    return control;
}

void AddButton(HWND parent, const Palette &ui, int id, const wchar_t *text, int x, int y) {
    AddControl(parent, ui, L"BUTTON", text, id, WS_TABSTOP | BS_OWNERDRAW, x, y, 96, 36);
}

void AddField(HWND parent, const Palette &ui, int id, const wchar_t *label, const std::wstring &value,
              int x, int y, int width, bool number = false) {
    AddControl(parent, ui, L"STATIC", label, -1, SS_NOPREFIX, x, y, width, 24);
    AddControl(parent, ui, L"EDIT", value.c_str(), id, WS_TABSTOP | ES_AUTOHSCROLL | (number ? ES_NUMBER : 0),
               x, y + 24, width, 28);
}

INT_PTR PaintDialog(HWND dialog, const Palette &ui, UINT message, WPARAM w, LPARAM l) {
    const auto theme = UiTheme(ui.dark);
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
        SetTextColor(reinterpret_cast<HDC>(w), theme.text);
        SetBkColor(reinterpret_cast<HDC>(w), message == WM_CTLCOLOREDIT ? theme.field : theme.surface);
        return reinterpret_cast<INT_PTR>(message == WM_CTLCOLOREDIT ? ui.field : ui.background);
    }
    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(dialog, &rect);
        FillRect(reinterpret_cast<HDC>(w), &rect, ui.background);
        return TRUE;
    }
    if (message != WM_DRAWITEM) return FALSE;
    auto item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
    UiBufferedPaint paint(item.hDC, item.rcItem);
    item.hDC = paint.Dc();
    FillRect(item.hDC, &item.rcItem, ui.background);
    const bool enabled = !(item.itemState & ODS_DISABLED), pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = enabled && item.CtlID == IDOK;
    const auto visual = UiButtonVisual(theme, enabled, pressed,
                                       GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, primary);
    UiBox(item.hDC, item.rcItem, visual.fill, visual.border, UiCapsuleRadius(item.rcItem));
    UiButtonKeyboardFocus(item.hDC, item.rcItem, item.itemState, ui.dpi, UiFocusInk(ui.dark, enabled && (primary || pressed)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, visual.ink);
    auto old = SelectObject(item.hDC, ui.font);
    wchar_t title[64]{};
    GetWindowTextW(item.hwndItem, title, static_cast<int>(std::size(title)));
    DrawTextW(item.hDC, title, -1, &item.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, old);
    return TRUE;
}

void PopulateRules(HWND dialog, const PowerKeywordConfig &config, int selected = -1) {
    HWND list = GetDlgItem(dialog, RulesId);
    UiRedrawScope redraw(list);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < config.rules.size(); ++i) {
        const auto &rule = config.rules[i];
        const auto row = static_cast<int>(i);
        ListCell(list, row, 0, rule.keyword);
        ListCell(list, row, 1, rule.onHit.empty() ? L"仅计数" : rule.onHit);
        ListCell(list, row, 2, rule.onHit.empty() ? L"—" : std::to_wstring(rule.hitDelayMs) + L" ms");
        ListCell(list, row, 3, rule.onMiss.empty() ? L"无操作" : rule.onMiss);
        ListCell(list, row, 4, rule.onMiss.empty() ? L"—" : std::to_wstring(rule.missDelayMs) + L" ms");
        ListCell(list, row, 5, rule.begin.empty() ? L"本轮开始" : rule.begin);
        ListCell(list, row, 6, rule.end.empty() ? L"本轮结束" : rule.end);
    }
    if (selected >= 0 && selected < ListView_GetItemCount(list)) {
        ListView_SetItemState(list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selected, FALSE);
    }
    const bool hasSelection = ListView_GetNextItem(list, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(dialog, EditId), hasSelection);
    EnableWindow(GetDlgItem(dialog, DeleteId), hasSelection);
}

} // namespace

Json PowerPane::EncodeKeywords(const PowerKeywordConfig &config) {
    auto rules = Json::array();
    for (const auto &rule : config.rules)
        rules.push_back({{"keyword", WideToMultiByte(rule.keyword, CP_UTF8)}, {"onHit", WideToMultiByte(rule.onHit, CP_UTF8)},
                         {"hitDelayMs", rule.hitDelayMs}, {"onMiss", WideToMultiByte(rule.onMiss, CP_UTF8)},
                         {"missDelayMs", rule.missDelayMs}, {"begin", WideToMultiByte(rule.begin, CP_UTF8)},
                         {"end", WideToMultiByte(rule.end, CP_UTF8)}});
    return {{"ready", WideToMultiByte(config.ready, CP_UTF8)}, {"rules", std::move(rules)}};
}

PowerKeywordConfig PowerPane::DecodeKeywords(const Json &config) {
    PowerKeywordConfig result;
    result.ready = WideText(config.at("ready").get<std::string>());
    if (!ValidField(result.ready) || !config.at("rules").is_array())
        throw std::runtime_error("关键字配置格式无效");
    for (const auto &item : config.at("rules")) {
        PowerKeywordRule rule{WideText(item.at("keyword").get<std::string>()), WideText(item.at("onHit").get<std::string>()),
                              WideText(item.at("onMiss").get<std::string>()), WideText(item.at("begin").get<std::string>()),
                              WideText(item.at("end").get<std::string>()), DelayValue(item.at("hitDelayMs")),
                              DelayValue(item.at("missDelayMs"))};
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
}

void PowerPane::LayoutKeywords(int width) {
    HWND list = Control(KeywordStatsId);
    if (!list) return;
    auto d = [this](int value) { return MulDiv(value, dpi_, 96); };
    const int result = std::clamp(width / 4, 66, 120);
    const int counter = std::clamp(width / 9, 42, 64);
    const int written = std::max(56, counter);
    const int first = std::max(50, width - result - counter * 2 - written - 20);
    const int widths[]{first, counter, counter, written, result};
    const wchar_t *headers[]{L"关键字", L"本轮", L"累计", L"已写入", L"本轮结果"};
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
    if (run && snapshot.ready) summary += L" · 上电完成";
    else if (run && snapshot.timedOut) summary += L" · 上电标志超时";
    else if (run && snapshot.readyExpected && run->Active()) summary += L" · 等待上电标志";
    if (!snapshot.reason.empty()) summary += L"\r\n" + snapshot.reason;
    else if (snapshot.state == PowerKeywordRun::State::Draining) summary += L"\r\n等待动作写入后掉电";
    else if (remote_.Active()) summary += L"\r\n关键字测试使用本机 USB 或 RS232 电源";
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
            else if (result.round) state = L"已捕获";
            else if (result.window == Window::Closed) state = L"未捕获";
            else if (result.window == Window::Interrupted) state = L"检测中断";
            else if (result.window == Window::Open) state = L"检测中";
            else state = snapshot.state == PowerKeywordRun::State::Listening ? L"等待开始" : L"未进入检测";
        }
        ListCell(list, i, 0, keywordConfig_.rules[i].keyword);
        ListCell(list, i, 1, std::to_wstring(result.round));
        ListCell(list, i, 2, std::to_wstring(result.total));
        ListCell(list, i, 3, std::to_wstring(result.written));
        ListCell(list, i, 4, state);
    }
}

void PowerPane::ConfigureKeywords() {
    if (const auto run = std::atomic_load(&keywordRun_); run && run->Active()) return;
    ConfigDialog data{{dpi_, dark_, font_, surfaceBrush_, brush_, &fieldStyle_}, keywordConfig_};
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
        const int width = std::clamp(MulDiv(monitor.rcWork.right - monitor.rcWork.left, 96, ui.dpi) - 40, 436, 900);
        PositionDialog(dialog, ui, width, 334);
        AddField(dialog, ui, ReadyId, L"上电完成标志", data->config.ready, 16, 16, width - 32);
        auto list = AddControl(dialog, ui, WC_LISTVIEWW, L"关键字规则", RulesId,
                               WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 16, 80, width - 32, 138);
        ThemeList(list, ui.dark);
        const wchar_t *headers[]{L"关键字", L"捕获动作", L"捕获延迟", L"未捕获动作", L"未捕获延迟", L"检测开始标志", L"检测结束标志"};
        const int widths[]{144, 124, 92, 124, 98, 124, 124};
        for (int i = 0; i < 7; ++i) Column(list, i, headers[i], ui.Scale(widths[i]));
        AddButton(dialog, ui, AddId, L"添加关键字", 16, 230);
        AddButton(dialog, ui, EditId, L"编辑", 124, 230);
        AddButton(dialog, ui, DeleteId, L"删除", 232, 230);
        AddButton(dialog, ui, IDCANCEL, L"取消", width - 220, 282);
        AddButton(dialog, ui, IDOK, L"保存配置", width - 112, 282);
        PopulateRules(dialog, data->config, 0);
        return TRUE;
    }
    if (!data) return FALSE;
    HWND list = GetDlgItem(dialog, RulesId);
    const int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    int action = message == WM_COMMAND ? LOWORD(w) : 0;
    if (message == WM_NOTIFY) {
        const auto &notice = *reinterpret_cast<NMHDR *>(l);
        if (notice.idFrom == RulesId && notice.code == NM_DBLCLK && reinterpret_cast<NMITEMACTIVATE *>(l)->iItem >= 0)
            action = EditId;
        if (notice.idFrom == RulesId && notice.code == LVN_ITEMCHANGED) {
            EnableWindow(GetDlgItem(dialog, EditId), selected >= 0);
            EnableWindow(GetDlgItem(dialog, DeleteId), selected >= 0);
        }
    }
    if (action == AddId || (action == EditId && selected >= 0)) {
        RuleDialog rule{data->ui, action == AddId ? PowerKeywordRule{} : data->config.rules[selected]};
        DialogTemplate resource;
        if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &resource.dialog, dialog, KeywordRuleProc,
                                     reinterpret_cast<LPARAM>(&rule)) == IDOK) {
            if (action == AddId) data->config.rules.push_back(std::move(rule.rule));
            else data->config.rules[selected] = std::move(rule.rule);
            PopulateRules(dialog, data->config, action == AddId ? static_cast<int>(data->config.rules.size()) - 1 : selected);
        }
        return TRUE;
    }
    if (action == DeleteId && selected >= 0) {
        data->config.rules.erase(data->config.rules.begin() + selected);
        PopulateRules(dialog, data->config, std::min(selected, static_cast<int>(data->config.rules.size()) - 1));
        return TRUE;
    }
    if (action == IDOK) {
        data->config.ready = Text(GetDlgItem(dialog, ReadyId));
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
        PositionDialog(dialog, ui, 436, 330);
        AddField(dialog, ui, RuleFieldId, L"关键字", rule.keyword, 16, 12, 404);
        AddField(dialog, ui, RuleFieldId + 1, L"捕获后发送（可留空）", rule.onHit, 16, 76, 272);
        AddField(dialog, ui, RuleFieldId + 2, L"捕获延迟 ms", std::to_wstring(rule.hitDelayMs), 300, 76, 120, true);
        AddField(dialog, ui, RuleFieldId + 3, L"未捕获时发送（可留空）", rule.onMiss, 16, 140, 272);
        AddField(dialog, ui, RuleFieldId + 4, L"未捕获延迟 ms", std::to_wstring(rule.missDelayMs), 300, 140, 120, true);
        AddField(dialog, ui, RuleFieldId + 5, L"检测开始标志（可留空）", rule.begin, 16, 204, 196);
        AddField(dialog, ui, RuleFieldId + 6, L"检测结束标志（可留空）", rule.end, 224, 204, 196);
        AddButton(dialog, ui, IDCANCEL, L"取消", 216, 278);
        AddButton(dialog, ui, IDOK, L"确定", 324, 278);
        return TRUE;
    }
    if (!data) return FALSE;
    const int action = message == WM_COMMAND ? LOWORD(w) : 0;
    if (action == IDOK) {
        try {
            auto field = [&](int index) { return Text(GetDlgItem(dialog, RuleFieldId + index)); };
            auto delay = [&](int index) {
                const auto text = field(index);
                if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::runtime_error("请输入有效的动作延迟毫秒数");
                const auto value = std::stoul(text);
                if (value > MaximumDelay) throw std::runtime_error("动作延迟超出范围");
                return static_cast<unsigned>(value);
            };
            PowerKeywordRule rule{field(0), field(1), field(3), field(5), field(6), delay(2), delay(4)};
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

} // namespace serialctl
