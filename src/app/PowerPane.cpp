#include "PowerPane.h"
#include "UiStyle.h"
#include "Win32Helpers.h"
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <windowsx.h>
namespace serialctl {
namespace {
constexpr int Connect = 10, Disconnect = 11, On = 12, Off = 13, Start = 40, Stop = 41, Save = 42, Protection = 43;
std::wstring Wide(const std::string &s) {
    return MultiByteToWide(reinterpret_cast<const std::uint8_t *>(s.data()), s.size(), CP_UTF8);
}
std::wstring Value(const Json &value, const wchar_t *suffix = L"") {
    if (!value.is_number())
        return L"—";
    wchar_t text[64]{};
    swprintf_s(text, L"%.3f%s", value.get<double>(), suffix);
    return text;
}
double Parse(const std::wstring &text) {
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
struct ResourceScan {
    std::atomic<bool> done{false};
    int kind = 0;
    Json usb = Json::array();
    std::vector<std::wstring> ports;
};
struct ConnectionDialog {
    PowerPane *pane;
    HFONT font;
    bool dark;
    PowerService *service;
    HWND kind, resource, port, baud, bits, parity, stop, error, driver;
    Json usb;
    std::shared_ptr<ResourceScan> scan;
};
} // namespace
PowerPane::~PowerPane() {
    if (titleFont_)
        DeleteObject(titleFont_);
    if (valueFont_)
        DeleteObject(valueFont_);
    if (smallFont_)
        DeleteObject(smallFont_);
    if (brush_)
        DeleteObject(brush_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    if (surfaceBrush_)
        DeleteObject(surfaceBrush_);
    if (panelBrush_) DeleteObject(panelBrush_);
}
void PowerPane::BeginShutdown() {
    closing_ = true;
    if (window_) KillTimer(window_, 1);
}
void PowerPane::WaitForBackground() {
    for (auto &worker : resourceScanWorkers_) worker.Drain();
}
bool PowerPane::Create(HWND parent, HFONT font, bool dark, PowerService *service) {
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
    face.lfHeight = MulDiv(-13, dpi_, 96);
    face.lfWeight = FW_NORMAL;
    smallFont_ = CreateFontIndirectW(&face);
    fieldStyle_.font = font;
    fieldStyle_.dpi = dpi_;
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SerialCtlPowerPane";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"电源管理",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 400, 500, parent, nullptr,
                              wc.hInstance, this);
    return window_ != nullptr;
}
HWND PowerPane::Child(const wchar_t *type, const wchar_t *text, int id, DWORD style) {
    auto h = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, window_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    controls_[id] = h;
    if ((id >= 50 && id <= 52) || (id >= 60 && id <= 62))
        SetPropW(h, L"SerialCtl.FieldSurface", reinterpret_cast<HANDLE>(1));
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (id >= 90)
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), FALSE);
    if (_wcsicmp(type, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
        UiStyleButton(h);
    if (_wcsicmp(type, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX)
        UiStyleCheckBox(h, &fieldStyle_, id >= 20 && id <= 22);
    if (_wcsicmp(type, L"EDIT") == 0)
        UiStyleField(h, &fieldStyle_);
    return h;
}
std::wstring PowerPane::Text(HWND h) {
    int length = GetWindowTextLengthW(h);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(h, text.data(), length + 1);
    text.resize(length);
    return text;
}
void PowerPane::Theme(bool dark) {
    dark_ = dark;
    if (brush_)
        DeleteObject(brush_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
    if (surfaceBrush_)
        DeleteObject(surfaceBrush_);
    if (panelBrush_) DeleteObject(panelBrush_);
    auto c = UiTheme(dark);
    brush_ = CreateSolidBrush(c.field);
    fieldBrush_ = CreateSolidBrush(c.raised);
    surfaceBrush_ = CreateSolidBrush(c.surface);
    panelBrush_ = CreateSolidBrush(UiAiTheme(dark).surface);
    for (const auto &entry : controls_) {
        const bool card = GetParent(entry.second) == body_ && (tab_ != 0 || (entry.first >= 20 && entry.first <= 62));
        const COLORREF backdrop = card ? c.surface : UiAiTheme(dark).surface;
        SetPropW(entry.second, L"SerialCtl.Backdrop", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(backdrop) + 1));
    }
    if (window_)
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
Json PowerPane::Selection() {
    Json selected = Json::array();
    for (int i = 0; i < 3; ++i)
        if (SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            selected.push_back(i + 1);
    if (draftMode_ != 0 && !selected.empty()) {
        bool related = std::find(selected.begin(), selected.end(), Json(1)) != selected.end() ||
                       std::find(selected.begin(), selected.end(), Json(2)) != selected.end();
        if (related) {
            selected = Json::array({1, 2});
            if (SendMessageW(check_[2], BM_GETCHECK, 0, 0) == BST_CHECKED)
                selected.push_back(3);
        }
    }
    return selected;
}
void PowerPane::Submit(Json command) {
    command["source"] = "local";
    Json result = service_->Submit(command);
    if (result.contains("error"))
        MessageBoxW(window_, Wide(result["error"]).c_str(), L"电源管理", MB_OK | MB_ICONWARNING);
    else {
        action_ = result["id"];
        pendingActions_.push_back(action_);
    }
    Refresh();
}
void PowerPane::SetRecoveryTarget(const std::string &id, const std::wstring &label, bool eligible) {
    if (id != recoveryTarget_ || !eligible) {
        if (recoveryBinding_.is_object()) {
            const auto task = service_->State().value("task", Json::object());
            if (task.value("running", false) && task.value("type", std::string()) == "recover")
                service_->Submit({{"type", "stop"}, {"source", "recovery-target-changed"}});
        }
        recoveryBinding_ = nullptr;
        SendMessageW(Control(248), BM_SETCHECK, BST_UNCHECKED, 0);
    }
    recoveryTarget_ = id;
    recoveryLabel_ = label;
    recoveryEligible_ = eligible;
    if (Control(248)) EnableWindow(Control(248), eligible && service_->State().value("connected", false));
}
Json PowerPane::RecoveryBinding() {
    const auto current = service_->State();
    if (!recoveryBinding_.is_object() || !recoveryEligible_ || !current.value("connected", false) ||
        SendMessageW(Control(248), BM_GETCHECK, 0, 0) != BST_CHECKED ||
        recoveryBinding_.value("sessionId", std::string()) != recoveryTarget_ ||
        recoveryBinding_.value("identity", std::string()) != current.value("identity", std::string()) ||
        recoveryBinding_.value("mode", std::string()) != current.value("mode", std::string()) ||
        recoveryBinding_.value("channels", Json::array()) != Selection())
        return {{"enabled", false}, {"sessionId", recoveryTarget_}};
    Json result = recoveryBinding_;
    result["enabled"] = true;
    result["minimumOffMs"] = 10000;
    return result;
}
std::wstring PowerPane::TaskPath() {
    wchar_t root[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, root);
    std::wstring dir = std::wstring(root) + L"\\SerialCtl";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\power-task.ini";
}
void PowerPane::LoadTask() {
    auto path = TaskPath();
    wchar_t text[64]{};
    for (auto item :
         {std::pair<HWND, const wchar_t *>{onTime_, L"OnSeconds"}, {offTime_, L"OffSeconds"}, {count_, L"Count"}}) {
        GetPrivateProfileStringW(L"Task", item.second, item.first == count_ ? L"10" : L"5", text, 64, path.c_str());
        SetWindowTextW(item.first, text);
    }
    int channels = GetPrivateProfileIntW(L"Task", L"Channels", 1, path.c_str());
    for (int i = 0; i < 3; ++i)
        SendMessageW(check_[i], BM_SETCHECK, (channels & (1 << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
}
void PowerPane::SaveTask() {
    auto path = TaskPath();
    int mask = 0;
    for (int i = 0; i < 3; ++i)
        if (SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            mask |= 1 << i;
    bool ok = WritePrivateProfileStringW(L"Task", L"Channels", std::to_wstring(mask).c_str(), path.c_str()) != FALSE;
    for (auto item :
         {std::pair<HWND, const wchar_t *>{onTime_, L"OnSeconds"}, {offTime_, L"OffSeconds"}, {count_, L"Count"}})
        ok = WritePrivateProfileStringW(L"Task", item.second, Text(item.first).c_str(), path.c_str()) && ok;
    if (!ok)
        MessageBoxW(window_, L"任务保存失败", L"电源管理", MB_OK | MB_ICONERROR);
}
void PowerPane::Action(int id) {
    refreshRequired_=true;
    try {
        if (id == 248) {
            recoveryBinding_ = nullptr;
            if (SendMessageW(Control(248), BM_GETCHECK, 0, 0) == BST_CHECKED && recoveryEligible_ &&
                state_.value("connected", false) && !Selection().empty())
                recoveryBinding_ = {{"sessionId", recoveryTarget_}, {"label", WideToMultiByte(recoveryLabel_, CP_UTF8)},
                    {"channels", Selection()}, {"identity", state_.value("identity", std::string())},
                    {"mode", state_.value("mode", std::string())}};
            else {
                SendMessageW(Control(248), BM_SETCHECK, BST_UNCHECKED, 0);
                const auto task = state_.value("task", Json::object());
                if (task.value("running", false) && task.value("type", std::string()) == "recover")
                    service_->Submit({{"type", "stop"}, {"source", "recovery-disabled"}});
            }
            Refresh();
            return;
        }
        if (id == 80) {
            SelectMode();
            return;
        }
        if (id >= 90 && id <= 93) {
            if(tab_ == id-90)return;
            const int old=tab_+90;
            tab_ = id - 90;
            scroll_ = 0;
            Layout();
            InvalidateRect(Control(old),nullptr,FALSE);
            InvalidateRect(Control(id),nullptr,FALSE);
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (id >= 110 && id <= 114) {
            if(automation_ == id-110)return;
            const int old=automation_+110;
            automation_ = id - 110;
            Layout();
            InvalidateRect(Control(old),nullptr,FALSE);
            InvalidateRect(Control(id),nullptr,FALSE);
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (id >= 190 && id <= 193) {
            if(settings_ == id-190)return;
            const int old=settings_+190;
            settings_ = id - 190;
            Layout();
            InvalidateRect(Control(old),nullptr,FALSE);
            InvalidateRect(Control(id),nullptr,FALSE);
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (id >= 20 && id <= 22) {
            if (draftMode_ != 0 && id != 22)
                SendMessageW(check_[id == 20 ? 1 : 0], BM_SETCHECK, SendMessageW(check_[id - 20], BM_GETCHECK, 0, 0),
                             0);
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (id == Connect)
            ShowConnection();
        else if (id == Disconnect)
            Submit({{"type", "disconnect"},{"preserveOutputs",true}});
        else if (id == Protection) {
            tab_ = 2;
            settings_ = 0;
            Layout();
        } else if (id == On || id == Off)
            Submit({{"type", "output"}, {"channels", Selection()}, {"enabled", id == On}});
        else if (id == 81) {
            Json settings = Json::array();
            auto selection = Selection();
            bool group = draftMode_ != 0;
            for (auto ch : selection) {
                int i = ch.get<int>() - 1;
                if (group && i == 1)
                    continue;
                settings.push_back({{"channels", group && i == 0 ? Json::array({1, 2}) : Json::array({i + 1})},
                                    {"voltage", Parse(Text(voltage_[i]))},
                                    {"current", Parse(Text(current_[i]))}});
            }
            Submit({{"type", "batch"}, {"mode", Mode()}, {"settings", settings}});
        } else if (id == Start) {
            auto millis = [&](HWND h) {
                double seconds = Parse(Text(h));
                if (seconds < 0 || seconds > 604800)
                    throw std::runtime_error("时间超出范围");
                return static_cast<long long>(std::llround(seconds * 1000));
            };
            if (automation_ == 0) {
                double count = Parse(Text(count_));
                if (std::floor(count) != count || count < 1 || count > 1000000)
                    throw std::runtime_error("循环次数必须为 1–1000000 的整数");
                Submit({{"type", "task"},
                        {"channels", Selection()},
                        {"onMs", millis(onTime_)},
                        {"offMs", millis(offTime_)},
                        {"count", static_cast<long long>(count)}});
            }
            if (automation_ == 1) {
                auto date = [&](HWND h) {
                    std::tm tm{};
                    std::wistringstream input(Text(h));
                    input >> std::get_time(&tm, L"%Y-%m-%d %H:%M:%S");
                    if (input.fail())
                        throw std::runtime_error("时间格式为 YYYY-MM-DD HH:MM:SS");
                    input >> std::ws;
                    if (!input.eof())
                        throw std::runtime_error("时间包含多余内容");
                    tm.tm_isdst = -1;
                    auto requested = tm;
                    auto value = std::mktime(&tm);
                    if (value == -1 || tm.tm_year != requested.tm_year || tm.tm_mon != requested.tm_mon ||
                        tm.tm_mday != requested.tm_mday || tm.tm_hour != requested.tm_hour ||
                        tm.tm_min != requested.tm_min || tm.tm_sec != requested.tm_sec)
                        throw std::runtime_error("无效日期");
                    return value;
                };
                auto begin = date(Control(120)), end = date(Control(121)), now = std::time(nullptr);
                if (begin < now || end <= begin)
                    throw std::runtime_error("开始时间须在将来，掉电时间须晚于开始时间");
                Submit({{"type", "schedule"},
                        {"channels", Selection()},
                        {"startMs", static_cast<long long>(begin - now) * 1000},
                        {"durationMs", static_cast<long long>(end - begin) * 1000}});
            }
            if (automation_ == 2)
                Submit({{"type", "sequence"}, {"channels", Selection()}, {"steps", steps_}});
            if (automation_ == 3)
                Submit({{"type", "deviceTimer"},
                        {"enabled", SendMessageW(Control(124), CB_GETCURSEL, 0, 0) == 1},
                        {"seconds", Parse(Text(Control(125)))}});
            if (automation_ == 4) {
                auto selected = Selection();
                if (selected.empty())
                    throw std::runtime_error("请选择通道");
                int i = selected[0].get<int>() - 1;
                Submit({{"type", "trigger"},
                        {"channels", selected},
                        {"delay", Parse(Text(Control(126)))},
                        {"voltageStep", Parse(Text(Control(127)))},
                        {"currentStep", Parse(Text(Control(128)))},
                        {"voltage", Parse(Text(voltage_[i]))},
                        {"current", Parse(Text(current_[i]))},
                        {"fire", true}});
            }
        } else if (id == Stop) {
            if (automation_ == 3) {
                Submit({{"type", "deviceTimer"}, {"enabled", false}, {"seconds", Parse(Text(Control(125)))}});
                Submit({{"type", "output"}, {"channels", Selection()}, {"enabled", false}});
            } else
                Submit({{"type", "stop"}});
        } else if (id == Save)
            TaskFile(true);
        else if (id == 44)
            TaskFile(false);
        else if (id == 100) {
            paused_ = !paused_;
            SetWindowTextW(Control(100), paused_ ? L"继续显示" : L"暂停显示");
        } else if (id == 101 || id == 240)
            Export(true);
        else if (id == 241)
            Export(false);
        else if (id == 103)
            Submit({{"type", "interval"},
                    {"milliseconds", SendMessageW(Control(103), CB_GETCURSEL, 0, 0) == 1 ? 2000 : 1000}});
        else if (id == 123)
            EditSequence();
        else if (id == 133)
            Submit({{"type", "settings"}, {"operation", "readTimer"}});
        else if (id >= 134 && id <= 137)
            Submit({{"type", "step"},
                    {"channels", Selection()},
                    {"quantity", id < 136 ? "voltage" : "current"},
                    {"direction", id % 2 == 0 ? 1 : -1}});
        else if (id >= 200)
            DeviceAction(id);
        Refresh();
    } catch (const std::exception &e) {
        MessageBoxW(GetAncestor(window_, GA_ROOT), Wide(e.what()).c_str(), L"电源管理", MB_OK | MB_ICONWARNING);
    }
}
void PowerPane::DeviceAction(int id) {
    int ch = std::clamp(static_cast<int>(SendMessageW(Control(200), CB_GETCURSEL, 0, 0)), 0, 2) + 1;
    auto confirm = [&](const wchar_t *text) {
        return MessageBoxW(GetAncestor(window_, GA_ROOT), text, L"电源管理", MB_OKCANCEL | MB_ICONWARNING) == IDOK;
    };
    auto op = [&](const char *name) { Submit({{"type", "settings"}, {"operation", name}}); };
    if (id == 200) {
        populated_ = false;
        PopulateSettings();
    }
    if (id == 206)
        Submit({{"type", "settings"}, {"operation", "readProtection"}, {"channels", Json::array({ch})}});
    if (id == 207 && confirm(L"关闭相关通道输出并清除保护状态？"))
        Submit({{"type", "settings"}, {"operation", "clearProtection"}, {"channels", Json::array({ch})}});
    if (id == 208)
        Submit({{"type", "settings"},
                {"operation", "protection"},
                {"channels", Json::array({ch})},
                {"enabled", SendMessageW(Control(201), BM_GETCHECK, 0, 0) == BST_CHECKED},
                {"limit", Parse(Text(Control(202)))},
                {"ovp", Parse(Text(Control(203)))},
                {"softwareVoltage", Parse(Text(Control(204)))},
                {"softwareCurrent", Parse(Text(Control(205)))}});
    if (id == 214) {
        Submit({{"type", "settings"}, {"operation", "readMode"}});
    }
    if (id == 215) {
        SendMessageW(Control(80), CB_SETCURSEL, SendMessageW(Control(210), CB_GETCURSEL, 0, 0), 0);
        SelectMode();
    }
    if (id == 222 || id == 223) {
        if (id == 222 || confirm(L"关闭三路输出，调用设备预设后保持掉电？"))
            Submit({{"type", "settings"},
                    {"operation", id == 222 ? "savePreset" : "recallPreset"},
                    {"slot", static_cast<int>(SendMessageW(Control(220), CB_GETCURSEL, 0, 0)) + 1}});
    }
    if (id == 224 || id == 225)
        Profile(id == 224);
    if (id == 233)
        op("beep");
    if (id == 235)
        op("clearText");
    if (id == 236)
        op("readPanel");
    if (id == 237)
        Submit({{"type", "settings"},
                {"operation", "panel"},
                {"control", SendMessageW(Control(230), CB_GETCURSEL, 0, 0) == 0 ? "local" : "remote"},
                {"locked", SendMessageW(Control(231), BM_GETCHECK, 0, 0) == BST_CHECKED},
                {"display", SendMessageW(Control(232), BM_GETCHECK, 0, 0) == BST_CHECKED},
                {"text", WideToMultiByte(Text(Control(234)), CP_UTF8)}});
    if (id >= 242 && id <= 247) {
        const char *names[] = {"identity", "errors", "status", "selfTest", "scpi", "reset"};
        if (id == 246) {
            ShowScpi();
            return;
        }
        if ((id == 245 || id == 247) &&
            !confirm(id == 247 ? L"关闭三路输出并复位设备？复位会清除当前设备设置。" : L"关闭三路输出后执行设备自检？"))
            return;
        Submit({{"type", "diagnostic"}, {"operation", names[id - 242]}});
    }
}
void PowerPane::PopulateSettings() {
    int ch = std::clamp(static_cast<int>(SendMessageW(Control(200), CB_GETCURSEL, 0, 0)), 0, 2);
    auto item = state_["channels"][ch].value("deviceProtection", Json::object());
    for (int i = 0; i < 4; ++i) {
        auto key = std::vector<const char *>{"limit", "ovp", "softwareVoltage", "softwareCurrent"}[i];
        SetWindowTextW(Control(202 + i), item.contains(key) ? Value(item[key]).c_str() : L"—");
    }
    SendMessageW(Control(201), BM_SETCHECK, item.value("enabled", false) ? BST_CHECKED : BST_UNCHECKED, 0);
    auto panel = state_.value("panel", Json::object());
    if (panel.contains("display"))
        SendMessageW(Control(232), BM_SETCHECK, panel["display"].get<bool>() ? BST_CHECKED : BST_UNCHECKED, 0);
    if (panel.contains("text"))
        SetWindowTextW(Control(234), Wide(panel["text"]).c_str());
    populated_ = true;
}
void PowerPane::Refresh() {
    if (!service_)
        return;
    bool wasConnected = state_.value("connected", false);
    auto nextState=service_->State();
    if(nextState==state_ && pendingActions_.empty() && !refreshRequired_)return;
    state_=std::move(nextState);
    refreshRequired_=false;
    bool connected = state_.value("connected", false), running = state_["task"].value("running", false);
    if (wasConnected != connected) {
        Layout();
        PostMessageW(GetParent(window_), PowerStateChanged, 0, 0);
        populated_ = false;
        if (!connected) {
            history_.clear();
            sampleStamp_.clear();
        }
    }
    bool finished = false, parametersChanged = false, deviceSettingsChanged = false;
    std::vector<std::pair<std::wstring, UINT>> notices;
    Json result;
    for (auto it = pendingActions_.begin(); !reporting_ && it != pendingActions_.end();) {
        result = service_->Action(*it);
        auto status = result.value("state", std::string());
        if (status == "completed" || status == "failed" || status == "canceled") {
            it = pendingActions_.erase(it);
            finished = true;
            auto command = result.value("command", Json::object());
            auto type = command.value("type", std::string());
            auto operation = command.value("operation", std::string());
            parametersChanged = parametersChanged || type == "connect" || type == "mode" || type == "batch" ||
                                type == "parameters" || type == "step" || type == "trigger" || operation == "reset" ||
                                operation == "recallPreset" || operation == "scpi";
            deviceSettingsChanged = deviceSettingsChanged || type == "settings" || type == "deviceTimer";
            if (status == "failed")
                notices.emplace_back(Wide(result.value("error", std::string("操作失败"))), MB_ICONWARNING);
            else if (result.contains("result") && type == "diagnostic" && operation != "reset")
                notices.emplace_back(Wide(result["result"].dump(2)), MB_ICONINFORMATION);
        } else
            ++it;
    }
    action_ = pendingActions_.empty() ? std::string() : pendingActions_.front();
    if (connected && (finished || !wasConnected ||
                      (action_.empty() && state_.value("mode", std::string("independent")) != Mode()))) {
        auto mode = state_.value("mode", std::string("independent"));
        int index = mode == "parallel" ? 1 : mode == "series" ? 2 : mode == "tracking" ? 3 : 0;
        bool changed = index != draftMode_;
        draftMode_ = index;
        SendMessageW(Control(80), CB_SETCURSEL, index, 0);
        SendMessageW(Control(210), CB_SETCURSEL, index, 0);
        if (changed || !wasConnected || parametersChanged) {
            for (int i = 0; i < 3; ++i) {
                auto ch = state_["channels"][i];
                Json v = ch["setVoltage"], a = ch["setCurrent"];
                if (i == 0 && (index == 1 || index == 2)) {
                    auto second = state_["channels"][1];
                    if (index == 2 && v.is_number() && second["setVoltage"].is_number())
                        v = v.get<double>() + second["setVoltage"].get<double>();
                    if (index == 1 && a.is_number() && second["setCurrent"].is_number())
                        a = a.get<double>() + second["setCurrent"].get<double>();
                }
                if(GetFocus()!=voltage_[i])SetWindowTextW(voltage_[i], Value(v).c_str());
                if(GetFocus()!=current_[i])SetWindowTextW(current_[i], Value(a).c_str());
            }
        }
        if (changed && index != 0) {
            bool selected = SendMessageW(check_[0], BM_GETCHECK, 0, 0) == BST_CHECKED ||
                            SendMessageW(check_[1], BM_GETCHECK, 0, 0) == BST_CHECKED;
            for (int i = 0; i < 2; ++i)
                SendMessageW(check_[i], BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (changed)
            Layout();
        if (deviceSettingsChanged || !wasConnected)
            PopulateSettings();
        if (state_.contains("timer") && deviceSettingsChanged) {
            auto timer = state_["timer"];
            SendMessageW(Control(124), CB_SETCURSEL, timer.value("enabled", false) ? 1 : 0, 0);
            SetWindowTextW(Control(125), Value(timer["seconds"]).c_str());
        }
    }
    EnableWindow(buttons_[0], !connected && action_.empty());
    EnableWindow(buttons_[1], connected);
    EnableWindow(buttons_[2], connected && !running && !Selection().empty());
    EnableWindow(buttons_[3], connected && !Selection().empty());
    EnableWindow(Control(81), connected && !running && !Selection().empty());
    EnableWindow(Control(80), !running && action_.empty());
    EnableWindow(Control(103), PowerService::ConnectionAvailable());
    for (int i = 0; i < 3; ++i) {
        EnableWindow(voltage_[i], connected && !running);
        EnableWindow(current_[i], connected && !running);
        if (i == 1 && draftMode_ == 3) {
            EnableWindow(voltage_[i], FALSE);
            EnableWindow(current_[i], FALSE);
        }
    }
    EnableWindow(start_, connected && !running && !Selection().empty());
    EnableWindow(stop_, connected && (running || automation_ == 3 ||
                                      (state_.contains("ownedChannels") && !state_["ownedChannels"].empty())));
    for (auto item : controls_)
        if (item.first >= 201 && item.first <= 247)
            EnableWindow(item.second, connected && !running);
    for (int id : {210, 211, 212, 213, 220, 221, 230})
        EnableWindow(Control(id), connected && !running);
    for (int id = 133; id <= 137; ++id)
        EnableWindow(Control(id), connected && !running);
    EnableWindow(Control(224), !running);
    EnableWindow(Control(225), !running);
    EnableWindow(Control(241), !logExportBusy_ && state_.contains("logPath"));
    EnableWindow(Control(101), !logExportBusy_ && state_.contains("csvPath"));
    EnableWindow(Control(240), !logExportBusy_ && state_.contains("csvPath"));
    EnableWindow(Control(248), connected && recoveryEligible_ && !Selection().empty());
    if (recoveryBinding_.is_object() && !RecoveryBinding().value("enabled", false)) {
        recoveryBinding_ = nullptr;
        SendMessageW(Control(248), BM_SETCHECK, BST_UNCHECKED, 0);
        if (state_.value("task", Json::object()).value("type", std::string()) == "recover" && running)
            service_->Submit({{"type", "stop"}, {"source", "recovery-binding-invalidated"}});
    }
    if (connected && !paused_) {
        auto stamp = state_["channels"][0]["timestamp"];
        if (stamp.is_string() && stamp.get<std::string>() != sampleStamp_) {
            sampleStamp_ = stamp;
            history_.push_back(state_["channels"]);
            if (history_.size() > 150)
                history_.pop_front();
        }
    }
    InvalidateRect(window_, nullptr, FALSE);
    InvalidateRect(body_, nullptr, FALSE);
    if (!notices.empty()) {
        reporting_ = true;
        for (auto &notice : notices)
            MessageBoxW(GetAncestor(window_, GA_ROOT), notice.first.c_str(), L"电源管理", MB_OK | notice.second);
        reporting_ = false;
    }
}
std::string PowerPane::Mode() const {
    static const char *modes[] = {"independent", "parallel", "series", "tracking"};
    return modes[std::clamp(draftMode_, 0, 3)];
}
HWND PowerPane::Combo(int id, const std::vector<std::wstring> &items, int selected) {
    HWND h = Child(L"COMBOBOX", L"", id, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
    for (const auto &s : items)
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
    SendMessageW(h, CB_SETCURSEL, selected, 0);
    UiStyleField(h, &fieldStyle_);
    return h;
}
void PowerPane::CreateControls() {
    const wchar_t *names[] = {L"连接", L"断开", L"加电", L"掉电"};
    for (int i = 0; i < 4; ++i)
        buttons_[i] = Child(L"BUTTON", names[i], Connect + i, BS_OWNERDRAW);
    for (int i = 0; i < 3; ++i) {
        check_[i] = Child(L"BUTTON", (L"CH" + std::to_wstring(i + 1)).c_str(), 20 + i, BS_AUTOCHECKBOX);
        voltage_[i] = Child(L"EDIT", L"0", 50 + i, ES_AUTOHSCROLL);
        current_[i] = Child(L"EDIT", L"0", 60 + i, ES_AUTOHSCROLL);
    }
    Combo(80, {L"独立", L"CH1＋CH2 并联", L"CH1＋CH2 串联", L"跟踪"});
    Child(L"BUTTON", L"应用设置", 81, BS_OWNERDRAW);
    Child(L"BUTTON", L"读取", 133, BS_OWNERDRAW);
    for (int i = 0; i < 4; ++i)
        Child(L"BUTTON", std::vector<std::wstring>{L"电压升步", L"电压降步", L"电流升步", L"电流降步"}[i].c_str(),
              134 + i, BS_OWNERDRAW);
    for (int i = 0; i < 4; ++i)
        Child(L"BUTTON", std::vector<std::wstring>{L"监测", L"自动化", L"设备设置", L"日志与诊断"}[i].c_str(), 90 + i,
              BS_OWNERDRAW);
    Child(L"BUTTON", L"暂停显示", 100, BS_OWNERDRAW);
    Child(L"BUTTON", L"导出 CSV", 101, BS_OWNERDRAW);
    Combo(103, {L"1 秒", L"2 秒"});
    for (int i = 0; i < 5; ++i)
        Child(L"BUTTON",
              std::vector<std::wstring>{L"循环测试", L"定时任务", L"步骤序列", L"设备定时器", L"触发与步进"}[i].c_str(),
              110 + i, BS_OWNERDRAW);
    onTime_ = Child(L"EDIT", L"5", 70, ES_AUTOHSCROLL);
    offTime_ = Child(L"EDIT", L"5", 71, ES_AUTOHSCROLL);
    count_ = Child(L"EDIT", L"10", 72, ES_NUMBER);
    start_ = Child(L"BUTTON", L"开始测试", Start, BS_OWNERDRAW);
    stop_ = Child(L"BUTTON", L"停止并掉电", Stop, BS_OWNERDRAW);
    save_ = Child(L"BUTTON", L"保存", Save, BS_OWNERDRAW);
    Child(L"BUTTON", L"载入", 44, BS_OWNERDRAW);
    Child(L"EDIT", L"", 120, ES_AUTOHSCROLL);
    Child(L"EDIT", L"", 121, ES_AUTOHSCROLL);
    Combo(122, {L"单次"});
    Child(L"BUTTON", L"编辑步骤", 123, BS_OWNERDRAW);
    Combo(124, {L"关闭", L"开启"});
    Child(L"EDIT", L"5", 125, ES_AUTOHSCROLL);
    Child(L"EDIT", L"0", 126, ES_AUTOHSCROLL);
    Child(L"EDIT", L"0.100", 127, ES_AUTOHSCROLL);
    Child(L"EDIT", L"0.100", 128, ES_AUTOHSCROLL);
    for (int i = 0; i < 4; ++i)
        Child(L"BUTTON", std::vector<std::wstring>{L"输出与保护", L"通道组合", L"设备预设", L"面板与控制"}[i].c_str(),
              190 + i, BS_OWNERDRAW);
    Combo(200, {L"CH1", L"CH2", L"CH3"});
    Child(L"BUTTON", L"设备过压保护", 201, BS_AUTOCHECKBOX);
    for (int i = 202; i <= 205; ++i)
        Child(L"EDIT", L"—", i, ES_AUTOHSCROLL);
    Child(L"BUTTON", L"读取", 206, BS_OWNERDRAW);
    Child(L"BUTTON", L"清除保护", 207, BS_OWNERDRAW);
    Child(L"BUTTON", L"应用", 208, BS_OWNERDRAW);
    Combo(210, {L"独立", L"并联", L"串联", L"跟踪"});
    Combo(211, {L"CH1＋CH2"});
    Child(L"EDIT", L"1 : 1", 212, ES_READONLY);
    Child(L"EDIT", L"1 : 1", 213, ES_READONLY);
    Child(L"BUTTON", L"读取", 214, BS_OWNERDRAW);
    Child(L"BUTTON", L"应用", 215, BS_OWNERDRAW);
    std::vector<std::wstring> slots;
    for (int i = 1; i <= 36; ++i)
        slots.push_back(std::to_wstring(i));
    Combo(220, slots);
    Combo(221, {L"当前方案"});
    for (int i = 0; i < 4; ++i)
        Child(L"BUTTON",
              std::vector<std::wstring>{L"保存到设备", L"调用设备预设", L"保存到文件", L"载入文件"}[i].c_str(), 222 + i,
              BS_OWNERDRAW);
    Combo(230, {L"本地", L"远程"}, 1);
    Child(L"BUTTON", L"锁定面板", 231, BS_AUTOCHECKBOX);
    Child(L"BUTTON", L"屏幕显示", 232, BS_AUTOCHECKBOX);
    Child(L"BUTTON", L"蜂鸣", 233, BS_OWNERDRAW);
    Child(L"EDIT", L"", 234, ES_AUTOHSCROLL);
    Child(L"BUTTON", L"清除文字", 235, BS_OWNERDRAW);
    Child(L"BUTTON", L"读取", 236, BS_OWNERDRAW);
    Child(L"BUTTON", L"应用", 237, BS_OWNERDRAW);
    Child(L"BUTTON", L"测量 CSV", 240, BS_OWNERDRAW);
    Child(L"BUTTON", L"导出日志", 241, BS_OWNERDRAW);
    Child(L"BUTTON", L"允许 AI 恢复关联串口", 248, BS_AUTOCHECKBOX);
    for (int i = 0; i < 6; ++i)
        Child(L"BUTTON",
              std::vector<std::wstring>{L"设备信息", L"错误队列", L"状态寄存器", L"自检", L"SCPI 调试", L"设备复位"}[i]
                  .c_str(),
              242 + i, BS_OWNERDRAW);
    SendMessageW(Control(232), BM_SETCHECK, BST_CHECKED, 0);
    LoadTask();
    for (int i = 0; i < 2; ++i) {
        std::time_t next = std::time(nullptr) + 60 * (i + 1);
        std::tm local{};
        localtime_s(&local, &next);
        wchar_t value[40]{};
        std::wcsftime(value, 40, L"%Y-%m-%d %H:%M:%S", &local);
        SetWindowTextW(Control(120 + i), value);
    }
    for (const auto &entry : controls_) {
        const int id = entry.first;
        if (id != Connect && id != Disconnect && id != On && id != Off && id != Start && id != Stop && id != Save &&
            id != 44 && id != 248 && !(id >= 90 && id <= 93))
        {
            SetParent(entry.second, body_);
            if (auto frame = reinterpret_cast<HWND>(GetPropW(entry.second, L"SerialCtl.FieldFrame"))) SetParent(frame, body_);
        }
    }
}
void PowerPane::SelectMode() {
    int selected = static_cast<int>(SendMessageW(Control(80), CB_GETCURSEL, 0, 0));
    if (selected < 0 || selected == draftMode_)
        return;
    int old = draftMode_;
    if (old == 0) {
        independentSelection_ = 0;
        for (int i = 0; i < 3; ++i)
            if (SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                independentSelection_ |= 1 << i;
    }
    draftMode_ = selected;
    if (state_.value("connected", false)) {
        if (MessageBoxW(GetAncestor(window_, GA_ROOT),
                        L"切换工作模式前将关闭三路输出，切换完成后保持掉电。请确认接线与所选模式一致。",
                        L"切换工作模式", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            draftMode_ = old;
            SendMessageW(Control(80), CB_SETCURSEL, old, 0);
            return;
        }
        Submit({{"type", "mode"}, {"mode", Mode()}});
    }
    SendMessageW(Control(210), CB_SETCURSEL, draftMode_, 0);
    if (selected == 0)
        for (int i = 0; i < 3; ++i)
            SendMessageW(check_[i], BM_SETCHECK, independentSelection_ & (1 << i) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (draftMode_ != 0) {
        bool selected = SendMessageW(check_[0], BM_GETCHECK, 0, 0) == BST_CHECKED ||
                        SendMessageW(check_[1], BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(check_[0], BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(check_[1], BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    Layout();
    Refresh();
}
void PowerPane::Layout() {
    if (!body_) return;
    UiLayoutBatch batch;
    UiLayoutBatch bodyBatch;
    currentLayout_ = &bodyBatch;
    RECT client{}; GetClientRect(window_, &client);
    const auto d = [this](int n) { return MulDiv(n, dpi_, 96); };
    const int width = static_cast<int>(client.right);
    const bool connected = state_.value("connected", false);
    for (const auto &entry : controls_) {
        const bool card = GetParent(entry.second) == body_ && (tab_ != 0 || (entry.first >= 20 && entry.first <= 62));
        const COLORREF backdrop = card ? UiTheme(dark_).surface : UiAiTheme(dark_).surface;
        SetPropW(entry.second, L"SerialCtl.Backdrop", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(backdrop) + 1));
    }
    const bool recovery = state_.value("task", Json::object()).value("running", false) &&
        state_["task"].value("type", std::string()) == "recover";
    for (const auto &entry : controls_)
        (GetParent(entry.second) == body_ ? bodyBatch : batch).Show(entry.second, SW_HIDE);
    auto fixed = [&](int id, int x, int y, int w, int h) {
        batch.Move(Control(id), x, y, std::max(1, w), h);
        batch.Show(Control(id), SW_SHOW);
    };
    const int headerHeight = width < d(400) ? d(52) : d(36);
    fixed(connected ? Disconnect : Connect, width - d(96), 0, d(96), d(36));
    HDC dc = GetDC(window_); const auto old = SelectObject(dc, smallFont_);
    int x = 0, y = headerHeight + d(12);
    for (int id = 90; id <= 93; ++id) {
        const auto label = Text(Control(id)); SIZE size{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size);
        const int w = std::min(width, static_cast<int>(size.cx) + d(32));
        if (x && x + w > width) { x = 0; y += d(44); }
        fixed(id, x, y, w, d(32)); x += w + d(12);
    }
    SelectObject(dc, old); ReleaseDC(window_, dc);
    bodyTop_ = y + d(44);
    const int actionRows = tab_ == 1 && width < d(440) ? 2 : 1;
    const int footerHeight = d(36 + 12 + 36 * actionRows + 12 * (actionRows - 1));
    footerTop_ = std::max(bodyTop_ + d(48), static_cast<int>(client.bottom) - footerHeight);
    bodyHeight_ = std::max(d(48), footerTop_ - bodyTop_ - d(12));
    batch.Move(body_, 0, bodyTop_, width, bodyHeight_, TRUE);
    batch.Show(body_, SW_SHOW);
    SetWindowTextW(Control(248), width < d(400) ? L"允许 AI 恢复" : L"允许 AI 恢复关联串口");
    fixed(248, 0, footerTop_ + d(4), std::max(d(112), width - (recovery ? d(108) : 0)), d(28));
    if (recovery && tab_ != 1) {
        SetWindowTextW(stop_, L"停止恢复");
        fixed(Stop, width - d(96), footerTop_, d(96), d(36));
    } else SetWindowTextW(stop_, L"停止测试");
    const int actionTop = footerTop_ + d(48);
    if (tab_ == 1) {
        const int ids[]{Start, Stop, Save, 44};
        const int cols = actionRows == 1 ? 4 : 2;
        const int w = (width - (cols - 1) * d(12)) / cols;
        for (int i = 0; i < 4; ++i) fixed(ids[i], (i % cols) * (w + d(12)),
            actionTop + (i / cols) * d(48), w, d(36));
        SetWindowTextW(start_, automation_ == 3 ? L"应用定时器" : automation_ == 4 ? L"应用并触发" : L"开始测试");
    } else {
        const int button = d(width < d(400) ? 72 : 96);
        fixed(On, width - button * 2 - d(12), actionTop, button, d(36));
        fixed(Off, width - button, actionTop, button, d(36));
    }
    auto measure = [&](int w) {
        bodyWidth_ = w;
        channelRects_ = {};
        cardColumns_ = w >= d(780) ? 3 : 1;
        const bool group = draftMode_ == 1 || draftMode_ == 2;
        if (tab_ == 0) {
            const bool narrowMode = w < d(400);
            cardTop_ = narrowMode ? d(88) : d(52);
            cardWidth_ = (w - d(12) * (cardColumns_ - 1)) / cardColumns_;
            cardHeight_ = d(cardWidth_ >= d(440) ? 144 : 184);
            const int modeWidth = narrowMode ? w - d(76) : std::min(d(200), w - d(192));
            bodyBatch.Move(Control(80), d(76), -scroll_, std::max(d(64), modeWidth), d(180));
            bodyBatch.ComboVisibleHeight(Control(80), d(28)); bodyBatch.Show(Control(80), SW_SHOW);
            bodyBatch.Move(Control(81), w - d(104), (narrowMode ? d(40) : 0) - scroll_, d(104), d(28));
            bodyBatch.Show(Control(81), SW_SHOW);
            int bottom = cardTop_;
            for (int i = 0; i < 3; ++i) {
                if (group && i == 1) continue;
                const int index = group && i == 2 ? 1 : i;
                const int col = i % cardColumns_;
                int left = col * (cardWidth_ + d(12));
                int cw = cardWidth_;
                if (group && cardColumns_ == 3 && i == 0) cw = cardWidth_ * 2 + d(12);
                const int top = cardTop_ + (index / cardColumns_) * (cardHeight_ + d(12));
                channelRects_[i] = {left, top, left + cw, top + cardHeight_};
                bottom = std::max(bottom, top + cardHeight_);
                SetWindowTextW(check_[i], group && i == 0 ? L"CH1＋CH2" : (L"CH" + std::to_wstring(i + 1)).c_str());
                bodyBatch.Move(check_[i], left + d(16), top + d(8) - scroll_, d(group && i == 0 ? 144 : 72), d(28));
                bodyBatch.Show(check_[i], SW_SHOW);
                const bool inlineFields = cw >= d(440);
                const int half = (cw - d(44)) / 2;
                const int label = d(group && i == 0 ? 120 : 100);
                bodyBatch.Move(voltage_[i], left + d(16) + label, top + d(104) - scroll_,
                    std::max(d(48), (inlineFields ? half : cw - d(32)) - label), d(28));
                bodyBatch.Move(current_[i], left + (inlineFields ? d(28) + half : d(16)) + label,
                    top + d(inlineFields ? 104 : 144) - scroll_,
                    std::max(d(48), (inlineFields ? half : cw - d(32)) - label), d(28));
                bodyBatch.Show(voltage_[i], SW_SHOW); bodyBatch.Show(current_[i], SW_SHOW);
            }
            detailsInset_ = 0;
            detailsTop_ = bottom + d(20);
        } else {
            detailsInset_ = d(16);
            detailsTop_ = d(16);
        }
        DetailsLayout(std::max(d(100), w - detailsInset_ * 2), detailsTop_);
    };
    measure(width);
    if (contentHeight_ > bodyHeight_) measure(std::max(d(100), width - d(16)));
    const int clamped = std::clamp(scroll_, 0, std::max(0, contentHeight_ - bodyHeight_));
    if (clamped != scroll_) { scroll_ = clamped; measure(bodyWidth_); }
    currentLayout_ = nullptr;
    batch.Commit();
    bodyBatch.Commit();
    InvalidateRect(body_, nullptr, FALSE);
    InvalidateRect(window_, nullptr, FALSE);
}

void PowerPane::DetailsLayout(int width, int top) {
    const auto d = [this](int n) { return MulDiv(n, dpi_, 96); };
    const int w = MulDiv(width, 96, dpi_);
    detailLabels_.clear();
    auto label = [&](const std::wstring &text, int x, int y, int size, int height = 24) {
        detailLabels_.push_back({text, {detailsInset_ + d(x), top + d(y), detailsInset_ + d(x + size), top + d(y + height)}});
    };
    auto move = [&](int id, int x, int y, int size, int height = 28) {
        HWND h = Control(id); if (!h) return;
        wchar_t cls[32]{}; GetClassNameW(h, cls, 32);
        const bool combo = _wcsicmp(cls, L"COMBOBOX") == 0;
        currentLayout_->Move(h, detailsInset_ + d(x), top + d(y) - scroll_, d(std::max(1, size)), d(combo ? 180 : height));
        if (combo) currentLayout_->ComboVisibleHeight(h, d(height));
        currentLayout_->Show(h, SW_SHOW);
    };
    auto flow = [&](const std::vector<int> &ids, int y) {
        int x = 0;
        HDC dc = GetDC(window_); const auto old = SelectObject(dc, smallFont_);
        for (int id : ids) {
            const auto text = Text(Control(id)); SIZE extent{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
            const int size = std::min(w, std::max(72, MulDiv(extent.cx, 96, dpi_) + 32));
            if (x && x + size > w) { x = 0; y += 44; }
            move(id, x, y, size, 32); x += size + 12;
        }
        SelectObject(dc, old); ReleaseDC(window_, dc);
        return y + 44;
    };
    auto fields = [&](const std::vector<std::pair<int, std::wstring>> &items, int y) {
        const int cols = w >= 420 ? 2 : 1;
        const int size = (w - (cols - 1) * 12) / cols;
        for (size_t i = 0; i < items.size(); ++i) {
            const int x = static_cast<int>(i % cols) * (size + 12);
            const int row = y + static_cast<int>(i / cols) * 72;
            label(items[i].second, x, row, size);
            move(items[i].first, x, row + 28, size);
        }
        return y + static_cast<int>((items.size() + cols - 1) / cols) * 72;
    };
    int y = 0;
    if (tab_ == 0) {
        label(L"实时趋势", 0, 0, w);
        y = flow({100, 101}, 32);
        label(L"采样", 0, y, 64); move(103, 76, y, std::min(104, w - 76));
        detailStatusTop_ = top + d(y + 44);
        contentHeight_ = detailStatusTop_ + d(w < 360 ? 400 : 200);
        return;
    }
    if (tab_ == 1) {
        y = flow({110, 111, 112, 113, 114}, 0) + 12;
        if (automation_ == 0) y = fields({{70,L"上电时长（秒）"},{71,L"断电等待（秒）"},{72,L"循环次数"}}, y);
        if (automation_ == 1) y = fields({{120,L"开始时间"},{121,L"掉电时间"},{122,L"重复"}}, y);
        if (automation_ == 2) y = fields({{123,L"步骤序列"}}, y);
        if (automation_ == 3) { y = fields({{124,L"设备输出定时器"},{125,L"掉电延时（秒）"}}, y); y = flow({133}, y); }
        if (automation_ == 4) {
            y = fields({{126,L"触发延时（秒）"},{127,L"电压步进（V）"},{128,L"电流步进（A）"}}, y);
            y = flow({134,135,136,137}, y);
        }
        label(L"任务状态", 0, y + 12, w);
        detailStatusTop_ = top + d(y + 44);
        contentHeight_ = detailStatusTop_ + d(120);
    } else if (tab_ == 2) {
        y = flow({190,191,192,193}, 0) + 12;
        if (settings_ == 0) {
            y = fields({{200,L"通道"},{202,L"电压设置上限（V）"},{203,L"设备过压阈值（V）"},
                {204,L"软件电压阈值（V）"},{205,L"软件电流阈值（A）"}}, y);
            y = flow({201}, y); y = flow({206,207,208}, y);
        } else if (settings_ == 1) {
            y = fields({{210,L"组合模式"},{211,L"关联通道"},{212,L"电压跟踪比例"},{213,L"电流跟踪比例"}}, y);
            y = flow({214,215}, y);
        } else if (settings_ == 2) {
            y = fields({{220,L"设备存储位置"},{221,L"本地方案"}}, y);
            y = flow({222,223,224,225}, y);
        } else {
            y = fields({{230,L"控制方式"},{234,L"屏幕文字"}}, y);
            y = flow({231,232,233}, y); y = flow({235,236,237}, y);
        }
        contentHeight_ = top + d(y + 16);
    } else {
        y = flow({240,241}, 0); y = flow({242,243,244,245,246,247}, y);
        label(L"操作记录", 0, y + 12, w);
        detailStatusTop_ = top + d(y + 44);
        const auto records = state_.value("records", Json::array());
        contentHeight_ = detailStatusTop_ + d(static_cast<int>(std::min<size_t>(12, records.size())) * 64 + 32);
    }
    contentHeight_ += d(16);
}

void PowerPane::Paint(HDC dc) {
    RECT r{}; GetClientRect(window_, &r);
    const auto d = [this](int n) { return MulDiv(n, dpi_, 96); };
    const auto c = UiTheme(dark_);
    FillRect(dc, &r, panelBrush_);
    auto text = [&](const std::wstring &value, RECT box, HFONT font, COLORREF ink, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        const auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, ink);
        DrawTextW(dc, value.c_str(), -1, &box, flags | DT_NOPREFIX); SelectObject(dc, old);
    };
    const bool narrow = r.right < d(400);
    icons_.Draw(dc, ToolbarIcon::Power, {0, d(8), d(18), d(26)}, dark_);
    text(L"IT6332A", {d(28),0,r.right-d(108),d(36)}, titleFont_,c.text);
    text(state_.value("connected",false) ? L"已连接" : L"未连接",
        {narrow ? 0 : d(144), narrow ? d(28) : 0, r.right-d(108), narrow ? d(52) : d(36)}, smallFont_,c.muted);
    UiSeparator(dc, 0, r.right, bodyTop_ - d(8), c.border);
    UiSeparator(dc, 0, r.right, footerTop_ - d(8), c.border);
    if (tab_ != 1) {
        std::wstring selected = L"已选";
        for (auto channel : Selection()) selected += L" CH" + std::to_wstring(channel.get<int>());
        text(selected, {0,footerTop_+d(48), r.right-d(r.right<d(400)?168:216), footerTop_+d(84)},smallFont_,c.muted);
    }
}

void PowerPane::PaintBody(HDC dc) {
    RECT r{}; GetClientRect(body_, &r);
    const auto d = [this](int n) { return MulDiv(n, dpi_, 96); };
    const auto c = UiTheme(dark_);
    FillRect(dc, &r, panelBrush_);
    const int saved = SaveDC(dc);
    SetViewportOrgEx(dc,0,-scroll_,nullptr);
    auto text = [&](const std::wstring &value, RECT box, HFONT font, COLORREF ink) {
        const auto old = SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,ink);
        DrawTextW(dc,value.c_str(),-1,&box,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,old);
    };
    if (tab_ == 0) {
        text(L"工作模式",{0,0,d(72),d(28)},font_,c.text);
        for (int i=0;i<3;++i) {
            const RECT card=channelRects_[i];
            if (card.right<=card.left) continue;
            UiBox(dc,card,c.surface,UiPanelBorder(dark_),d(12));
            if (!state_.contains("channels")) continue;
            auto channel=state_["channels"][i];
            const bool combined=(draftMode_==1||draftMode_==2)&&i==0;
            if (combined) {
                const auto other=state_["channels"][1];
                for (const char *key : {"voltage","current","power"}) {
                    if (!channel[key].is_number() || !other[key].is_number()) channel[key]=nullptr;
                    else if (std::string(key)=="power" || (draftMode_==1&&std::string(key)=="current") ||
                        (draftMode_==2&&std::string(key)=="voltage"))
                        channel[key]=channel[key].get<double>()+other[key].get<double>();
                }
                if (channel["output"]!=other["output"]) channel["output"]=nullptr;
            }
            const int left=card.left, top=card.top, width=card.right-card.left;
            const std::wstring status=channel["output"].is_boolean() ? (channel["output"].get<bool>()?L"输出中":L"已关闭") :
                state_.value("connected",false)?L"状态未知":L"未连接";
            RECT statusBox{left+d(combined?164:100),top+d(8),card.right-d(16),top+d(36)};
            const auto old=SelectObject(dc,smallFont_); SetTextColor(dc,c.muted); SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,status.c_str(),-1,&statusBox,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); SelectObject(dc,old);
            const wchar_t *labels[]{L"电压（V）",L"电流（A）",L"功率（W）"};
            const char *keys[]{"voltage","current","power"};
            const int col=(width-d(32))/3;
            for (int n=0;n<3;++n) {
                const int x=left+d(16)+n*col;
                text(labels[n],{x,top+d(40),x+col,top+d(60)},smallFont_,c.muted);
                text(Value(channel[keys[n]]),{x,top+d(60),x+col,top+d(92)},valueFont_,c.text);
            }
            const bool inlineFields=width>=d(440);
            const int half=(width-d(44))/2;
            text(combined&&draftMode_==2?L"总电压（V）":L"设定电压（V）",
                {left+d(16),top+d(104),left+d(combined?132:112),top+d(132)},smallFont_,c.muted);
            const int currentLeft=left+(inlineFields?d(28)+half:d(16));
            text(combined&&draftMode_==1?L"总电流限值（A）":L"电流限值（A）",
                {currentLeft,top+d(inlineFields?104:144),currentLeft+d(combined?120:100),top+d(inlineFields?132:172)},smallFont_,c.muted);
        }
    } else UiBox(dc,{0,0,bodyWidth_,contentHeight_},c.surface,UiPanelBorder(dark_),d(12));
    PaintDetails(dc,bodyWidth_-detailsInset_*2,detailsTop_);
    RestoreDC(dc,saved);
    if (contentHeight_>bodyHeight_) {
        UiBox(dc,ScrollThumb(),c.border,c.border,d(3));
    }
}

void PowerPane::PaintDetails(HDC dc,int width,int) {
    const auto d=[this](int n){return MulDiv(n,dpi_,96);};
    const auto c=UiTheme(dark_);
    auto text=[&](const std::wstring &value,RECT box,COLORREF color,HFONT font=nullptr,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS){
        const auto old=SelectObject(dc,font?font:smallFont_);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
        DrawTextW(dc,value.c_str(),-1,&box,flags|DT_NOPREFIX);SelectObject(dc,old);
    };
    for(const auto &label:detailLabels_) text(label.first,label.second,c.muted);
    if(tab_==0){
        const bool stack=width<d(360);
        const int plotWidth=stack?width:(width-d(12))/2;
        for(int metric=0;metric<2;++metric){
            const int x=stack?0:metric*(plotWidth+d(12));
            const int y=detailStatusTop_+(stack?metric*d(200):0);
            UiBox(dc,{x,y,x+plotWidth,y+d(188)},c.surface,c.border,d(12));
            text(metric?L"电流（A）":L"电压（V）",{x+d(12),y+d(8),x+plotWidth-d(12),y+d(32)},c.muted);
            const int left=x+d(12),right=x+plotWidth-d(12),top=y+d(44),bottom=y+d(172);
            for(int n=0;n<=4;++n) UiSeparator(dc,left,right,top+(bottom-top)*n/4,c.border);
            double maximum=0.001;
            for(const auto &sample:history_) for(const auto &channel:sample){const auto value=channel[metric?"current":"voltage"];if(value.is_number())maximum=std::max(maximum,value.get<double>());}
            for(int channel=0;channel<3;++channel){
                std::vector<POINT> points;size_t index=0;
                for(const auto &sample:history_){
                    const auto value=sample[channel][metric?"current":"voltage"];
                    if(value.is_number())points.push_back({left+static_cast<LONG>((right-left)*index/std::max<size_t>(1,history_.size()-1)),
                        bottom-static_cast<LONG>((bottom-top)*std::clamp(value.get<double>()/maximum,0.0,1.0))});
                    ++index;
                }
                UiPolyline(dc,points.data(),static_cast<int>(points.size()),channel==0?c.accent:channel==1?c.text:c.muted,1.0f);
            }
        }
    }else if(tab_==1){
        const auto task=state_.value("task",Json::object());
        const bool running=task.value("running",false);
        std::wstring status=running?L"运行中":L"未运行";
        if(task.value("type",std::string())=="recover")status=running?
            L"断电等待 · "+std::to_wstring((task.value("remainingMs",std::uint64_t(0))+999)/1000)+L" 秒":
            task.value("phase",std::string())=="on_confirmed"?L"上电已确认":L"恢复已停止";
        else if(running)status+=L" · 第 "+std::to_wstring(task.value("completed",0)+1)+L" / "+std::to_wstring(task.value("count",1))+L" 轮";
        if(state_.contains("error"))status=Wide(state_["error"]);
        text(status,{detailsInset_,detailStatusTop_,detailsInset_+width,detailStatusTop_+d(56)},
            state_.contains("error")?c.danger:c.text,font_,DT_LEFT|DT_WORDBREAK);
        if(task.value("type",std::string())=="recover"&&running){
            text(L"确认断电后等待满 10 秒，再上电并读取设备启动日志。",
                {detailsInset_,detailStatusTop_+d(64),detailsInset_+width,detailStatusTop_+d(112)},c.muted,nullptr,DT_LEFT|DT_WORDBREAK);
        }
    }else if(tab_==3){
        const auto records=state_.value("records",Json::array());
        const size_t start=records.size()>12?records.size()-12:0;
        for(size_t index=start;index<records.size();++index){
            const auto &record=records[index];const int y=detailStatusTop_+static_cast<int>(index-start)*d(64);
            text(Wide(record.value("timestamp",std::string()))+L" · "+Wide(record.value("operation",std::string())),
                {detailsInset_,y,detailsInset_+width,y+d(28)},c.text);
            text(Wide(record.value("source",std::string()))+L" · "+Wide(record.value("result",std::string())),
                {detailsInset_,y+d(28),detailsInset_+width,y+d(56)},c.muted);
        }
    }
}

LRESULT CALLBACK PowerPane::BodyProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto *self=reinterpret_cast<PowerPane*>(data);
    if(m==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);{UiBufferedPaint paint(dc,r);self->PaintBody(paint.Dc());}EndPaint(h,&ps);return 0;}
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_MOUSEWHEEL){self->Scroll(self->scroll_-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*MulDiv(48,self->dpi_,96));return 0;}
    if(m==WM_LBUTTONDOWN){
        RECT r{};GetClientRect(h,&r);
        if(GET_X_LPARAM(l)>=r.right-MulDiv(16,self->dpi_,96)&&self->contentHeight_>self->bodyHeight_){
            const auto thumb=self->ScrollThumb();const int y=GET_Y_LPARAM(l);
            self->scrollGrab_ = y>=thumb.top && y<thumb.bottom ? y-thumb.top : (thumb.bottom-thumb.top)/2;
            self->scrolling_=true;SetCapture(h);
        }
    }
    if((m==WM_MOUSEMOVE&&self->scrolling_)||(m==WM_LBUTTONDOWN&&self->scrolling_)){
        const auto thumb=self->ScrollThumb();
        self->Scroll(static_cast<int>(static_cast<long long>(GET_Y_LPARAM(l)-self->scrollGrab_)*
            std::max(0,self->contentHeight_-self->bodyHeight_)/std::max(1,self->bodyHeight_-static_cast<int>(thumb.bottom-thumb.top))));return 0;
    }
    if(m==WM_LBUTTONUP||m==WM_CAPTURECHANGED){self->scrolling_=false;if(GetCapture()==h)ReleaseCapture();}
    if(m==WM_COMMAND||m==WM_DRAWITEM||m==WM_MEASUREITEM||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLORBTN||
        m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX||m==WM_NOTIFY)return SendMessageW(self->window_,m,w,l);
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,BodyProc,id);
    return DefSubclassProc(h,m,w,l);
}

RECT PowerPane::ScrollThumb() const {
    RECT r{}; GetClientRect(body_,&r);
    const int height=std::min(bodyHeight_,std::max(MulDiv(28,dpi_,96),static_cast<int>(static_cast<long long>(bodyHeight_)*bodyHeight_/std::max(1,contentHeight_))));
    const int top=static_cast<int>(static_cast<long long>(scroll_)*(bodyHeight_-height)/std::max(1,contentHeight_-bodyHeight_));
    return {r.right-MulDiv(8,dpi_,96),top,r.right-MulDiv(3,dpi_,96),top+height};
}
void PowerPane::Scroll(int position) {
    const int next=std::clamp(position,0,std::max(0,contentHeight_-bodyHeight_));
    if(next==scroll_)return;
    const int delta=scroll_-next;scroll_=next;
    UiRedrawScope redraw(body_);
    std::vector<std::pair<HWND,RECT>> moves;
    for(HWND child=GetWindow(body_,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,body_,reinterpret_cast<POINT*>(&bounds),2);
        OffsetRect(&bounds,0,delta);moves.push_back({child,bounds});
    }
    const UINT flags=SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS;
    auto batch=BeginDeferWindowPos(static_cast<int>(moves.size()));
    for(const auto& move:moves) {
        if(!batch)break;
        batch=DeferWindowPos(batch,move.first,nullptr,move.second.left,move.second.top,0,0,flags);
    }
    if(!batch || !EndDeferWindowPos(batch))
        for(const auto& move:moves) SetWindowPos(move.first,nullptr,move.second.left,move.second.top,0,0,flags);
    InvalidateRect(body_,nullptr,FALSE);
}
LRESULT CALLBACK PowerPane::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto *self = reinterpret_cast<PowerPane *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<PowerPane *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    if (self->closing_ && (m == WM_TIMER || m == WM_COMMAND)) return 0;
    if (m == WM_CREATE) {
        self->Theme(self->dark_);
        self->body_ = CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
            0,0,100,100,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetWindowSubclass(self->body_,BodyProc,1,reinterpret_cast<DWORD_PTR>(self));
        self->CreateControls();
        self->Refresh();
        SetTimer(h, 1, 500, nullptr);
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        self->Scroll(self->scroll_ - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * MulDiv(48, self->dpi_, 96));
        return 0;
    }
    if (m == WM_SIZE) {
        self->Layout();
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, 1);
        return 0;
    }
    if (m == WM_TIMER) {
        HWND focus = GetFocus();
        if (focus && focus != self->lastFocus_ && IsChild(self->body_, focus) && IsWindowVisible(focus)) {
            RECT field{}, view{};
            GetWindowRect(focus, &field);
            MapWindowPoints(HWND_DESKTOP, self->body_, reinterpret_cast<POINT *>(&field), 2);
            GetClientRect(self->body_, &view);
            if (field.top < 0)
                self->Scroll(self->scroll_ + field.top);
            else if (field.bottom > view.bottom)
                self->Scroll(self->scroll_ + field.bottom - view.bottom);
        }
        self->lastFocus_ = focus;
        self->Refresh();
        return 0;
    }
    if (m == WM_COMMAND && (HIWORD(w) == BN_CLICKED || HIWORD(w) == CBN_SELCHANGE ||
                           (LOWORD(w) == Connect && HIWORD(w) == BN_DOUBLECLICKED))) {
        self->Action(LOWORD(w));
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_MEASUREITEM) {
        auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            item.itemHeight = MulDiv(28, self->dpi_, 96);
            return TRUE;
        }
    }
    if (m == WM_PAINT) {
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
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) {
        auto c = UiTheme(self->dark_);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
        wchar_t cls[24]{};
        GetClassNameW(reinterpret_cast<HWND>(l), cls, 24);
        bool edit = _wcsicmp(cls, L"EDIT") == 0;
        if (edit || m == WM_CTLCOLORLISTBOX) {
            SetTextColor(reinterpret_cast<HDC>(w), IsWindowEnabled(reinterpret_cast<HWND>(l)) ? c.text : c.muted);
            SetBkColor(reinterpret_cast<HDC>(w), c.field);
            return reinterpret_cast<LRESULT>(self->brush_);
        }
        bool channel = (id >= 20 && id <= 22) || id == 201 || id == 231 || id == 232;
        SetBkColor(reinterpret_cast<HDC>(w), m == WM_CTLCOLOREDIT ? c.field : channel ? c.surface : c.raised);
        return reinterpret_cast<LRESULT>(m == WM_CTLCOLOREDIT ? self->brush_
                                         : channel            ? self->surfaceBrush_
                                                              : self->fieldBrush_);
    }
    if (m == WM_DRAWITEM) {
        auto item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType == ODT_COMBOBOX) {
            UiDrawComboItem(item, self->fieldStyle_);
            return TRUE;
        }
        UiBufferedPaint paint(item.hDC, item.rcItem);
        item.hDC = paint.Dc();
        auto c = UiTheme(self->dark_);
        bool primary = item.CtlID == On || item.CtlID == Connect || item.CtlID == Start || item.CtlID == IDOK;
        bool mainTab = item.CtlID >= 90 && item.CtlID <= 93, subTab = item.CtlID >= 110 && item.CtlID <= 114,
             groupTab = item.CtlID >= 190 && item.CtlID <= 193;
        bool active = mainTab    ? item.CtlID == 90 + self->tab_
                      : subTab   ? item.CtlID == 110 + self->automation_
                      : groupTab ? item.CtlID == 190 + self->settings_
                                 : false;
        if (mainTab || subTab || groupTab) {
            const bool enabled = !(item.itemState & ODS_DISABLED);
            const bool hovered = enabled && GetPropW(item.hwndItem, L"SerialCtl.PowerHover");
            const bool pressed = enabled && (item.itemState & ODS_SELECTED);
            HBRUSH background = CreateSolidBrush(UiControlBackdrop(item.hwndItem, UiAiTheme(self->dark_).surface));
            FillRect(item.hDC, &item.rcItem, background);
            DeleteObject(background);
            const auto visual = UiButtonVisual(c, enabled, pressed, hovered);
            const COLORREF base = UiControlBackdrop(item.hwndItem, UiAiTheme(self->dark_).surface);
            const COLORREF fill = pressed ? c.accent : (active || hovered) ? (self->dark_ ? c.surface : c.raised) : base;
            if (fill != base)
                UiBox(item.hDC, item.rcItem, fill, fill, UiCapsuleRadius(item.rcItem));
            UiButtonKeyboardFocus(item.hDC, item.rcItem, item.itemState, self->dpi_,
                UiFocusInk(self->dark_, !(item.itemState & ODS_DISABLED) && (primary || (item.itemState & ODS_SELECTED))));
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, visual.ink);
            auto old = SelectObject(item.hDC, self->smallFont_);
            auto label = self->Text(item.hwndItem);
            RECT rect = item.rcItem;
            DrawTextW(item.hDC, label.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(item.hDC, old);
            return TRUE;
        }
        wchar_t parentClass[32]{};
        GetClassNameW(GetParent(item.hwndItem), parentClass, 32);
        HBRUSH outside = CreateSolidBrush(UiControlBackdrop(item.hwndItem,
            _wcsicmp(parentClass, L"#32770") == 0 ? c.surface : c.field));
        FillRect(item.hDC, &item.rcItem, outside);
        DeleteObject(outside);
        const auto visual = UiButtonVisual(c, !(item.itemState & ODS_DISABLED), item.itemState & ODS_SELECTED,
                                           GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, primary);
        UiBox(item.hDC, item.rcItem, visual.fill, (item.itemState & ODS_DISABLED) ? UiPanelBorder(self->dark_) : visual.border, UiCapsuleRadius(item.rcItem));
        UiButtonKeyboardFocus(item.hDC, item.rcItem, item.itemState, self->dpi_,
                UiFocusInk(self->dark_, !(item.itemState & ODS_DISABLED) && (primary || (item.itemState & ODS_SELECTED))));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, (item.itemState & ODS_DISABLED)           ? c.muted
                               : (item.itemState & ODS_SELECTED)         ? visual.ink
                               : item.CtlID == Off || item.CtlID == Stop ? c.danger
                                                                         : visual.ink);
        auto old = SelectObject(item.hDC, item.CtlID >= 90 ? self->smallFont_ : self->font_);
        RECT rect = item.rcItem;
        auto value = self->Text(item.hwndItem);
        DrawTextW(item.hDC, value.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old);
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
void PowerPane::ShowConnection() {
    ConnectionDialog data{this, font_, dark_, service_};
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    };
    Template t{};
    t.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dialog.cx = 238;
    t.dialog.cy = 240;
    UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &t.dialog, GetAncestor(window_, GA_ROOT), ConnectProc,
                            reinterpret_cast<LPARAM>(&data));
}
INT_PTR CALLBACK PowerPane::ConnectProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto *data = reinterpret_cast<ConnectionDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    if (m == WM_INITDIALOG) {
        data = reinterpret_cast<ConnectionDialog *>(l);
        SetWindowLongPtrW(h, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(h, L"连接电源");
        int dpi = data->pane->dpi_;
        auto d = [dpi](int v) { return MulDiv(v, dpi, 96); };
        UiResizeDialog(h, dpi, 436, 400);
        auto add = [&](const wchar_t *cls, const wchar_t *text, int id, int x, int y, int width, DWORD style = 0,
                       int height = 28) {
            const bool combo = _wcsicmp(cls, L"COMBOBOX") == 0;
            if (combo)
                style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
            auto c =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, d(x), d(y), d(width), d(height), h,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(data->font), TRUE);
            if (_wcsicmp(cls, L"BUTTON") == 0 && (style & BS_TYPEMASK) == BS_OWNERDRAW)
                UiStyleButton(c);
            if (_wcsicmp(cls, L"EDIT") == 0 || _wcsicmp(cls, L"COMBOBOX") == 0)
                UiStyleField(c, &data->pane->fieldStyle_);
            if (combo) {
                RECT bounds{};
                GetWindowRect(c, &bounds);
                const int current = static_cast<int>(SendMessageW(c, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
                SendMessageW(c, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                             std::max(1, current + d(28) - static_cast<int>(bounds.bottom - bounds.top)));
            }
            return c;
        };
        if (!PowerService::ConnectionAvailable()) {
            UiResizeDialog(h, dpi, 360, 132);
            add(L"STATIC", Wide(PowerService::ConnectionUnavailableMessage()).c_str(), -1, 16, 16, 328, 0, 48);
            add(L"BUTTON", L"确定", IDOK, 248, 80, 96, WS_TABSTOP | BS_OWNERDRAW, 36);
            RECT owner{}, rect{};
            GetWindowRect(GetParent(h), &owner);
            GetWindowRect(h, &rect);
            SetWindowPos(h, nullptr, owner.left + (owner.right - owner.left - (rect.right - rect.left)) / 2,
                         owner.top + (owner.bottom - owner.top - (rect.bottom - rect.top)) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
            return TRUE;
        }
        add(L"STATIC", L"连接方式", -1, 16, 12, 200);
        data->kind = add(L"COMBOBOX", L"", 100, 16, 40, 404, CBS_DROPDOWNLIST | WS_TABSTOP, 200);
        for (auto text : {L"USB", L"RS232"})
            SendMessageW(data->kind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(data->kind, CB_SETCURSEL, 0, 0);
        add(L"STATIC", L"设备 / COM 口", -1, 16, 80, 200);
        data->resource = add(L"COMBOBOX", L"", 101, 16, 108, 300, CBS_DROPDOWNLIST | WS_TABSTOP, 200);
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
        data->driver = add(L"BUTTON", L"安装 USB 驱动", 109, 16, 308, 144, WS_TABSTOP | BS_OWNERDRAW);
        add(L"BUTTON", L"取消", IDCANCEL, 280, 364, 96, WS_TABSTOP | BS_OWNERDRAW, 36);
        add(L"BUTTON", L"连接", IDOK, 392, 364, 96, WS_TABSTOP | BS_OWNERDRAW, 36);
        RECT owner{}, rect{};
        GetWindowRect(GetParent(h), &owner);
        GetWindowRect(h, &rect);
        SetWindowPos(h, nullptr, owner.left + (owner.right - owner.left - (rect.right - rect.left)) / 2,
                     owner.top + (owner.bottom - owner.top - (rect.bottom - rect.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        SendMessageW(h, WM_COMMAND, MAKEWPARAM(100, CBN_SELCHANGE), 0);
        return TRUE;
    }
    if (!data)
        return FALSE;
    if (m == WM_TIMER && w == 209) {
        if (!data->scan || !data->scan->done) return TRUE;
        KillTimer(h, 209);
        const int kind = static_cast<int>(SendMessageW(data->kind, CB_GETCURSEL, 0, 0));
        if (kind != data->scan->kind) return TRUE;
        HWND combo = kind == 0 ? data->resource : data->port;
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        if (kind == 0) {
            data->usb = data->scan->usb;
            for (const auto &s : data->usb) {
                const auto text = Wide(s);
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
        } else for (const auto &text : data->scan->ports)
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        const bool any = SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0;
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(h, IDOK), any);
        EnableWindow(GetDlgItem(h, 103), TRUE);
        SetWindowTextW(data->error, any ? L"" : kind == 0 && !PowerService::VisaAvailable()
            ? L"未安装 VISA，请安装发布包中的 USB 驱动。" : L"未发现设备，请检查连接后刷新。");
        return TRUE;
    }
    if (m == WM_DRAWITEM || m == WM_MEASUREITEM)
        return Proc(data->pane->window_, m, w, l);
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX ||
        m == WM_CTLCOLORBTN) {
        auto c = UiTheme(data->dark);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        const bool field = m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX;
        SetBkColor(reinterpret_cast<HDC>(w), field ? c.field : c.surface);
        return reinterpret_cast<INT_PTR>(field ? data->pane->brush_ : data->pane->surfaceBrush_);
    }
    if (m == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(h, &rect);
        FillRect(reinterpret_cast<HDC>(w), &rect, data->pane->surfaceBrush_);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if (!PowerService::ConnectionAvailable()) {
            if (id == IDOK || id == IDCANCEL)
                UiEndOwnedDialog(h, id);
            return TRUE;
        }
        int kind = static_cast<int>(SendMessageW(data->kind, CB_GETCURSEL, 0, 0));
        if ((id == 100 && HIWORD(w) == CBN_SELCHANGE) || (id == 103 && HIWORD(w) == BN_CLICKED)) {
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
            HWND combo = kind == 0 ? data->resource : data->port;
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"正在查找设备…"));
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(h, IDOK), FALSE);
            EnableWindow(GetDlgItem(h, 103), FALSE);
            auto job = data->scan = std::make_shared<ResourceScan>();
            job->kind = kind;
            data->pane->resourceScanWorkers_[kind == 0 ? 0 : 1].Retire([job] {
                try {
                    if (job->kind == 0) job->usb = PowerService::UsbResources();
                    else for (int index = 1; index <= 256; ++index) {
                        const auto name = L"COM" + std::to_wstring(index);
                        wchar_t target[1024]{};
                        if (QueryDosDeviceW(name.c_str(), target, 1024)) job->ports.push_back(name);
                    }
                } catch (...) { }
                job->done = true;
            });
            SetTimer(h, 209, 40, nullptr);
            return TRUE;
        }
        if (id == 109) {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path(exe);
            path = path.substr(0, path.find_last_of(L"\\")) + L"\\drivers\\ni-visa18\\setup.exe";
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                SetWindowTextW(data->error, L"发布包中的驱动文件缺失，请重新解压完整压缩包。");
            else {
                auto result =
                    reinterpret_cast<INT_PTR>(ShellExecuteW(h, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32)
                    SetWindowTextW(data->error, L"驱动安装程序未能启动，请从发布包目录手动运行 setup.exe。");
            }
            return TRUE;
        }
        if (id == IDCANCEL) {
            UiEndOwnedDialog(h, IDCANCEL);
            return TRUE;
        }
        if (id == IDOK) {
            try {
                if (kind != 0 && kind != 1)
                    throw std::runtime_error("请选择 USB 或 RS232");
                Json command = {{"type", "connect"}, {"backend", kind == 0 ? "usb" : "serial"}};
                if (kind == 0) {
                    auto index = SendMessageW(data->resource, CB_GETCURSEL, 0, 0);
                    if (index < 0 || static_cast<size_t>(index) >= data->usb.size())
                        throw std::runtime_error("请选择 USB 设备");
                    command["resource"] = data->usb[static_cast<size_t>(index)];
                }
                if (kind == 1) {
                    command["port"] = WideToMultiByte(data->pane->Text(data->port), CP_UTF8);
                    command["baud"] = std::stoi(data->pane->Text(data->baud));
                    command["dataBits"] = std::stoi(data->pane->Text(data->bits));
                    command["parity"] = SendMessageW(data->parity, CB_GETCURSEL, 0, 0);
                    command["stopBits"] = SendMessageW(data->stop, CB_GETCURSEL, 0, 0) == 1 ? 2 : 0;
                }
                data->pane->Submit(command);
                UiEndOwnedDialog(h, IDOK);
            } catch (const std::exception &e) {
                SetWindowTextW(data->error, Wide(e.what()).c_str());
            }
            return TRUE;
        }
    }
    return FALSE;
}
std::wstring PowerPane::ChooseFile(bool save, const wchar_t *filter, const wchar_t *extension) {
    wchar_t path[32768]{};
    OPENFILENAMEW file{};
    file.lStructSize = sizeof(file);
    file.hwndOwner = GetAncestor(window_, GA_ROOT);
    file.lpstrFilter = filter;
    file.lpstrFile = path;
    file.nMaxFile = 32768;
    file.lpstrDefExt = extension;
    file.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&file) : GetOpenFileNameW(&file)) ? path : L"";
}
void PowerPane::SetLogExportBusy(bool busy) {
    logExportBusy_ = busy;
    EnableWindow(Control(241), !busy && state_.contains("logPath"));
    EnableWindow(Control(101), !busy && state_.contains("csvPath"));
    EnableWindow(Control(240), !busy && state_.contains("csvPath"));
}
void PowerPane::Export(bool csv) {
    if (logExportBusy_) throw std::runtime_error("已有日志正在导出，可在设置中取消");
    if (!logExportHandler_) throw std::runtime_error("后台日志导出不可用");
    const auto key = csv ? "csvPath" : "logPath";
    if (!state_.contains(key)) throw std::runtime_error("尚无日志文件");
    const auto before = service_->CaptureLogExportPoint(csv);
    if (before.root.empty() || WideToMultiByte(before.root, CP_UTF8) != state_[key].get<std::string>())
        throw std::runtime_error("原电源日志已变化，请刷新后重新导出");
    auto path = ChooseFile(true, csv ? L"CSV 文件\0*.csv\0\0" : L"日志文件\0*.log\0\0", csv ? L"csv" : L"log");
    if (path.empty()) return;
    auto point = service_->CaptureLogExportPoint(csv);
    if (point.generation != before.generation || point.root != before.root)
        throw std::runtime_error("原电源日志已切换，未导出");
    logExportHandler_(csv, path, std::move(point));
}

namespace {
Json ReadPowerFile(const std::wstring &path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        throw std::runtime_error("无法读取方案文件");
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024)
        throw std::runtime_error("方案文件过大");
    file.seekg(0);
    Json result;
    file >> result;
    return result;
}
void WritePowerFile(const std::wstring &path, const Json &value) {
    const auto temporary = path + L".tmp";
    {
        std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("无法保存方案");
        file << value.dump(2);
        file.flush();
        if (!file) {
            DeleteFileW(temporary.c_str());
            throw std::runtime_error("方案写入失败");
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("无法替换方案文件");
    }
}
} // namespace
void PowerPane::TaskFile(bool save) {
    auto path = ChooseFile(save, L"电源任务\0*.json\0\0", L"json");
    if (path.empty())
        return;
    if (save) {
        Json fields = Json::object();
        for (int id : {70, 71, 72, 120, 121, 125, 126, 127, 128})
            fields[std::to_string(id)] = WideToMultiByte(Text(Control(id)), CP_UTF8);
        WritePowerFile(path, {{"format", "SerialCtl.PowerTask.1"},
                              {"view", automation_},
                              {"channels", Selection()},
                              {"fields", fields},
                              {"timerEnabled", SendMessageW(Control(124), CB_GETCURSEL, 0, 0) == 1},
                              {"steps", steps_}});
        SaveTask();
    } else {
        auto file = ReadPowerFile(path);
        if (file.at("format") != "SerialCtl.PowerTask.1" || !file.at("view").is_number_integer() || file["view"] < 0 ||
            file["view"] > 4 || !file.at("steps").is_array() || file["steps"].empty() || file["steps"].size() > 1000 ||
            !file.at("channels").is_array() || file["channels"].size() > 3)
            throw std::runtime_error("任务格式不正确");
        for (auto ch : file["channels"])
            if (!ch.is_number_integer() || ch < 1 || ch > 3)
                throw std::runtime_error("无效任务通道");
        for (int id : {70, 71, 72, 120, 121, 125, 126, 127, 128})
            if (!file.at("fields").at(std::to_string(id)).is_string() ||
                file["fields"][std::to_string(id)].get<std::string>().size() > 128)
                throw std::runtime_error("无效任务字段");
        for (auto step : file["steps"]) {
            if (!step.at("voltage").is_number() || !step.at("current").is_number() ||
                !step.at("durationMs").is_number_integer() || step["durationMs"] < 100 ||
                step["durationMs"] > 604800000 || !step.at("enabled").is_boolean())
                throw std::runtime_error("无效任务步骤");
        }
        bool enabled = file.at("timerEnabled").get<bool>();
        automation_ = file["view"];
        steps_ = file["steps"];
        for (int id : {70, 71, 72, 120, 121, 125, 126, 127, 128})
            SetWindowTextW(Control(id), Wide(file["fields"][std::to_string(id)]).c_str());
        for (int i = 0; i < 3; ++i)
            SendMessageW(check_[i], BM_SETCHECK,
                         std::find(file["channels"].begin(), file["channels"].end(), Json(i + 1)) !=
                                 file["channels"].end()
                             ? BST_CHECKED
                             : BST_UNCHECKED,
                         0);
        SendMessageW(Control(124), CB_SETCURSEL, enabled ? 1 : 0, 0);
        Layout();
    }
}
void PowerPane::Profile(bool save) {
    auto path = ChooseFile(save, L"电源方案\0*.json\0\0", L"json");
    if (path.empty())
        return;
    if (save) {
        Json settings = Json::array();
        bool group = draftMode_ != 0;
        for (int i = 0; i < 3; ++i) {
            if (group && i == 1)
                continue;
            settings.push_back({{"channels", group && i == 0 ? Json::array({1, 2}) : Json::array({i + 1})},
                                {"voltage", Parse(Text(voltage_[i]))},
                                {"current", Parse(Text(current_[i]))}});
        }
        WritePowerFile(path, {{"format", "SerialCtl.PowerProfile.1"}, {"mode", Mode()}, {"settings", settings}});
    } else {
        auto file = ReadPowerFile(path);
        if (file.at("format") != "SerialCtl.PowerProfile.1" || file.at("mode") != Mode() ||
            !file.at("settings").is_array() || file["settings"].empty() || file["settings"].size() > 3)
            throw std::runtime_error("方案格式或工作模式不匹配，请先选择方案对应模式");
        if (!state_.value("connected", false))
            throw std::runtime_error("请先连接电源");
        Submit({{"type", "batch"}, {"mode", file["mode"]}, {"settings", file["settings"]}});
    }
}
namespace {
Json ParsePowerSteps(const std::wstring &value) {
    Json steps = Json::array();
    std::wistringstream input(value);
    std::wstring line;
    while (std::getline(input, line)) {
        if (line.find_first_not_of(L" \t\r") == std::wstring::npos)
            continue;
        std::replace(line.begin(), line.end(), L',', L' ');
        std::wistringstream row(line);
        row.imbue(std::locale::classic());
        double v = 0, a = 0, seconds = 0;
        int on = 0;
        row >> v >> a >> seconds >> on;
        if (row.fail())
            throw std::runtime_error("步骤格式不正确");
        row >> std::ws;
        if (!row.eof() || !std::isfinite(v) || !std::isfinite(a) || !std::isfinite(seconds) || v < 0 || v > 60 ||
            a < 0 || a > 12 || seconds < 0.1 || seconds > 604800 || (on != 0 && on != 1) || steps.size() >= 1000)
            throw std::runtime_error("步骤数值超出范围");
        steps.push_back({{"voltage", v},
                         {"current", a},
                         {"durationMs", static_cast<long long>(std::llround(seconds * 1000))},
                         {"enabled", on == 1}});
    }
    if (steps.empty())
        throw std::runtime_error("至少需要一个步骤");
    return steps;
}
struct PowerTextDialog {
    PowerPane *pane;
    std::wstring title, label, value;
    bool multiline;
    HWND edit = nullptr;
};
} // namespace
void PowerPane::EditSequence() {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    for (auto step : steps_)
        text << step["voltage"].get<double>() << L", " << step["current"].get<double>() << L", "
             << step["durationMs"].get<double>() / 1000 << L", " << (step["enabled"].get<bool>() ? 1 : 0) << L"\r\n";
    PowerTextDialog data{this, L"编辑电源步骤", L"每行：电压(V), 限流(A), 保持(秒), 输出(1/0)", text.str(), true};
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    } t{};
    t.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dialog.cx = 238;
    t.dialog.cy = 190;
    if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &t.dialog, GetAncestor(window_, GA_ROOT), TextProc,
                                reinterpret_cast<LPARAM>(&data)) != IDOK)
        return;
    steps_ = ParsePowerSteps(data.value);
}
void PowerPane::ShowScpi() {
    PowerTextDialog data{this, L"本机 SCPI 调试", L"单条指令（调试前后关闭三路输出）", L"*IDN?", false};
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, cls = 0, title = 0;
    } t{};
    t.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dialog.cx = 238;
    t.dialog.cy = 90;
    if (UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr), &t.dialog, GetAncestor(window_, GA_ROOT), TextProc,
                                reinterpret_cast<LPARAM>(&data)) == IDOK)
        Submit({{"type", "diagnostic"}, {"operation", "scpi"}, {"line", WideToMultiByte(data.value, CP_UTF8)}});
}
INT_PTR CALLBACK PowerPane::TextProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto *data = reinterpret_cast<PowerTextDialog *>(GetWindowLongPtrW(h, DWLP_USER));
    if (m == WM_INITDIALOG) {
        data = reinterpret_cast<PowerTextDialog *>(l);
        SetWindowLongPtrW(h, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(h, data->title.c_str());
        auto *pane = data->pane;
        auto d = [pane](int n) { return MulDiv(n, pane->dpi_, 96); };
        int height = data->multiline ? 316 : 140;
        UiResizeDialog(h, pane->dpi_, 436, height);
        auto add = [&](const wchar_t *cls, const wchar_t *text, int id, DWORD style, int x, int y, int width,
                       int size) {
            auto control =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, d(x), d(y), d(width), d(size), h,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(pane->font_), TRUE);
            return control;
        };
        auto label = add(L"STATIC", data->label.c_str(), -1, 0, 16, 12, 404, 28);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(pane->smallFont_), TRUE);
        data->edit = add(L"EDIT", data->value.c_str(), 100,
                         WS_TABSTOP | ES_AUTOHSCROLL |
                             (data->multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL) : 0),
                         16, 44, 404, data->multiline ? 204 : 28);
        UiStyleField(data->edit, &pane->fieldStyle_);
        SendMessageW(data->edit, EM_SETLIMITTEXT, data->multiline ? 65536 : 256, 0);
        auto cancel = add(L"BUTTON", L"取消", IDCANCEL, WS_TABSTOP | BS_OWNERDRAW, 216, height - 52, 96, 36);
        auto ok = add(L"BUTTON", L"确定", IDOK, WS_TABSTOP | BS_OWNERDRAW, 324, height - 52, 96, 36);
        UiStyleButton(cancel);
        UiStyleButton(ok);
        RECT owner{}, bounds{};
        GetWindowRect(GetAncestor(pane->window_, GA_ROOT), &owner);
        GetWindowRect(h, &bounds);
        SetWindowPos(h, nullptr, owner.left + (owner.right - owner.left - (bounds.right - bounds.left)) / 2,
                     owner.top + (owner.bottom - owner.top - (bounds.bottom - bounds.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }
    if (!data)
        return FALSE;
    auto *pane = data->pane;
    auto c = UiTheme(pane->dark_);
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) {
            data->value = pane->Text(data->edit);
            if (data->multiline)
                try {
                    ParsePowerSteps(data->value);
                } catch (const std::exception &e) {
                    MessageBoxW(h, Wide(e.what()).c_str(), L"编辑电源步骤", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            UiEndOwnedDialog(h, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) {
            UiEndOwnedDialog(h, IDCANCEL);
            return TRUE;
        }
    }
    if (m == WM_CLOSE) {
        UiEndOwnedDialog(h, IDCANCEL);
        return TRUE;
    }
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT) {
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        SetBkColor(reinterpret_cast<HDC>(w), m == WM_CTLCOLOREDIT ? c.field : c.surface);
        return reinterpret_cast<INT_PTR>(m == WM_CTLCOLOREDIT ? pane->brush_ : pane->surfaceBrush_);
    }
    if (m == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(h, &rect);
        FillRect(reinterpret_cast<HDC>(w), &rect, pane->surfaceBrush_);
        return TRUE;
    }
    if (m == WM_DRAWITEM) {
        auto item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        UiBufferedPaint paint(item.hDC, item.rcItem);
        item.hDC = paint.Dc();
        FillRect(item.hDC, &item.rcItem, pane->surfaceBrush_);
        const bool enabled = !(item.itemState & ODS_DISABLED);
        const bool primary = item.CtlID == IDOK && enabled;
        const auto visual = UiButtonVisual(c, enabled, item.itemState & ODS_SELECTED,
                                           GetPropW(item.hwndItem, L"SerialCtl.PowerHover") != nullptr, primary);
        UiBox(item.hDC, item.rcItem, visual.fill, visual.border, UiCapsuleRadius(item.rcItem));
        UiButtonKeyboardFocus(item.hDC, item.rcItem, item.itemState, pane->dpi_,
            UiFocusInk(pane->dark_, enabled && (primary || (item.itemState & ODS_SELECTED))));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, visual.ink);
        auto old = SelectObject(item.hDC, pane->font_);
        auto text = pane->Text(item.hwndItem);
        DrawTextW(item.hDC, text.c_str(), -1, &item.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old);
        return TRUE;
    }
    return FALSE;
}
} // namespace serialctl
