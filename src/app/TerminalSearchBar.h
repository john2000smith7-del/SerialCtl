#pragma once

#include "PaintBuffer.h"
#include <functional>
#include <string>
#include <windows.h>

namespace serialctl {

class TerminalSearchBar {
public:
    struct Callbacks {
        std::function<void(const std::wstring&)> onQueryChanged;
        std::function<void(int delta)> onNavigate;
        std::function<void()> onClose;
    };

    TerminalSearchBar() = default;
    ~TerminalSearchBar();
    TerminalSearchBar(const TerminalSearchBar&) = delete;
    TerminalSearchBar& operator=(const TerminalSearchBar&) = delete;

    bool Create(HWND parent, HINSTANCE instance, Callbacks callbacks);
    void Destroy();

    HWND Handle() const { return window_; }
    HWND EditHandle() const { return edit_; }
    bool Visible() const { return window_ && IsWindowVisible(window_) != FALSE; }
    bool ContainsFocus() const;

    void Open(bool selectAll = true);
    void Close(bool returnFocusToParent = true);
    void Theme(bool dark, int dpi, HFONT font);
    void Layout();
    void SetAvoidRect(const RECT* rect);
    RECT CardBoundsInParent() const { return Visible() ? cardBounds_ : RECT{}; }
    RECT DefaultCardBoundsInParent() const { return Visible() ? defaultCardBounds_ : RECT{}; }

    std::wstring Query() const;
    void SetCountState(int currentIndex, size_t totalMatches, bool hasQuery);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum ControlId {
        IdEdit = 101,
        IdPrev = 102,
        IdNext = 103,
        IdClose = 104
    };

    static LRESULT CALLBACK EditSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR subclassId, DWORD_PTR referenceData);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint(HDC dc);
    void DrawOwnerButton(const DRAWITEMSTRUCT& item);
    void UpdateFieldBrush();

    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    HWND edit_ = nullptr;
    HWND prevButton_ = nullptr;
    HWND nextButton_ = nullptr;
    HWND closeButton_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    PaintBuffer paintBuffer_;
    Callbacks callbacks_;

    bool dark_ = false;
    int dpi_ = 96;
    bool suppressChange_ = false;
    std::wstring countText_;
    RECT editFrameRect_{};
    RECT countRect_{};
    RECT cardBounds_{};
    RECT defaultCardBounds_{};
    RECT avoidRect_{};
    bool hasAvoidRect_ = false;
};

} // namespace serialctl
