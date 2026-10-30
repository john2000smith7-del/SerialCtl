#pragma once
#include "CommandDocument.h"
#include <functional>
#include <richedit.h>
#include <windows.h>
namespace serialctl {
class CommandNotebook {
  public:
    bool Create(HWND parent, int id);
    HWND Handle() const {
        return window_;
    }
    HWND Editor() const {
        return editor_;
    }
    void Style(bool dark, int dpi, HFONT ui, HFONT mono);
    void SetText(const std::wstring &text, bool dirty = false);
    const std::wstring &Text() const {
        return text_;
    }
    void SetName(std::wstring name) {
        name_ = std::move(name);
        InvalidateRect(window_, nullptr, FALSE);
    }
    bool Dirty() const {
        return dirty_;
    }
    void Saved() {
        dirty_ = false;
        SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void State(bool writable, bool running, size_t sent = 0, size_t total = 0, std::wstring target = {});
    void Selection(LONG a, LONG b);
    void SelectLine(size_t line);
    std::vector<std::wstring> SelectedCommands() const;
    void SetDoubleClick(bool value) {
        doubleClick_ = value;
    }
    bool DoubleClick() const {
        return doubleClick_;
    }
    unsigned Interval() const {
        return interval_;
    }
    void SetInterval(unsigned v) {
        interval_ = std::clamp(v, 1u, 60000u);
    }
    std::function<void(int)> action; // run=1, save=2, import=3, export=4
    std::function<void(size_t)> runLine;
    std::function<void(const std::wstring &)> drop;
    std::function<void()> changed, optionsChanged;

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Message(UINT, WPARAM, LPARAM);
    std::pair<size_t, size_t> SelectedRows() const;
    size_t NativeRow(LONG position) const;
    void Layout();
    void Paint(HDC);
    void Refresh(bool edit = false);
    void SelectionInk(HDC);
    void Scroll(int x, int y);
    void Menu();
    int D(int v) const {
        return MulDiv(v, dpi_, 96);
    }
    HWND window_ = nullptr, editor_ = nullptr, buttons_[4]{}, more_ = nullptr, tooltip_ = nullptr;
    HFONT ui_ = nullptr, mono_ = nullptr;
    bool dark_ = false, dirty_ = false, loading_ = false, writable_ = false, running_ = false, doubleClick_ = true;
    int dpi_ = 96, rowHeight_ = 20, top_ = 80, bottom_ = 0, gutter_ = 48, scrollAxis_ = 0, scrollGrab_ = 0;
    int armedLine_ = -1;
    int contentWidth_ = 0;
    unsigned interval_ = 500;
    size_t sent_ = 0, total_ = 0;
    CHARRANGE selection_{0, 0};
    std::wstring text_, name_ = L"命令笔记.txt", target_;
    std::vector<CommandLine> lines_{{0, 0, 0}};
    RECT vertical_{}, horizontal_{};
};
} // namespace serialctl
