#pragma once
#include "PowerService.h"
#include "UiStyle.h"
#include <array>
#include <deque>
#include <functional>
#include <map>
namespace serialctl {
inline constexpr UINT PowerStateChanged = WM_APP + 9;
class PowerPane {
  public:
    ~PowerPane();
    bool Create(HWND parent, HFONT font, bool dark, PowerService *service);
    HWND Handle() const {
        return window_;
    }
    void Theme(bool dark);
    void ShowConnection();
    Json SelectedChannels() {
        return Selection();
    }

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK ConnectProc(HWND, UINT, WPARAM, LPARAM);
    HWND Child(const wchar_t *type, const wchar_t *text, int id, DWORD style = 0);
    void Scroll(int position);
    void Layout();
    void Paint(HDC);
    void Action(int id);
    void Refresh();
    HWND Combo(int id, const std::vector<std::wstring> &items, int selected = 0);
    HWND Control(int id) {
        return GetDlgItem(window_, id);
    }
    void CreateControls();
    void DetailsLayout(int width, int top);
    void PaintDetails(HDC dc, int width, int top);
    void SelectMode();
    void DeviceAction(int id);
    void Export(bool csv);
    void Profile(bool save);
    void EditSequence();
    void ShowScpi();
    static INT_PTR CALLBACK TextProc(HWND, UINT, WPARAM, LPARAM);
    std::wstring ChooseFile(bool save, const wchar_t *filter, const wchar_t *extension);
    std::string Mode() const;
    void PopulateSettings();
    Json Selection();
    void Submit(Json command);
    std::wstring Text(HWND);
    void LoadTask();
    void SaveTask();
    std::wstring TaskPath();
    void TaskFile(bool save);
    HWND window_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr, valueFont_ = nullptr, smallFont_ = nullptr;
    bool dark_ = true;
    int dpi_ = 96;
    UiFieldStyle fieldStyle_{&dark_, nullptr, 96};
    int scroll_ = 0;
    int contentHeight_ = 0;
    int cardTop_ = 0, cardWidth_ = 0, cardHeight_ = 0, cardColumns_ = 1, taskTop_ = 0;
    bool scrolling_ = false;
    int tab_ = 0, automation_ = 0, settings_ = 0, draftMode_ = 0, independentSelection_ = 1;
    int detailsTop_ = 0;
    UiLayoutBatch* currentLayout_ = nullptr;
    bool refreshRequired_ = true;
    bool paused_ = false, populated_ = false, reporting_ = false;
    std::string sampleStamp_;
    std::deque<Json> history_;
    Json steps_ = Json::array({Json{{"voltage", 0.0}, {"current", 0.0}, {"durationMs", 5000}, {"enabled", true}}});
    std::map<int, HWND> controls_;
    PowerService *service_ = nullptr;
    std::array<HWND, 3> check_{}, voltage_{}, current_{}, apply_{};
    std::array<HWND, 4> buttons_{};
    HWND protectionButton_ = nullptr;
    HWND onTime_ = nullptr, offTime_ = nullptr, count_ = nullptr, start_ = nullptr, stop_ = nullptr, save_ = nullptr;
    HBRUSH brush_ = nullptr, fieldBrush_ = nullptr, surfaceBrush_ = nullptr;
    std::string action_;
    std::deque<std::string> pendingActions_;
    Json state_ = Json::object();
};
} // namespace serialctl
