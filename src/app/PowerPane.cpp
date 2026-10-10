#include "PowerPane.h"
#include "UiStyle.h"
#include "Win32Helpers.h"
#include <cmath>
#include <commctrl.h>
#include <locale>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <windowsx.h>
namespace serialctl
{
namespace
{
constexpr int Connect = 10, Disconnect = 11, On = 12, Off = 13, Start = 40, Stop = 41, Save = 42,
              Protection = 43;
std::wstring Wide(const std::string &s)
{
    return MultiByteToWide(reinterpret_cast<const std::uint8_t *>(s.data()), s.size(), CP_UTF8);
}
std::wstring Value(const Json &value, const wchar_t *suffix = L"")
{
    if (!value.is_number())
        return L"—";
    wchar_t text[64]{};
    swprintf_s(text, L"%.3f%s", value.get<double>(), suffix);
    return text;
}
double Parse(const std::wstring &text)
{
    std::wistringstream in(text);
    in.imbue(std::locale::classic());
    double n = 0;
    in >> n;
    if (in.fail())
        throw std::runtime_error("请输入有效数值");
    in >> std::ws;
    if (!std::isfinite(n) || std::abs(n) > 1e9)
        throw std::runtime_error("数值超出范围");
    if (!in.eof())
        throw std::runtime_error("请输入有效数值");
    return n;
}
struct ConnectionDialog
{
    PowerPane *pane;
    HFONT font;
    bool dark;
    PowerService *service;
    HWND kind, resource, port, baud, bits, parity, stop, error, driver;
    Json usb;
};
} // namespace
PowerPane::~PowerPane()
{
    if (titleFont_)
        DeleteObject(titleFont_);
    if (valueFont_)
        DeleteObject(valueFont_);
    if (brush_)
        DeleteObject(brush_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    if (surfaceBrush_)
        DeleteObject(surfaceBrush_);
}
bool PowerPane::Create(HWND parent, HFONT font, bool dark, PowerService *service)
{
    font_ = font;
    service_ = service;
    dark_ = dark;
    HDC dc = GetDC(parent);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(parent, dc);
    LOGFONTW face{};
    GetObjectW(font, sizeof(face), &face);
    face.lfWeight = FW_SEMIBOLD;
    face.lfHeight = MulDiv(-18, dpi_, 96);
    titleFont_ = CreateFontIndirectW(&face);
    face.lfHeight = MulDiv(-20, dpi_, 96);
    valueFont_ = CreateFontIndirectW(&face);
    fieldStyle_.font = font;
    fieldStyle_.dpi = dpi_;
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SerialCtlPowerPane";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"电源管理",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 400, 500, parent,
                              nullptr, wc.hInstance, this);
    return window_ != nullptr;
}
HWND PowerPane::Child(const wchar_t *type, const wchar_t *text, int id, DWORD style)
{
    auto h = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0,
                             window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (_wcsicmp(type, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
        UiStyleButton(h);
    if (_wcsicmp(type, L"EDIT") == 0)
        UiStyleField(h, &fieldStyle_);
    return h;
}
std::wstring PowerPane::Text(HWND h)
{
    int length = GetWindowTextLengthW(h);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(h, text.data(), length + 1);
    text.resize(length);
    return text;
}
void PowerPane::Theme(bool dark)
{
    dark_ = dark;
    if (brush_)
        DeleteObject(brush_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    if (surfaceBrush_)
        DeleteObject(surfaceBrush_);
    auto c = UiTheme(dark);
    brush_ = CreateSolidBrush(c.field);
    fieldBrush_ = CreateSolidBrush(c.raised);
    surfaceBrush_ = CreateSolidBrush(c.surface);
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
Json PowerPane::Selection()
{
    Json selected = Json::array();
    for (int i = 0; i < 3; ++i)
        if (SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            selected.push_back(i + 1);
    return selected;
}
void PowerPane::Submit(Json command)
{
    command["source"] = "local";
    Json result = service_->Submit(command);
    if (result.contains("error"))
        MessageBoxW(window_, Wide(result["error"]).c_str(), L"电源管理", MB_OK | MB_ICONWARNING);
    else
        action_ = result["id"];
    Refresh();
}
void PowerPane::Action(int id)
{
    try
    {
        if (id == Protection)
            ShowProtection();
        else if (id == Connect)
            ShowConnection();
        else if (id == Disconnect)
            Submit({{"type", "disconnect"}});
        else if (id == On || id == Off)
            Submit({{"type", "output"}, {"channels", Selection()}, {"enabled", id == On}});
        else if (id >= 30 && id < 33)
        {
            int i = id - 30;
            Submit({{"type", "parameters"},
                    {"channels", Json::array({i + 1})},
                    {"voltage", Parse(Text(voltage_[i]))},
                    {"current", Parse(Text(current_[i]))}});
        }
        else if (id == Start)
        {
            double count = Parse(Text(count_));
            if (std::floor(count) != count)
                throw std::runtime_error("循环次数必须为整数");
            Submit({{"type", "task"},
                    {"channels", Selection()},
                    {"onMs", static_cast<long long>(Parse(Text(onTime_)) * 1000)},
                    {"offMs", static_cast<long long>(Parse(Text(offTime_)) * 1000)},
                    {"count", static_cast<long long>(count)}});
        }
        else if (id == Stop)
            Submit({{"type", "stop"}});
        else if (id == Save)
            SaveTask();
        Refresh();
    }
    catch (const std::exception &e)
    {
        MessageBoxW(window_, Wide(e.what()).c_str(), L"电源管理", MB_OK | MB_ICONWARNING);
    }
}
std::wstring PowerPane::TaskPath()
{
    wchar_t root[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, root);
    std::wstring dir = std::wstring(root) + L"\\SerialCtl";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\power-task.ini";
}
void PowerPane::LoadTask()
{
    auto path = TaskPath();
    wchar_t text[64]{};
    for (auto item : {std::pair<HWND, const wchar_t *>{onTime_, L"OnSeconds"},
                      {offTime_, L"OffSeconds"},
                      {count_, L"Count"}})
    {
        GetPrivateProfileStringW(L"Task", item.second, item.first == count_ ? L"10" : L"5", text,
                                 64, path.c_str());
        SetWindowTextW(item.first, text);
    }
    int channels = GetPrivateProfileIntW(L"Task", L"Channels", 1, path.c_str());
    for (int i = 0; i < 3; ++i)
        SendMessageW(check_[i], BM_SETCHECK, (channels & (1 << i)) ? BST_CHECKED : BST_UNCHECKED,
                     0);
}
void PowerPane::SaveTask()
{
    auto path = TaskPath();
    int mask = 0;
    for (int i = 0; i < 3; ++i)
        if (SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            mask |= 1 << i;
    bool ok = WritePrivateProfileStringW(L"Task", L"Channels", std::to_wstring(mask).c_str(),
                                         path.c_str()) != FALSE;
    for (auto item : {std::pair<HWND, const wchar_t *>{onTime_, L"OnSeconds"},
                      {offTime_, L"OffSeconds"},
                      {count_, L"Count"}})
        ok = WritePrivateProfileStringW(L"Task", item.second, Text(item.first).c_str(),
                                        path.c_str()) &&
             ok;
    if (!ok)
        MessageBoxW(window_, L"任务保存失败", L"电源管理", MB_OK | MB_ICONERROR);
}
void PowerPane::Refresh()
{
    bool wasConnected = state_.value("connected", false);
    state_ = service_->State();
    if (!wasConnected && state_.value("connected", false))
    {
        for (int i = 0; i < 3; ++i)
        {
            SetWindowTextW(voltage_[i], Value(state_["channels"][i]["setVoltage"]).c_str());
            SetWindowTextW(current_[i], Value(state_["channels"][i]["setCurrent"]).c_str());
        }
    }
    if (wasConnected != state_.value("connected", false))
        PostMessageW(GetParent(window_), PowerStateChanged, 0, 0);
    bool connected = state_.value("connected", false),
         running = state_["task"].value("running", false);
    bool selected = !Selection().empty();
    EnableWindow(buttons_[0], !connected);
    EnableWindow(buttons_[1], connected);
    EnableWindow(buttons_[2], connected && selected);
    EnableWindow(buttons_[3], connected && selected);
    EnableWindow(protectionButton_, connected && selected && !running);
    EnableWindow(start_, connected && selected && !running);
    EnableWindow(stop_, connected && (running || (state_.contains("ownedChannels") &&
                                                  !state_["ownedChannels"].empty())));
    for (auto h : apply_)
        EnableWindow(h, connected && !running);
    if (!action_.empty())
    {
        auto a = service_->Action(action_);
        if (a.value("state", std::string()) == "failed")
        {
            auto error = a.value("error", std::string("操作失败"));
            action_.clear();
            MessageBoxW(window_, Wide(error).c_str(), L"电源管理", MB_OK | MB_ICONWARNING);
        }
        else if (a.value("state", std::string()) == "completed")
            action_.clear();
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void PowerPane::Layout()
{
    RECT r{};
    GetClientRect(window_, &r);
    auto d = [this](int x) { return MulDiv(x, dpi_, 96); };
    const int w = r.right - d(16);
    auto move = [&](HWND h, int x, int y, int width, int height) {
        MoveWindow(h, x, y - scroll_, width, height, TRUE);
    };
    move(buttons_[0], w - d(200), 0, d(96), d(36));
    move(buttons_[1], w - d(96), 0, d(96), d(36));
    move(buttons_[2], w - d(272), d(48), d(96), d(36));
    move(buttons_[3], w - d(168), d(48), d(96), d(36));
    move(protectionButton_, w - d(64), d(48), d(64), d(36));
    cardColumns_ = w >= d(600) ? 3 : 1;
    cardWidth_ = (w - d(12) * (cardColumns_ - 1)) / cardColumns_;
    cardTop_ = d(100);
    cardHeight_ = d(252);
    for (int i = 0; i < 3; ++i)
    {
        int x = (i % cardColumns_) * (cardWidth_ + d(12));
        int y = cardTop_ + (i / cardColumns_) * (cardHeight_ + d(12));
        move(check_[i], x + d(16), y + d(12), d(72), d(28));
        move(voltage_[i], x + d(96), y + d(148), cardWidth_ - d(112), d(28));
        move(current_[i], x + d(96), y + d(184), cardWidth_ - d(112), d(28));
        move(apply_[i], x + d(16), y + d(220), cardWidth_ - d(32), d(28));
    }
    taskTop_ = cardTop_ + ((3 + cardColumns_ - 1) / cardColumns_) * (cardHeight_ + d(12)) + d(12);
    int field = (w - d(16)) / 3;
    for (int i = 0; i < 3; ++i)
    {
        move(i == 0   ? onTime_
             : i == 1 ? offTime_
                      : count_,
             i * (field + d(8)), taskTop_ + d(44), field, d(28));
        move(i == 0   ? start_
             : i == 1 ? stop_
                      : save_,
             i * (field + d(8)), taskTop_ + d(84), field, d(36));
    }
    contentHeight_ = taskTop_ + d(172);
}
void PowerPane::Paint(HDC dc)
{
    RECT r{};
    GetClientRect(window_, &r);
    auto c = UiTheme(dark_);
    FillRect(dc, &r, brush_);
    SetViewportOrgEx(dc, 0, -scroll_, nullptr);
    auto d = [this](int x) { return MulDiv(x, dpi_, 96); };
    const int width = r.right - d(16);
    auto text = [&](const std::wstring &label, RECT rect, COLORREF color,
                    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    HFONT font = nullptr) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        auto old = SelectObject(dc, font ? font : font_);
        DrawTextW(dc, label.c_str(), -1, &rect, flags);
        SelectObject(dc, old);
    };
    text(L"IT6332A", {0, 0, width - d(216), d(36)}, c.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
         titleFont_);
    std::wstring selected = L"已选";
    for (const auto &channel : Selection())
        selected += L" CH" + std::to_wstring(channel.get<int>());
    if (Selection().empty())
        selected = L"未选择通道";
    text(selected, {0, d(48), width - d(288), d(84)}, c.text);
    for (int i = 0; i < 3; ++i)
    {
        int x = (i % cardColumns_) * (cardWidth_ + d(12));
        int y = cardTop_ + (i / cardColumns_) * (cardHeight_ + d(12));
        RECT card{x, y, x + cardWidth_, y + cardHeight_};
        bool checked = SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
        UiBox(dc, card, c.surface, checked ? c.text : c.border, d(12));
        if (!state_.contains("channels"))
            continue;
        auto channel = state_["channels"][i];
        std::wstring status = channel["output"].is_boolean()
                                  ? (channel["output"].get<bool>() ? L"已加电" : L"已掉电")
                                  : L"未连接";
        text(status, {x + d(88), y + d(12), x + cardWidth_ - d(16), y + d(40)}, c.muted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int half = (cardWidth_ - d(32)) / 2;
        text(L"电压 (V)", {x + d(16), y + d(56), x + d(16) + half, y + d(76)}, c.muted);
        text(L"电流 (A)", {x + d(16) + half, y + d(56), x + cardWidth_ - d(16), y + d(76)},
             c.muted);
        text(Value(channel["voltage"]), {x + d(16), y + d(80), x + d(16) + half, y + d(112)},
             c.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE, valueFont_);
        text(Value(channel["current"]),
             {x + d(16) + half, y + d(80), x + cardWidth_ - d(16), y + d(112)}, c.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE, valueFont_);
        text(L"功率 " + Value(channel["power"], L" W"),
             {x + d(16), y + d(116), x + cardWidth_ - d(16), y + d(140)}, c.muted);
        text(L"电压 (V)", {x + d(16), y + d(148), x + d(88), y + d(176)}, c.muted);
        text(L"电流 (A)", {x + d(16), y + d(184), x + d(88), y + d(212)}, c.muted);
    }
    const auto task = state_.value("task", Json::object());
    const bool running = task.value("running", false);
    std::wstring taskTitle = L"循环测试";
    if (running)
        taskTitle += L" · " + std::to_wstring(task.value("completed", 0)) + L" / " +
                     std::to_wstring(task.value("count", 0));
    text(taskTitle, {0, taskTop_, width, taskTop_ + d(24)}, c.text);
    int field = (width - d(16)) / 3;
    for (int i = 0; i < 3; ++i)
        text(i == 0   ? L"加电时长 (秒)"
             : i == 1 ? L"掉电时长 (秒)"
                      : L"循环次数",
             {i * (field + d(8)), taskTop_ + d(24), i * (field + d(8)) + field, taskTop_ + d(44)},
             c.muted);
    std::wstring status = state_.contains("error")           ? Wide(state_["error"])
                          : state_.value("connected", false) ? L"已连接"
                                                             : L"未连接";
    if (state_.contains("logError"))
        status = Wide(state_["logError"]);
    text(status, {0, taskTop_ + d(128), width, taskTop_ + d(168)},
         state_.contains("error") || state_.contains("logError") ? c.danger : c.muted,
         DT_LEFT | DT_WORDBREAK);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    if (contentHeight_ > r.bottom)
    {
        int thumb =
            std::max(d(32), static_cast<int>(r.bottom * r.bottom) / std::max(1, contentHeight_));
        int top =
            scroll_ * (r.bottom - thumb) / std::max(1, contentHeight_ - static_cast<int>(r.bottom));
        RECT bar{r.right - d(8), top, r.right - d(3), top + thumb};
        UiBox(dc, bar, c.border, c.border, d(3));
    }
}
void PowerPane::Scroll(int position)
{
    RECT r{};
    GetClientRect(window_, &r);
    Layout();
    scroll_ = std::clamp(position, 0, std::max(0, contentHeight_ - static_cast<int>(r.bottom)));
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}
LRESULT CALLBACK PowerPane::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *self = reinterpret_cast<PowerPane *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE)
    {
        self = static_cast<PowerPane *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    if (m == WM_CREATE)
    {
        self->Theme(self->dark_);
        const wchar_t *names[] = {L"连接", L"断开", L"加电", L"掉电"};
        for (int i = 0; i < 4; ++i)
            self->buttons_[i] = self->Child(L"BUTTON", names[i], Connect + i, BS_OWNERDRAW);
        for (int i = 0; i < 3; ++i)
        {
            self->check_[i] = self->Child(L"BUTTON", (L"CH" + std::to_wstring(i + 1)).c_str(),
                                          20 + i, BS_AUTOCHECKBOX);
            self->voltage_[i] = self->Child(L"EDIT", L"0", 50 + i, ES_AUTOHSCROLL);
            self->current_[i] = self->Child(L"EDIT", L"0", 60 + i, ES_AUTOHSCROLL);
            self->apply_[i] = self->Child(L"BUTTON", L"应用参数", 30 + i, BS_OWNERDRAW);
        }
        self->protectionButton_ = self->Child(L"BUTTON", L"保护", Protection, BS_OWNERDRAW);
        self->onTime_ = self->Child(L"EDIT", L"5", 70, ES_AUTOHSCROLL);
        self->offTime_ = self->Child(L"EDIT", L"5", 71, ES_AUTOHSCROLL);
        self->count_ = self->Child(L"EDIT", L"10", 72, ES_NUMBER);
        self->start_ = self->Child(L"BUTTON", L"开始测试", Start, BS_OWNERDRAW);
        self->stop_ = self->Child(L"BUTTON", L"停止并掉电", Stop, BS_OWNERDRAW);
        self->save_ = self->Child(L"BUTTON", L"保存任务", Save, BS_OWNERDRAW);
        self->LoadTask();
        self->Refresh();
        SetTimer(h, 1, 500, nullptr);
        return 0;
    }
    if (m == WM_MOUSEWHEEL)
    {
        self->Scroll(self->scroll_ -
                     GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * MulDiv(48, self->dpi_, 96));
        return 0;
    }
    if (m == WM_LBUTTONDOWN)
    {
        RECT r{};
        GetClientRect(h, &r);
        if (GET_X_LPARAM(l) >= r.right - MulDiv(16, self->dpi_, 96) &&
            self->contentHeight_ > r.bottom)
        {
            self->scrolling_ = true;
            SetCapture(h);
            self->Scroll(GET_Y_LPARAM(l) *
                         std::max(0, self->contentHeight_ - static_cast<int>(r.bottom)) /
                         std::max(1, static_cast<int>(r.bottom)));
            return 0;
        }
    }
    if (m == WM_MOUSEMOVE && self->scrolling_)
    {
        RECT r{};
        GetClientRect(h, &r);
        self->Scroll(GET_Y_LPARAM(l) *
                     std::max(0, self->contentHeight_ - static_cast<int>(r.bottom)) /
                     std::max(1, static_cast<int>(r.bottom)));
        return 0;
    }
    if (m == WM_LBUTTONUP || m == WM_CAPTURECHANGED)
    {
        self->scrolling_ = false;
        if (GetCapture() == h)
            ReleaseCapture();
    }
    if (m == WM_SIZE)
    {
        self->Scroll(self->scroll_);
        return 0;
    }
    if (m == WM_TIMER)
    {
        HWND focus = GetFocus();
        if (focus && IsChild(h, focus) && IsWindowVisible(h))
        {
            RECT field{}, view{};
            GetWindowRect(focus, &field);
            MapWindowPoints(HWND_DESKTOP, h, reinterpret_cast<POINT *>(&field), 2);
            GetClientRect(h, &view);
            if (field.top < 0)
                self->Scroll(self->scroll_ + field.top);
            else if (field.bottom > view.bottom)
                self->Scroll(self->scroll_ + field.bottom - view.bottom);
        }
        self->Refresh();
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(w) == BN_CLICKED)
    {
        self->Action(LOWORD(w));
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT)
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rect{};
        GetClientRect(h, &rect);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
        auto old = SelectObject(buffer, bitmap);
        self->Paint(buffer);
        BitBlt(dc, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN || m == WM_CTLCOLOREDIT)
    {
        auto c = UiTheme(self->dark_);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
        bool channel = id >= 20 && id <= 22;
        SetBkColor(reinterpret_cast<HDC>(w), m == WM_CTLCOLOREDIT ? c.field
                                             : channel            ? c.surface
                                                                  : c.raised);
        return reinterpret_cast<LRESULT>(m == WM_CTLCOLOREDIT ? self->brush_
                                         : channel            ? self->surfaceBrush_
                                                              : self->fieldBrush_);
    }
    if (m == WM_DRAWITEM)
    {
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        auto c = UiTheme(self->dark_);
        bool primary = item.CtlID == On || item.CtlID == Connect || item.CtlID == Start;
        HBRUSH outside =
            CreateSolidBrush(item.CtlID >= 30 && item.CtlID <= 32 ? c.surface : c.field);
        FillRect(item.hDC, &item.rcItem, outside);
        DeleteObject(outside);
        UiBox(item.hDC, item.rcItem,
              (item.itemState & ODS_SELECTED)               ? c.border
              : primary && !(item.itemState & ODS_DISABLED) ? c.accent
              : GetPropW(item.hwndItem, L"SerialCtl.PowerHover") && !(item.itemState & ODS_DISABLED)
                  ? c.surface
                  : c.raised,
              (item.itemState & ODS_FOCUS) ? c.accent : c.border, MulDiv(10, self->dpi_, 96));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, (item.itemState & ODS_DISABLED)           ? c.muted
                               : item.CtlID == Off || item.CtlID == Stop ? c.danger
                               : primary                                 ? RGB(255, 255, 255)
                                                                         : c.text);
        auto old = SelectObject(item.hDC, self->font_);
        RECT rect = item.rcItem;
        auto value = self->Text(item.hwndItem);
        DrawTextW(item.hDC, value.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old);
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
void PowerPane::ShowConnection()
{
    ConnectionDialog data{this, font_, dark_, service_};
    struct Template
    {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    };
    Template t{};
    t.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dialog.cx = 238;
    t.dialog.cy = 240;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &t.dialog, GetAncestor(window_, GA_ROOT),
                            ConnectProc, reinterpret_cast<LPARAM>(&data));
}
INT_PTR CALLBACK PowerPane::ConnectProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto *data = reinterpret_cast<ConnectionDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    if (m == WM_INITDIALOG)
    {
        data = reinterpret_cast<ConnectionDialog *>(l);
        SetWindowLongPtrW(h, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(h, L"连接电源");
        int dpi = data->pane->dpi_;
        auto d = [dpi](int v) { return MulDiv(v, dpi, 96); };
        UiResizeDialog(h, dpi, 436, 400);
        auto add = [&](const wchar_t *cls, const wchar_t *text, int id, int x, int y, int width,
                       DWORD style = 0, int height = 28) {
            auto c =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, d(x), d(y), d(width),
                                d(height), h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), TRUE);
            if (_wcsicmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
                UiStyleButton(c);
            if (_wcsicmp(cls, L"EDIT") == 0 || _wcsicmp(cls, L"COMBOBOX") == 0)
                UiStyleField(c, &data->pane->fieldStyle_);
            return c;
        };
        add(L"STATIC", L"连接方式", -1, 16, 12, 200);
        data->kind = add(L"COMBOBOX", L"", 100, 16, 40, 404, CBS_DROPDOWNLIST | WS_TABSTOP, 200);
        for (auto text : {L"USB", L"RS232"})
            SendMessageW(data->kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(data->kind, CB_SETCURSEL, 0, 0);
        add(L"STATIC", L"设备 / COM 口", -1, 16, 80, 200);
        data->resource =
            add(L"COMBOBOX", L"", 101, 16, 108, 300, CBS_DROPDOWNLIST | WS_TABSTOP, 200);
        data->port = add(L"COMBOBOX", L"", 102, 16, 108, 300, WS_TABSTOP | CBS_DROPDOWNLIST, 200);
        add(L"BUTTON", L"刷新", 103, 328, 108, 88, WS_TABSTOP | BS_OWNERDRAW);
        add(L"STATIC", L"波特率", 110, 16, 156, 92);
        add(L"STATIC", L"数据位", 111, 120, 156, 92);
        add(L"STATIC", L"校验", 112, 224, 156, 92);
        add(L"STATIC", L"停止位", 113, 328, 156, 92);
        data->baud = add(L"EDIT", L"9600", 104, 16, 184, 92, ES_NUMBER | WS_TABSTOP);
        data->bits = add(L"EDIT", L"8", 105, 120, 184, 92, ES_NUMBER | WS_TABSTOP);
        data->parity = add(L"COMBOBOX", L"", 106, 224, 184, 92, CBS_DROPDOWNLIST | WS_TABSTOP, 120);
        for (auto text : {L"无", L"奇", L"偶"})
            SendMessageW(data->parity, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(data->parity, CB_SETCURSEL, 0, 0);
        data->stop = add(L"COMBOBOX", L"", 107, 328, 184, 92, CBS_DROPDOWNLIST | WS_TABSTOP, 100);
        for (auto text : {L"1", L"2"})
            SendMessageW(data->stop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(data->stop, CB_SETCURSEL, 0, 0);
        data->error = add(L"STATIC", L"", 108, 16, 236, 404, 0, 64);
        data->driver =
            add(L"BUTTON", L"安装 USB 驱动", 109, 16, 308, 144, WS_TABSTOP | BS_OWNERDRAW);
        add(L"BUTTON", L"取消", IDCANCEL, 280, 364, 96, WS_TABSTOP | BS_OWNERDRAW, 36);
        add(L"BUTTON", L"连接", IDOK, 392, 364, 96, WS_TABSTOP | BS_OWNERDRAW, 36);
        RECT owner{}, rect{};
        GetWindowRect(GetParent(h), &owner);
        GetWindowRect(h, &rect);
        SetWindowPos(h, nullptr,
                     owner.left + (owner.right - owner.left - (rect.right - rect.left)) / 2,
                     owner.top + (owner.bottom - owner.top - (rect.bottom - rect.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        SendMessageW(h, WM_COMMAND, MAKEWPARAM(100, CBN_SELCHANGE), 0);
        return TRUE;
    }
    if (!data)
        return FALSE;
    if (m == WM_DRAWITEM)
        return Proc(data->pane->window_, m, w, l);
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT ||
        m == WM_CTLCOLORLISTBOX || m == WM_CTLCOLORBTN)
    {
        auto c = UiTheme(data->dark);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        SetBkColor(reinterpret_cast<HDC>(w), c.field);
        return reinterpret_cast<INT_PTR>(data->pane->brush_);
    }
    if (m == WM_COMMAND)
    {
        int id = LOWORD(w);
        int kind = static_cast<int>(SendMessageW(data->kind, CB_GETCURSEL, 0, 0));
        if (id == 100 || id == 103)
        {
            auto d = [data](int x) { return MulDiv(x, data->pane->dpi_, 96); };
            const bool driverNeeded = kind == 0 && !PowerService::VisaAvailable();
            int footer = kind == 1 ? 268 : driverNeeded ? 264 : 200;
            MoveWindow(GetDlgItem(h, IDCANCEL), d(216), d(footer), d(96), d(36), TRUE);
            MoveWindow(GetDlgItem(h, IDOK), d(324), d(footer), d(96), d(36), TRUE);
            MoveWindow(data->error, d(16), d(kind == 1 ? 224 : 156), d(404), d(32), TRUE);
            MoveWindow(data->driver, d(16), d(kind == 1 ? 308 : 220), d(144), d(28), TRUE);
            UiResizeDialog(h, data->pane->dpi_, 436, footer + 52);
            for (int i = 104; i <= 107; ++i)
                ShowWindow(GetDlgItem(h, i), kind == 1 ? SW_SHOW : SW_HIDE);
            for (int i = 110; i <= 113; ++i)
                ShowWindow(GetDlgItem(h, i), kind == 1 ? SW_SHOW : SW_HIDE);
            ShowWindow(data->resource, kind == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(data->port, kind == 1 ? SW_SHOW : SW_HIDE);
            ShowWindow(GetDlgItem(h, 103), SW_SHOW);
            ShowWindow(data->driver, driverNeeded ? SW_SHOW : SW_HIDE);
            if (kind == 0)
            {
                SendMessageW(data->resource, CB_RESETCONTENT, 0, 0);
                data->usb = PowerService::UsbResources();
                for (const auto &s : data->usb)
                {
                    auto text = Wide(s);
                    SendMessageW(data->resource, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(text.c_str()));
                }
                SendMessageW(data->resource, CB_SETCURSEL, 0, 0);
                SetWindowTextW(data->error, !PowerService::VisaAvailable()
                                                ? L"未安装 VISA，请安装发布包中的 USB 驱动。"
                                            : data->usb.empty()
                                                ? L"未发现 USB 电源，请检查连接后刷新。"
                                                : L"");
            }
            else
            {
                if (kind == 1)
                {
                    auto previous = data->pane->Text(data->port);
                    SendMessageW(data->port, CB_RESETCONTENT, 0, 0);
                    wchar_t target[1024]{};
                    for (int index = 1; index <= 256; ++index)
                    {
                        std::wstring name = L"COM" + std::to_wstring(index);
                        if (QueryDosDeviceW(name.c_str(), target, 1024))
                            SendMessageW(data->port, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(name.c_str()));
                    }
                    if (previous.empty())
                        SendMessageW(data->port, CB_SETCURSEL, 0, 0);
                    else
                    {
                        auto index = SendMessageW(data->port, CB_FINDSTRINGEXACT, -1,
                                                  reinterpret_cast<LPARAM>(previous.c_str()));
                        SendMessageW(data->port, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
                    }
                }
                SetWindowTextW(data->error, L"");
            }
            return TRUE;
        }
        if (id == 109)
        {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path(exe);
            path = path.substr(0, path.find_last_of(L"\\")) + L"\\drivers\\ni-visa18\\setup.exe";
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                SetWindowTextW(data->error, L"发布包中的驱动文件缺失，请重新解压完整压缩包。");
            else
            {
                auto result = reinterpret_cast<INT_PTR>(
                    ShellExecuteW(h, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32)
                    SetWindowTextW(data->error,
                                   L"驱动安装程序未能启动，请从发布包目录手动运行 setup.exe。");
            }
            return TRUE;
        }
        if (id == IDCANCEL)
        {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        if (id == IDOK)
        {
            try
            {
                if (kind != 0 && kind != 1)
                    throw std::runtime_error("请选择 USB 或 RS232");
                Json command = {{"type", "connect"}, {"backend", kind == 0 ? "usb" : "serial"}};
                if (kind == 0)
                {
                    auto index = SendMessageW(data->resource, CB_GETCURSEL, 0, 0);
                    if (index < 0 || static_cast<size_t>(index) >= data->usb.size())
                        throw std::runtime_error("请选择 USB 设备");
                    command["resource"] = data->usb[static_cast<size_t>(index)];
                }
                if (kind == 1)
                {
                    command["port"] = WideToMultiByte(data->pane->Text(data->port), CP_UTF8);
                    command["baud"] = std::stoi(data->pane->Text(data->baud));
                    command["dataBits"] = std::stoi(data->pane->Text(data->bits));
                    command["parity"] = SendMessageW(data->parity, CB_GETCURSEL, 0, 0);
                    command["stopBits"] = SendMessageW(data->stop, CB_GETCURSEL, 0, 0) == 1 ? 2 : 0;
                }
                data->pane->Submit(command);
                EndDialog(h, IDOK);
            }
            catch (const std::exception &e)
            {
                SetWindowTextW(data->error, Wide(e.what()).c_str());
            }
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace serialctl

namespace serialctl
{
void PowerPane::ShowProtection()
{
    struct Template
    {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    } t{};
    t.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dialog.cx = 238;
    t.dialog.cy = 156;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &t.dialog, GetAncestor(window_, GA_ROOT),
                            ProtectionProc, reinterpret_cast<LPARAM>(this));
}
INT_PTR CALLBACK PowerPane::ProtectionProc(HWND dialog, UINT message, WPARAM w, LPARAM l)
{
    auto *self = reinterpret_cast<PowerPane *>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        self = reinterpret_cast<PowerPane *>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        SetWindowTextW(dialog, L"通道异常保护");
        auto d = [self](int x) { return MulDiv(x, self->dpi_, 96); };
        UiResizeDialog(dialog, self->dpi_, 424, 252);
        auto add = [&](const wchar_t *cls, const wchar_t *text, int id, int x, int y, int width,
                       int height, DWORD style = 0) {
            HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, d(x), d(y),
                                     d(width), d(height), dialog,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->font_), TRUE);
            if (_wcsicmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
                UiStyleButton(h);
            if (_wcsicmp(cls, L"EDIT") == 0)
                UiStyleField(h, &self->fieldStyle_);
            return h;
        };
        double maxV = 30, maxA = 6;
        for (const auto &c : self->Selection())
        {
            if (c == 3)
            {
                maxV = 5;
                maxA = 3;
            }
        }
        auto selected = self->Selection();
        if (!selected.empty())
        {
            auto c = self->state_["channels"][selected[0].get<int>() - 1];
            if (c.contains("protection"))
            {
                maxV = c["protection"]["voltageLimit"];
                maxA = c["protection"]["currentLimit"];
            }
        }
        add(L"BUTTON", L"启用保护", 90, 16, 16, 168, 28, BS_AUTOCHECKBOX | WS_TABSTOP);
        SendDlgItemMessageW(dialog, 90, BM_SETCHECK, BST_CHECKED, 0);
        add(L"STATIC", L"电压上限 (V)", -1, 16, 60, 184, 28);
        add(L"STATIC", L"电流上限 (A)", -1, 224, 60, 184, 28);
        add(L"EDIT", Value(maxV).c_str(), 91, 16, 92, 184, 28, ES_AUTOHSCROLL | WS_TABSTOP);
        add(L"EDIT", Value(maxA).c_str(), 92, 224, 92, 184, 28, ES_AUTOHSCROLL | WS_TABSTOP);
        add(L"STATIC", L"", 93, 16, 132, 392, 52);
        add(L"BUTTON", L"取消", IDCANCEL, 208, 200, 96, 36, WS_TABSTOP | BS_OWNERDRAW);
        add(L"BUTTON", L"保存", IDOK, 312, 200, 96, 36, BS_OWNERDRAW | WS_TABSTOP);
        RECT owner{}, rect{};
        GetWindowRect(GetParent(dialog), &owner);
        GetWindowRect(dialog, &rect);
        SetWindowPos(dialog, nullptr,
                     owner.left + (owner.right - owner.left - (rect.right - rect.left)) / 2,
                     owner.top + (owner.bottom - owner.top - (rect.bottom - rect.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    if (!self)
        return FALSE;
    if (message == WM_DRAWITEM)
        return Proc(self->window_, message, w, l);
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
        message == WM_CTLCOLORBTN)
    {
        auto colors = UiTheme(self->dark_);
        SetTextColor(reinterpret_cast<HDC>(w), colors.text);
        SetBkColor(reinterpret_cast<HDC>(w), colors.field);
        return reinterpret_cast<INT_PTR>(self->brush_);
    }
    if (message == WM_COMMAND)
    {
        if (LOWORD(w) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(w) == IDOK)
        {
            try
            {
                Json command = {
                    {"type", "protection"},
                    {"channels", self->Selection()},
                    {"voltageLimit", Parse(self->Text(GetDlgItem(dialog, 91)))},
                    {"currentLimit", Parse(self->Text(GetDlgItem(dialog, 92)))},
                    {"enabled", SendDlgItemMessageW(dialog, 90, BM_GETCHECK, 0, 0) == BST_CHECKED}};
                command["source"] = "local";
                Json result = self->service_->Submit(command);
                if (result.contains("error"))
                    throw std::runtime_error(result["error"].get<std::string>());
                self->action_ = result["id"];
                EndDialog(dialog, IDOK);
            }
            catch (const std::exception &e)
            {
                SetDlgItemTextW(dialog, 93, Wide(e.what()).c_str());
            }
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace serialctl
