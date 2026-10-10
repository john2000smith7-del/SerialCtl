#pragma once
#include "PowerService.h"
#include "UiStyle.h"
#include <array>
#include <functional>
namespace serialctl
{
class PowerPane
{
  public:
    ~PowerPane();
    bool Create(HWND parent, HFONT font, bool dark, PowerService *service);
    HWND Handle() const
    {
        return window_;
    }
    void Theme(bool dark);
    void ShowConnection();

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void ShowProtection();
    static INT_PTR CALLBACK ProtectionProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK ConnectProc(HWND, UINT, WPARAM, LPARAM);
    HWND Child(const wchar_t *type, const wchar_t *text, int id, DWORD style = 0);
    void Scroll(int position);
    void Layout();
    void Paint(HDC);
    void Action(int id);
    void Refresh();
    Json Selection();
    void Submit(Json command);
    std::wstring Text(HWND);
    void LoadTask();
    void SaveTask();
    std::wstring TaskPath();
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    bool dark_ = true;
    int dpi_ = 96;
    UiFieldStyle fieldStyle_{&dark_, nullptr, 96};
    int scroll_ = 0;
    bool scrolling_ = false;
    PowerService *service_ = nullptr;
    std::array<HWND, 3> check_{}, voltage_{}, current_{}, apply_{};
    std::array<HWND, 4> buttons_{};
    HWND protectionButton_ = nullptr;
    HWND onTime_ = nullptr, offTime_ = nullptr, count_ = nullptr, start_ = nullptr, stop_ = nullptr,
         save_ = nullptr;
    HBRUSH brush_ = nullptr, fieldBrush_ = nullptr;
    std::string action_;
    Json state_ = Json::object();
};
} // namespace serialctl
