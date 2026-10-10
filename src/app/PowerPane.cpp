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
struct ConnectionDialog {
    PowerPane *pane;
    HFONT font;
    bool dark;
    PowerService *service;
    HWND kind, resource, port, baud, bits, parity, stop, error, driver;
    Json usb;
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
    auto c = UiTheme(dark);
    brush_ = CreateSolidBrush(c.field);
    fieldBrush_ = CreateSolidBrush(c.raised);
    surfaceBrush_ = CreateSolidBrush(c.surface);
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
            Submit({{"type", "disconnect"}});
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
    EnableWindow(Control(241), state_.contains("logPath"));
    EnableWindow(Control(101), state_.contains("csvPath"));
    EnableWindow(Control(240), state_.contains("csvPath"));
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
    HWND h = Child(L"COMBOBOX", L"", id, CBS_DROPDOWNLIST | WS_VSCROLL);
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
    UiLayoutBatch batch;currentLayout_=&batch;
    RECT r{};
    GetClientRect(window_, &r);
    auto d = [this](int v) { return MulDiv(v, dpi_, 96); };
    int w = r.right;
    cardColumns_ = w >= d(600) ? 3 : 1;
    cardTop_ = d(104);
    cardHeight_ = d(216);
    bool group = draftMode_ == 1 || draftMode_ == 2;
    int rows = cardColumns_ == 3 ? 1 : group ? 2 : 3;
    taskTop_ = cardTop_ + rows * (cardHeight_ + d(12)) + d(4);
    detailsTop_ = taskTop_ + d(48);
    contentHeight_ = detailsTop_ + d(201);
    if (contentHeight_ > r.bottom)
        w -= d(16);
    cardWidth_ = (w - d(12) * (cardColumns_ - 1)) / cardColumns_;
    auto move = [&](HWND h, int x, int y, int width, int height) {
        batch.Move(h, x, y - scroll_, width, height);
        batch.Show(h, SW_SHOW);
    };
    move(buttons_[0], w - d(200), 0, d(96), d(36));
    move(buttons_[1], w - d(96), 0, d(96), d(36));
    move(buttons_[2], w - d(200), d(52), d(96), d(36));
    move(buttons_[3], w - d(96), d(52), d(96), d(36));
    move(Control(81), w - d(312), d(52), d(104), d(36));
    SendMessageW(Control(80), CB_SETITEMHEIGHT, -1, d(30));
    move(Control(80), d(72), d(52), d(160), d(180));
    SendMessageW(Control(80), CB_SETITEMHEIGHT, -1, d(30));
    for (int i = 0; i < 3; ++i) {
        bool hidden = group && i == 1;
        int index = group && i == 2 ? 1 : i;
        int column = i % cardColumns_;
        int x = column * (w + d(12)) / cardColumns_, y = cardTop_ + (index / cardColumns_) * (cardHeight_ + d(12)),
            cw = (column + 1) * (w + d(12)) / cardColumns_ - x - d(12);
        if (group && cardColumns_ == 3) {
            if (i == 0)
                cw = 2 * (w + d(12)) / 3 - d(12);
            if (i == 2)
                x = 2 * (w + d(12)) / 3;
        }
        SetWindowTextW(check_[i], group && i == 0 ? L"CH1＋CH2" : (L"CH" + std::to_wstring(i + 1)).c_str());
        move(check_[i], x + d(16), y + d(12), d(group && i == 0 ? 140 : 72), d(28));
        int label = d(group && i == 0 ? 124 : 100);
        move(voltage_[i], x + label, y + d(136), cw - label - d(16), d(28));
        move(current_[i], x + label, y + d(172), cw - label - d(16), d(28));
        if (hidden) {
            batch.Show(check_[i], SW_HIDE);
            batch.Show(voltage_[i], SW_HIDE);
            batch.Show(current_[i], SW_HIDE);
        }
    }
    const int widths[] = {92, 92, 92, 104};
    int x = 0;
    for (int i = 0; i < 4; ++i) {
        move(Control(90 + i), x + d(2), taskTop_ + d(2), d(widths[i]), d(28));
        x += d(widths[i]);
    }
    for (auto item : controls_)
        if (item.first >= 100 || (item.first >= 40 && item.first <= 44) || (item.first >= 70 && item.first <= 72))
            batch.Show(item.second, SW_HIDE);
    DetailsLayout(w, detailsTop_);
    currentLayout_=nullptr;batch.Commit();
}
void PowerPane::DetailsLayout(int width, int top) {
    auto d = [this](int v) { return MulDiv(v, dpi_, 96); };
    auto move = [&](int id, int x, int y, int w, int h = 28) {
        HWND control = Control(id);
        wchar_t cls[24]{};
        GetClassNameW(control, cls, 24);
        if (_wcsicmp(cls, L"COMBOBOX") == 0) {
            SendMessageW(control, CB_SETITEMHEIGHT, -1, d(h - 6));
            h = 180;
        }
        currentLayout_->Move(control, d(x), top + d(y) - scroll_, d(w), d(h));
        currentLayout_->Show(control, SW_SHOW);
    };
    const int w = MulDiv(width, 96, dpi_);
    if (tab_ == 0) {
        move(103, w - 248, 0, 72);
        move(100, w - 168, 0, 78);
        move(101, w - 82, 0, 82);
    }
    if (tab_ == 1) {
        int x = 0;
        for (int i = 0; i < 5; ++i) {
            int size = i == 3 || i == 4 ? 65 : 52;
            move(110 + i, x, 0, size);
            x += size + 12;
        }
        int field = (w - 24) / 3;
        const int ids[5][3] = {{70, 71, 72}, {120, 121, 122}, {123, 0, 0}, {124, 125, 0}, {126, 127, 128}};
        for (int i = 0; i < 3; ++i)
            if (ids[automation_][i])
                move(ids[automation_][i], i * (field + 12), 71, automation_ == 2 ? w : field);
        const int wide = (w - 184) / 2;
        move(Start, 0, 115, wide, 36);
        move(Stop, wide + 8, 115, wide, 36);
        move(Save, w - 168, 115, 80, 36);
        move(44, w - 80, 115, 80, 36);
        SetWindowTextW(start_, automation_ == 3 ? L"应用定时器" : automation_ == 4 ? L"应用并触发" : L"开始测试");
    }
    if (tab_ == 1 && automation_ == 3)
        move(133, w - 80, 71, 80);
    if (tab_ == 1 && automation_ == 4) {
        for (int i = 0; i < 4; ++i)
            move(134 + i, i * 112, 156, 104);
        contentHeight_ = top + d(240);
    }
    if (tab_ == 2) {
        for (int i = 0; i < 4; ++i)
            move(190 + i, 0, i * 36, 104, 32);
        const int x = 132, cw = w - x, half = (cw - 12) / 2;
        if (settings_ == 0) {
            move(200, x + 38, 0, 96);
            move(201, w - 100, 0, 100);
            for (int i = 0; i < 4; ++i)
                move(202 + i, x + (i % 2) * (half + 12), 67 + (i / 2) * 67, half);
            move(206, w - 256, 174, 80);
            move(207, w - 168, 174, 80);
            move(208, w - 80, 174, 80);
        }
        if (settings_ == 1) {
            move(210, x + 100, 12, cw - 100);
            move(211, x + 100, 52, cw - 100);
            move(212, x, 116, half);
            move(213, x + half + 12, 116, half);
            move(214, w - 168, 172, 80);
            move(215, w - 80, 172, 80);
        }
        if (settings_ == 2) {
            move(220, x + 100, 12, cw - 100);
            move(221, x + 100, 52, cw - 100);
            move(222, w - 232, 92, 112);
            move(223, w - 112, 92, 112);
            move(224, w - 232, 132, 112);
            move(225, w - 112, 132, 112);
        }
        if (settings_ == 3) {
            move(230, x + 100, 12, cw - 100);
            move(231, x, 52, 112);
            move(232, x + 124, 52, 112);
            move(233, x + 248, 52, 80);
            move(234, x + 100, 92, cw - 100);
            move(235, w - 256, 132, 80);
            move(236, w - 168, 132, 80);
            move(237, w - 80, 132, 80);
        }
    }
    if (tab_ == 3) {
        move(240, w - 200, 0, 96);
        move(241, w - 96, 0, 96);
        int x = 0;
        for (int i = 0; i < 6; ++i) {
            int size = i == 4 ? 104 : 88;
            if (x + size > w) {
                x = 0;
                top += d(36);
            }
            move(242 + i, x, 156, size);
            x += size + 8;
        }
        contentHeight_ = std::max(contentHeight_, top + d(192));
    }
}
void PowerPane::Paint(HDC dc) {
    RECT r{};
    GetClientRect(window_, &r);
    auto c = UiTheme(dark_);
    FillRect(dc, &r, brush_);
    SetViewportOrgEx(dc, 0, -scroll_, nullptr);
    auto d = [this](int v) { return MulDiv(v, dpi_, 96); };
    int width = r.right - (contentHeight_ > r.bottom ? d(16) : 0);
    auto text = [&](const std::wstring &s, RECT rect, COLORREF color, HFONT font = nullptr,
                    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        auto old = SelectObject(dc, font ? font : smallFont_);
        DrawTextW(dc, s.c_str(), -1, &rect, flags);
        SelectObject(dc, old);
    };
    text(L"IT6332A", {0, 0, d(96), d(36)}, c.text, titleFont_);
    text(state_.value("connected", false) ? L"已连接" : L"未连接", {d(104), 0, width - d(216), d(36)}, c.muted);
    text(L"工作模式", {0, d(52), d(72), d(88)}, c.text, font_);
    bool group = draftMode_ == 1 || draftMode_ == 2;
    for (int i = 0; i < 3; ++i) {
        if (group && i == 1)
            continue;
        int index = group && i == 2 ? 1 : i;
        int column = i % cardColumns_;
        int x = column * (width + d(12)) / cardColumns_, y = cardTop_ + (index / cardColumns_) * (cardHeight_ + d(12)),
            cw = (column + 1) * (width + d(12)) / cardColumns_ - x - d(12);
        if (group && cardColumns_ == 3) {
            if (i == 0)
                cw = 2 * (width + d(12)) / 3 - d(12);
            if (i == 2)
                x = 2 * (width + d(12)) / 3;
        }
        bool checked = SendMessageW(check_[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
        UiBox(dc, {x, y, x + cw, y + cardHeight_}, c.surface, checked ? c.text : c.border, d(12));
        if (!state_.contains("channels"))
            continue;
        auto channel = state_["channels"][i];
        bool combined = group && i == 0;
        if (combined) {
            auto other = state_["channels"][1];
            for (auto key : {"voltage", "current", "power"})
                if (channel[key].is_number() && other[key].is_number()) {
                    if (std::string(key) == "power" || (draftMode_ == 1 && std::string(key) == "current") ||
                        (draftMode_ == 2 && std::string(key) == "voltage"))
                        channel[key] = channel[key].get<double>() + other[key].get<double>();
                } else
                    channel[key] = nullptr;
            if (other["output"] != channel["output"])
                channel["output"] = nullptr;
        }
        std::wstring status = channel["output"].is_boolean() ? (channel["output"].get<bool>() ? L"已加电" : L"已掉电")
                              : state_.value("connected", false) ? L"状态未知"
                                                                 : L"未连接";
        text(status, {x + d(combined ? 156 : 88), y + d(12), x + cw - d(16), y + d(40)}, c.muted, nullptr,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int half = (cw - d(32)) / 2;
        text(combined && draftMode_ == 2 ? L"总电压 (V)" : L"电压 (V)",
             {x + d(16), y + d(52), x + d(16) + half, y + d(72)}, c.muted);
        text(combined && draftMode_ == 1 ? L"总电流 (A)" : L"电流 (A)",
             {x + d(16) + half, y + d(52), x + cw - d(16), y + d(72)}, c.muted);
        text(Value(channel["voltage"]), {x + d(16), y + d(76), x + d(16) + half, y + d(104)}, c.text, valueFont_);
        text(Value(channel["current"]), {x + d(16) + half, y + d(76), x + cw - d(16), y + d(104)}, c.text, valueFont_);
        text(L"功率 " + Value(channel["power"], L" W"), {x + d(16), y + d(108), x + cw - d(16), y + d(128)}, c.muted);
        if (combined)
            text(L"CH1 " + Value(state_["channels"][0]["voltage"]) + L"　CH2 " +
                     Value(state_["channels"][1]["voltage"]),
                 {x + d(172), y + d(108), x + cw - d(16), y + d(128)}, c.muted, nullptr,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        text(combined && draftMode_ == 2 ? L"总电压 (V)" : L"设定电压 (V)",
             {x + d(16), y + d(136), x + d(combined ? 116 : 92), y + d(164)}, c.muted);
        text(combined && draftMode_ == 1 ? L"总电流限值 (A)" : L"电流限值 (A)",
             {x + d(16), y + d(172), x + d(combined ? 116 : 92), y + d(200)}, c.muted);
    }
    UiBox(dc, {0, taskTop_, d(384), taskTop_ + d(32)}, c.raised, c.border, d(10));
    std::wstring selected = L"已选";
    auto selection = Selection();
    for (auto ch : selection)
        selected += L" CH" + std::to_wstring(ch.get<int>());
    if (selection.empty())
        selected = L"未选择通道";
    text(selected, {d(396), taskTop_, width, taskTop_ + d(32)}, c.muted, nullptr,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    PaintDetails(dc, width, detailsTop_);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    if (contentHeight_ > r.bottom) {
        int thumb = std::max(d(32), static_cast<int>(r.bottom * r.bottom) / std::max(1, contentHeight_));
        int top = scroll_ * (r.bottom - thumb) / std::max(1, contentHeight_ - static_cast<int>(r.bottom));
        UiBox(dc, {r.right - d(8), top, r.right - d(3), top + thumb}, c.border, c.border, d(3));
    }
}
void PowerPane::PaintDetails(HDC dc, int width, int top) {
    auto d = [this](int v) { return MulDiv(v, dpi_, 96); };
    auto c = UiTheme(dark_);
    const int w = MulDiv(width, 96, dpi_);
    auto text = [&](const std::wstring &s, int x, int y, int size, int height = 28, COLORREF color = CLR_INVALID) {
        RECT rect{d(x), top + d(y), d(x + size), top + d(y + height)};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color == CLR_INVALID ? c.muted : color);
        auto old = SelectObject(dc, smallFont_);
        DrawTextW(dc, s.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    };
    if (tab_ == 0) {
        text(L"实时趋势", 0, 0, 160);
        text(L"采样", w - 284, 0, 36);
        int pw = (w - 12) / 2;
        for (int metric = 0; metric < 2; ++metric) {
            int x = metric * (pw + 12);
            RECT box{d(x), top + d(40), d(x + pw), top + d(188)};
            UiBox(dc, box, c.surface, c.border, d(12));
            text(metric ? L"电流 (A)" : L"电压 (V)", x + 12, 52, pw - 24);
            int left = d(x + 12), right = d(x + pw - 12), bottom = top + d(160), plotTop = top + d(80);
            COLORREF gridColor =
                RGB((GetRValue(c.border) + GetRValue(c.surface)) / 2, (GetGValue(c.border) + GetGValue(c.surface)) / 2,
                    (GetBValue(c.border) + GetBValue(c.surface)) / 2);
            HPEN grid = CreatePen(PS_SOLID, 1, gridColor);
            auto old = SelectObject(dc, grid);
            for (int i = 0; i <= 4; ++i) {
                int y = plotTop + (bottom - plotTop) * i / 4;
                MoveToEx(dc, left, y, nullptr);
                LineTo(dc, right, y);
            }
            MoveToEx(dc, left, plotTop, nullptr);
            LineTo(dc, left, bottom);
            SelectObject(dc, old);
            DeleteObject(grid);
            if (history_.size() < 2)
                continue;
            double max = 0.001;
            for (const auto &sample : history_)
                for (const auto &ch : sample)
                    if (ch[metric ? "current" : "voltage"].is_number())
                        max = std::max(max, ch[metric ? "current" : "voltage"].get<double>());
            for (int channel = 0; channel < 3; ++channel) {
                COLORREF ink = channel == 0 ? c.accent : channel == 1 ? c.text : c.muted;
                HPEN pen = CreatePen(channel == 2 ? PS_DOT : PS_SOLID, 1, ink);
                auto previous = SelectObject(dc, pen);
                bool begun = false;
                int i = 0;
                for (const auto &sample : history_) {
                    auto value = sample[channel][metric ? "current" : "voltage"];
                    if (value.is_number()) {
                        int px = left + static_cast<int>((right - left) * i / (history_.size() - 1)),
                            py = bottom -
                                 static_cast<int>((bottom - plotTop) * std::clamp(value.get<double>() / max, 0.0, 1.0));
                        if (begun)
                            LineTo(dc, px, py);
                        else
                            MoveToEx(dc, px, py, nullptr);
                        begun = true;
                    } else
                        begun = false;
                    ++i;
                }
                SelectObject(dc, previous);
                DeleteObject(pen);
            }
        }
    }
    if (tab_ == 1) {
        static const wchar_t *labels[5][3] = {{L"加电时长 (秒)", L"掉电时长 (秒)", L"循环次数"},
                                              {L"开始时间", L"掉电时间", L"重复"},
                                              {L"步骤序列", L"", L""},
                                              {L"设备输出定时器", L"掉电延时 (秒)", L"设备状态"},
                                              {L"触发延时 (秒)", L"电压步进 (V)", L"电流步进 (A)"}};
        int field = (w - 24) / 3;
        for (int i = 0; i < 3; ++i)
            text(labels[automation_][i], i * (field + 12), 44, field, 20);
        if (automation_ == 2)
            text(std::to_wstring(steps_.size()) + L" 个步骤", w - 160, 40, 160, 20);
        if (automation_ == 3) {
            const auto timer = state_.value("timer", Json::object());
            text(timer.contains("enabled") ? (timer["enabled"].get<bool>() ? L"开启" : L"关闭") : L"—",
                 2 * (field + 12), 64, field);
        }
        auto task = state_.value("task", Json::object());
        std::wstring status =
            task.value("running", false) ? L"运行中 · " + std::to_wstring(task.value("completed", 0)) : L"未运行";
        if (state_.contains("error"))
            status = Wide(state_["error"]);
        text(status, 0, automation_ == 4 ? 200 : 163, w, 28, state_.contains("error") ? c.danger : c.muted);
    }
    if (tab_ == 2) {
        int x = 132, cw = w - x, half = (cw - 12) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, c.border);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, d(116), top, nullptr);
        LineTo(dc, d(116), top + d(201));
        SelectObject(dc, old);
        DeleteObject(pen);
        if (settings_ == 0) {
            text(L"通道", x, 0, 36);
            const wchar_t *labels[] = {L"电压设置上限 (V)", L"设备过压阈值 (V)", L"软件电压阈值 (V)",
                                       L"软件电流阈值 (A)"};
            for (int i = 0; i < 4; ++i)
                text(labels[i], x + (i % 2) * (half + 12), 40 + (i / 2) * 67, half, 20);
            int ch = static_cast<int>(SendMessageW(Control(200), CB_GETCURSEL, 0, 0));
            auto p = state_["channels"][std::clamp(ch, 0, 2)].value("deviceProtection", Json::object());
            text(p.contains("tripped") ? (p["tripped"].get<bool>() ? L"保护状态 已触发" : L"保护状态 正常")
                                       : L"保护状态 —",
                 x, 172, std::max(0, cw - 264));
        }
        if (settings_ == 1) {
            text(L"组合模式", x, 12, 88);
            text(L"关联通道", x, 52, 88);
            text(L"电压跟踪比例", x, 88, half, 20);
            text(L"电流跟踪比例", x + half + 12, 88, half, 20);
        }
        if (settings_ == 2) {
            text(L"设备存储位置", x, 12, 100);
            text(L"本地方案", x, 52, 88);
        }
        if (settings_ == 3) {
            text(L"控制方式", x, 12, 88);
            text(L"屏幕文字", x, 92, 88);
        }
    }
    if (tab_ == 3) {
        text(L"操作记录", 0, 0, 160);
        const int positions[] = {0, 170, 250, 360};
        for (int i = 0; i < 4; ++i)
            text(std::vector<std::wstring>{L"时间", L"来源", L"操作", L"结果"}[i], positions[i], 40, w - positions[i],
                 28);
        auto records = state_.value("records", Json::array());
        int start = std::max(0, static_cast<int>(records.size()) - 3);
        for (int i = start; i < static_cast<int>(records.size()); ++i) {
            auto record = records[i];
            int y = 72 + (i - start) * 24;
            text(Wide(record.value("timestamp", std::string())), 0, y, 164, 24);
            text(Wide(record.value("source", std::string())), 170, y, 72, 24);
            text(Wide(record.value("operation", std::string())), 250, y, 104, 24);
            text(Wide(record.value("result", std::string())), 360, y, w - 360, 24);
        }
    }
}
void PowerPane::Scroll(int position) {
    RECT r{};
    GetClientRect(window_, &r);
    Layout();
    scroll_ = std::clamp(position, 0, std::max(0, contentHeight_ - static_cast<int>(r.bottom)));
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
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
    if (m == WM_CREATE) {
        self->Theme(self->dark_);
        self->CreateControls();
        self->Refresh();
        SetTimer(h, 1, 500, nullptr);
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        self->Scroll(self->scroll_ - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * MulDiv(48, self->dpi_, 96));
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        RECT r{};
        GetClientRect(h, &r);
        if (GET_X_LPARAM(l) >= r.right - MulDiv(16, self->dpi_, 96) && self->contentHeight_ > r.bottom) {
            self->scrolling_ = true;
            SetCapture(h);
            self->Scroll(GET_Y_LPARAM(l) * std::max(0, self->contentHeight_ - static_cast<int>(r.bottom)) /
                         std::max(1, static_cast<int>(r.bottom)));
            return 0;
        }
    }
    if (m == WM_MOUSEMOVE && self->scrolling_) {
        RECT r{};
        GetClientRect(h, &r);
        self->Scroll(GET_Y_LPARAM(l) * std::max(0, self->contentHeight_ - static_cast<int>(r.bottom)) /
                     std::max(1, static_cast<int>(r.bottom)));
        return 0;
    }
    if (m == WM_LBUTTONUP || m == WM_CAPTURECHANGED) {
        self->scrolling_ = false;
        if (GetCapture() == h)
            ReleaseCapture();
    }
    if (m == WM_SIZE) {
        self->Scroll(self->scroll_);
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, 1);
        return 0;
    }
    if (m == WM_TIMER) {
        HWND focus = GetFocus();
        if (focus && IsChild(h, focus) && IsWindowVisible(h)) {
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
    if (m == WM_COMMAND && (HIWORD(w) == BN_CLICKED || HIWORD(w) == CBN_SELCHANGE)) {
        self->Action(LOWORD(w));
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
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
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        auto c = UiTheme(self->dark_);
        bool primary = item.CtlID == On || item.CtlID == Connect || item.CtlID == Start;
        bool mainTab = item.CtlID >= 90 && item.CtlID <= 93, subTab = item.CtlID >= 110 && item.CtlID <= 114,
             groupTab = item.CtlID >= 190 && item.CtlID <= 193;
        bool active = mainTab    ? item.CtlID == 90 + self->tab_
                      : subTab   ? item.CtlID == 110 + self->automation_
                      : groupTab ? item.CtlID == 190 + self->settings_
                                 : false;
        if (mainTab || subTab || groupTab) {
            HBRUSH background = CreateSolidBrush(mainTab ? c.raised : c.field);
            FillRect(item.hDC, &item.rcItem, background);
            DeleteObject(background);
            if (mainTab || groupTab)
                UiBox(item.hDC, item.rcItem, active ? (mainTab ? c.surface : c.raised) : (mainTab ? c.raised : c.field),
                      active && mainTab ? c.border : (mainTab ? c.raised : c.field), MulDiv(10, self->dpi_, 96));
            else if (active) {
                RECT underline = item.rcItem;
                underline.top = underline.bottom - MulDiv(2, self->dpi_, 96);
                HBRUSH accent = CreateSolidBrush(c.accent);
                FillRect(item.hDC, &underline, accent);
                DeleteObject(accent);
            }
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, active ? c.accent : c.muted);
            auto old = SelectObject(item.hDC, self->smallFont_);
            auto label = self->Text(item.hwndItem);
            RECT rect = item.rcItem;
            DrawTextW(item.hDC, label.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item.hDC, old);
            return TRUE;
        }
        HBRUSH outside = CreateSolidBrush(c.field);
        FillRect(item.hDC, &item.rcItem, outside);
        DeleteObject(outside);
        UiBox(item.hDC, item.rcItem,
              (item.itemState & ODS_SELECTED)                                                        ? c.border
              : primary && !(item.itemState & ODS_DISABLED)                                          ? c.accent
              : GetPropW(item.hwndItem, L"SerialCtl.PowerHover") && !(item.itemState & ODS_DISABLED) ? c.surface
                                                                                                     : c.raised,
              (item.itemState & ODS_FOCUS) ? c.accent : c.border, MulDiv(10, self->dpi_, 96));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, (item.itemState & ODS_DISABLED)           ? c.muted
                               : item.CtlID == Off || item.CtlID == Stop ? c.danger
                               : primary                                 ? RGB(255, 255, 255)
                                                                         : c.text);
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
            auto c =
                CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, d(x), d(y), d(width), d(height), h,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
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
    if (m == WM_DRAWITEM)
        return Proc(data->pane->window_, m, w, l);
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX ||
        m == WM_CTLCOLORBTN) {
        auto c = UiTheme(data->dark);
        SetTextColor(reinterpret_cast<HDC>(w), c.text);
        SetBkColor(reinterpret_cast<HDC>(w), c.field);
        return reinterpret_cast<INT_PTR>(data->pane->brush_);
    }
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        int kind = static_cast<int>(SendMessageW(data->kind, CB_GETCURSEL, 0, 0));
        if (id == 100 || id == 103) {
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
            if (kind == 0) {
                SendMessageW(data->resource, CB_RESETCONTENT, 0, 0);
                data->usb = PowerService::UsbResources();
                for (const auto &s : data->usb) {
                    auto text = Wide(s);
                    SendMessageW(data->resource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
                }
                SendMessageW(data->resource, CB_SETCURSEL, 0, 0);
                SetWindowTextW(data->error, !PowerService::VisaAvailable() ? L"未安装 VISA，请安装发布包中的 USB 驱动。"
                                            : data->usb.empty()            ? L"未发现 USB 电源，请检查连接后刷新。"
                                                                           : L"");
            } else {
                if (kind == 1) {
                    auto previous = data->pane->Text(data->port);
                    SendMessageW(data->port, CB_RESETCONTENT, 0, 0);
                    wchar_t target[1024]{};
                    for (int index = 1; index <= 256; ++index) {
                        std::wstring name = L"COM" + std::to_wstring(index);
                        if (QueryDosDeviceW(name.c_str(), target, 1024))
                            SendMessageW(data->port, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                    }
                    if (previous.empty())
                        SendMessageW(data->port, CB_SETCURSEL, 0, 0);
                    else {
                        auto index = SendMessageW(data->port, CB_FINDSTRINGEXACT, -1,
                                                  reinterpret_cast<LPARAM>(previous.c_str()));
                        SendMessageW(data->port, CB_SETCURSEL, index == CB_ERR ? 0 : index, 0);
                    }
                }
                SetWindowTextW(data->error, L"");
            }
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
void PowerPane::Export(bool csv) {
    const auto key = csv ? "csvPath" : "logPath";
    if (!state_.contains(key))
        throw std::runtime_error("尚无日志文件");
    auto path = ChooseFile(true, csv ? L"CSV 文件\0*.csv\0\0" : L"日志文件\0*.log\0\0", csv ? L"csv" : L"log");
    if (path.empty())
        return;
    std::wstring error;
    if (!service_->ExportLog(csv, path, error))
        throw std::runtime_error(WideToMultiByte(error, CP_UTF8));
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
        auto cancel = add(L"BUTTON", L"取消", IDCANCEL, WS_TABSTOP | BS_OWNERDRAW, 220, height - 52, 96, 36);
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
        return reinterpret_cast<INT_PTR>(pane->surfaceBrush_);
    }
    if (m == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(h, &rect);
        FillRect(reinterpret_cast<HDC>(w), &rect, pane->surfaceBrush_);
        return TRUE;
    }
    if (m == WM_DRAWITEM) {
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        UiBox(item.hDC, item.rcItem, item.CtlID == IDOK ? c.accent : c.raised,
              item.itemState & ODS_FOCUS ? c.accent : c.border, MulDiv(10, pane->dpi_, 96));
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, item.CtlID == IDOK ? RGB(255, 255, 255) : c.text);
        auto old = SelectObject(item.hDC, pane->font_);
        auto text = pane->Text(item.hwndItem);
        DrawTextW(item.hDC, text.c_str(), -1, &item.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, old);
        return TRUE;
    }
    return FALSE;
}
} // namespace serialctl
