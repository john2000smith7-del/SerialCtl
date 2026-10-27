#pragma once
#include "AiPetModel.h"
#include "UiStyle.h"
#include <memory>

namespace serialctl {
inline constexpr UINT AiPetMenuRequested = WM_APP + 300;
inline constexpr UINT AiPetActivityChanged = WM_APP + 301;
inline constexpr UINT AiPetPreferencesChanged = WM_APP + 302;

class AiPetPane {
  public:
    ~AiPetPane();
    bool Create(HWND parent, HFONT font, bool dark, pet::Settings settings);
    HWND Handle() const {
        return window_;
    }
    const pet::Settings &Preferences() const {
        return model_.Preferences();
    }
    int DesiredHeight() const;
    void Present(bool aiPage, bool available);
    void Theme(bool dark);
    void Activity(pet::Activity activity);
    void ShowMenu(POINT point);
    void Shutdown();

  private:
#ifdef SERIALCTL_AI_TESTING
    friend struct AiPetPaneTestAccess;
#endif
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);
    void Tick();
    void Paint(HDC);
    void Action(int);
    void SettingsDialog();
    void Changed();
    int D(int n) const {
        return MulDiv(n, dpi_, 96);
    }
    HWND window_ = nullptr, parent_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr;
    int dpi_ = 96;
    bool dark_ = false, available_ = false, dragging_ = false, moved_ = false, hover_ = false;
    bool keyboardFocus_ = true;
    POINT grab_{};
    pet::Settings dragStart_;
    pet::Model model_;
    pet::Sample sample_;
    RECT sprite_{}, menu_{};
    std::unique_ptr<UiResourceBitmap> image_;
};
} // namespace serialctl
