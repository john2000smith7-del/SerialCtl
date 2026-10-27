#include "AiPetPane.h"
#include "UiTestEvidence.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::wstring evidenceName;
bool menuTest = false, saveDialog = false, modalObserved = false, modalFits = true;
HWND FindNative(const wchar_t *cls) {
    struct Search {
        const wchar_t *cls;
        HWND found = nullptr;
    } search{cls};
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND h, LPARAM l) -> BOOL {
            auto &q = *reinterpret_cast<Search *>(l);
            wchar_t cls[64]{};
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, q.cls) == 0 && IsWindowVisible(h)) {
                q.found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}
void CALLBACK ModalTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND h = FindNative(menuTest ? L"SerialCtlPetMenu" : L"#32770");
    if (!h)
        return;
    modalObserved = true;
    if (!menuTest) {
        RECT r{};
        GetWindowRect(h, &r);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
        modalFits = r.left >= monitor.rcWork.left && r.top >= monitor.rcWork.top && r.right <= monitor.rcWork.right &&
                    r.bottom <= monitor.rcWork.bottom;
    }
    KillTimer(nullptr, timer);
    if (menuTest) {
        SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(h, WM_KEYDOWN, VK_RIGHT, 0);
    }
    ui_test::SaveUiEvidence(h, evidenceName);
    if (menuTest)
        PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
    else {
        if (saveDialog)
            SendDlgItemMessageW(h, 103, BM_SETCHECK, BST_UNCHECKED, 0);
        PostMessageW(h, WM_COMMAND, saveDialog ? IDOK : IDCANCEL, 0);
    }
}
} // namespace
namespace serialctl {
struct AiPetPaneTestAccess {
    static void Run(AiPetPane &pane, HWND parent) {
        const auto original = pane.Preferences();
        for (int dpi : {96, 120, 144, 192})
            for (bool dark : {false, true})
                for (int size : {96, 120, 144}) {
                    pane.dpi_ = dpi;
                    pane.dark_ = dark;
                    auto p = original;
                    p.size = size;
                    p.opening = false;
                    pane.model_.Configure(p, 100);
                    pane.model_.Page(true, 100);
                    const int width = MulDiv(148, dpi, 96), height = pane.DesiredHeight();
                    SetWindowPos(pane.Handle(), nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
                    pane.Present(true, true);
                    for (auto activity : {pet::Activity::Idle, pet::Activity::Thinking, pet::Activity::Working,
                                          pet::Activity::Done, pet::Activity::Failed}) {
                        pane.model_.SetActivity(activity, 100);
                        pane.sample_ = pane.model_.At(101);
                        HDC dc = GetDC(pane.Handle());
                        pane.Paint(dc);
                        ReleaseDC(pane.Handle(), dc);
                        Check(pane.sprite_.top >= MulDiv(28, dpi, 96), "Pet overlaps its status bubble");
                        Check(pane.sprite_.left >= -2 && pane.sprite_.right <= width + 2,
                              "Pet exceeds the connection lane");
                    }
                    if (size == 96) {
                        ui_test::SaveUiEvidence(
                            pane.Handle(), L"v208-pet-" + std::to_wstring(dpi) + (dark ? L"-dark.png" : L"-light.png"),
                            [&](HDC dc) { pane.Paint(dc); });
                        pane.hover_ = true;
                        ui_test::SaveUiEvidence(pane.Handle(),
                                                L"v208-pet-hover-" + std::to_wstring(dpi) +
                                                    (dark ? L"-dark.png" : L"-light.png"),
                                                [&](HDC dc) { pane.Paint(dc); });
                        Check(pane.menu_.bottom <= pane.sprite_.top, "Pet menu overlaps the character and status lane");
                        pane.hover_ = false;
                    }
                }
        for (int dpi : {96, 120, 144, 192})
            for (bool dark : {false, true}) {
                pane.model_.Configure(original, GetTickCount64());
                pane.dpi_ = dpi;
                pane.Theme(dark);
                const auto old = pane.font_;
                pane.font_ =
                    CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                evidenceName = L"v208-pet-settings-" + std::to_wstring(dpi) + (dark ? L"-dark.png" : L"-light.png");
                menuTest = false;
                saveDialog = false;
                SetTimer(nullptr, 0, 50, ModalTimer);
                pane.SettingsDialog();
                Check(modalFits, "Pet settings escaped the monitor work area at high DPI");
                Check(pane.Preferences().opening == original.opening, "Cancel modified pet settings");
                DeleteObject(pane.font_);
                pane.font_ = old;
            }
        pane.dpi_ = 96;
        pane.Theme(false);
        pane.model_.Configure(original, 100);
        for (bool dark : {false, true}) {
            pane.Theme(dark);
            menuTest = true;
            evidenceName = dark ? L"v208-pet-menu-dark.png" : L"v208-pet-menu-light.png";
            SetTimer(nullptr, 0, 50, ModalTimer);
            modalObserved = false;
            SetForegroundWindow(parent);
            SetActiveWindow(parent);
            SetFocus(pane.Handle());
            pane.ShowMenu({60, 80});
            Check(modalObserved, "Pet action menu did not remain open for interaction");
        }
        menuTest = false;
        saveDialog = true;
        evidenceName = L"v208-pet-settings-save.png";
        SetTimer(nullptr, 0, 50, ModalTimer);
        pane.SettingsDialog();
        Check(!pane.Preferences().opening, "Save did not persist opening preference");
        pane.model_.Configure(original, 100);
        pane.model_.Page(false, 100);
        pane.model_.Page(true, 200);
        Check(pane.model_.OpeningDue() == 4200, "Opening was not scheduled four seconds after entry");
        pane.model_.Page(true, 300);
        Check(pane.model_.OpeningDue() == 4200, "Layout reset opening delay");
        pane.sample_ = pane.model_.At(4200);
        Check(pane.sample_.opening, "Opening did not start");
        for (unsigned elapsed = 0; elapsed < 8000; elapsed += 40) {
            pane.sample_ = pane.model_.At(4200 + elapsed);
            Check(pane.sample_.pose != 16, "Rejected double-punch pose returned");
        }
        pane.model_.At(12200);
        Check(pane.model_.CurrentAction() == pet::Action::None, "Opening did not finish");
        pane.model_.Page(false, 12300);
        pane.model_.Page(true, 12400);
        pane.model_.Page(false, 12500);
        Check(!pane.model_.At(17000).manual, "Leaving AI did not cancel opening");
        pane.model_.Page(true, 18000);
        pane.model_.Play(pet::Action::Wave, 18001);
        pane.model_.SetActivity(pet::Activity::Working, 18100);
        Check(pane.model_.At(20000).pose == 33, "Latest AI state was not restored");
        pane.model_.Configure(original, GetTickCount64());
        pane.model_.Stop(GetTickCount64());
        SetWindowPos(pane.Handle(), nullptr, 0, 0, 148, pane.DesiredHeight(), SWP_NOZORDER | SWP_SHOWWINDOW);
        pane.sample_ = pane.model_.At(GetTickCount64());
        {
            HDC dc = GetDC(pane.Handle());
            pane.Paint(dc);
            ReleaseDC(pane.Handle(), dc);
        }
        const POINT grab{(pane.sprite_.left + pane.sprite_.right) / 2, (pane.sprite_.top + pane.sprite_.bottom) / 2};
        SendMessageW(pane.Handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grab.x, grab.y));
        auto focusPixel = [&] {
            HDC dc = GetDC(pane.Handle());
            pane.Paint(dc);
            const auto pixel = GetPixel(dc, 2, pane.DesiredHeight() / 2);
            ReleaseDC(pane.Handle(), dc);
            return pixel;
        };
        const auto mousePixel = focusPixel();
        Check(mousePixel == UiTheme(pane.dark_).surface, "Mouse activation left a keyboard focus outline");
        SendMessageW(pane.Handle(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(grab.x + 8, grab.y));
        {
            HDC dc = GetDC(pane.Handle());
            pane.Paint(dc);
            ReleaseDC(pane.Handle(), dc);
        }
        Check((pane.sprite_.left + pane.sprite_.right) / 2 >= grab.x + 6, "Pet dragging did not follow the mouse");
        SendMessageW(pane.Handle(), WM_LBUTTONUP, 0, MAKELPARAM(grab.x + 8, grab.y));
        Check(pane.model_.CurrentAction() == pet::Action::None, "Dragging unexpectedly started an action");
        SendMessageW(pane.Handle(), WM_KEYDOWN, VK_RIGHT, 0);
        Check(focusPixel() != mousePixel, "Keyboard activation lost its focus cue");
        pane.Present(false, false);
        Check(!pane.available_, "Hidden component retained active timer state");
        pane.Present(true, true);
        EnableWindow(parent, FALSE);
        pane.Tick();
        Check(!pane.sample_.opening, "Modal dialog started opening");
        EnableWindow(parent, TRUE);
        pane.Present(false, false);
        pane.model_.Configure(original, GetTickCount64());
        SetWindowPos(pane.Handle(), nullptr, 0, 0, 148, pane.DesiredHeight(), SWP_NOZORDER | SWP_SHOWWINDOW);
        pane.Present(true, true);
        const auto entry = GetTickCount64();
        bool observedOpening = false;
        while (GetTickCount64() - entry < 4400) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (pane.sample_.opening) {
                Check(GetTickCount64() - entry >= 3900, "Native timer opened too early");
                observedOpening = true;
                break;
            }
            Sleep(5);
        }
        Check(observedOpening, "Native timer failed to play opening after four seconds");
        pane.Present(false, false);
    }
};
} // namespace serialctl
int main() {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &startup, nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HWND parent = CreateWindowExW(0, L"STATIC", L"Pet native fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 600, 700,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 0;
    {
        AiPetPane pane;
        try {
            Check(pane.Create(parent, font, false, {}), "Cannot decode embedded native pet asset");
            AiPetPaneTestAccess::Run(pane, parent);
            std::cout << "AI pet: native asset, both themes/four DPI scales, bounded dock, delayed/cancelled opening "
                         "and state restoration PASS\n";
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            result = 1;
        }
        pane.Shutdown();
    }
    DestroyWindow(parent);
    DeleteObject(font);
    Gdiplus::GdiplusShutdown(token);
    return result;
}
