#pragma once
#include "TerminalViewportController.h"
#include "TerminalDecoder.h"
#include "PaintBuffer.h"
#include <windows.h>
namespace serialctl {
class TerminalViewport {
public:
    struct State : TerminalViewportController::State { TerminalDecoder decoder; };
    struct Options {
        bool dark = false, timestamps = false, liveMonitor = false, measure = false;
        int mode = -1;
        HFONT font = nullptr, smallFont = nullptr;
    };
    auto& Metrics() { return controller_.Metrics(); }
    const auto& Metrics() const { return controller_.Metrics(); }
    auto& Input() { return controller_.Input(); }
    const auto& Input() const { return controller_.Input(); }
    void Clear() { controller_.Clear(); }
    void Scroll(State& state, int lines) { controller_.Scroll(state, lines); }
    void ScrollHorizontal(State& state, int columns) { controller_.ScrollHorizontal(state, columns); }
    void ToBottom(State& state) { controller_.ToBottom(state); }
    void Select(size_t line, int column, bool extend) { controller_.Select(line, column, extend); }
    void ClearSelection() { controller_.ClearSelection(); }
    std::wstring SelectionText(const State& state, bool all) const { return controller_.SelectionText(state, all); }
    void Resize(State& state, bool wrap, bool active, int previousRows) { controller_.Resize(state, wrap, active, previousRows); }
    bool PointToCell(HWND, const State&, POINT, const Options&, size_t&, int&) const;
    void Measure(HWND, HFONT, bool timestamps);
    void Paint(HDC, HWND, State*, const Options&);
    enum class PointerResult { Unhandled, Handled, CopySelection };
    PointerResult Pointer(HWND, UINT, LPARAM, State*, const Options&);
private:
    TerminalViewportController controller_;
    PaintBuffer paintBuffer_;
    std::uint64_t paintSample_ = 0;
};
} // namespace serialctl
