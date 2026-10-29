#pragma once
#include "TerminalModel.h"
#include "CmdLineEditor.h"
#include <algorithm>
#include <memory>
namespace serialctl {
// UI-thread-owned draft projection. Device revisions, cursor edits and view
// changes invalidate it; repainting an unchanged draft does no history copying.
class TerminalViewportController {
public:
    struct State {
        TerminalModel terminal;
        int terminalScrollOffset = 0, terminalHorizontalOffset = 0;
        bool terminalHasNewOutput = false;
        std::wstring terminalTitle;
        CmdLineEditor cmdEditor;
        bool cmdInteractionWarning = false;
        std::wstring paintProbeText;
        std::intptr_t paintProbe = 0;
        std::int64_t pendingProbeArrival = 0;
    };
    struct Grid {
        int cellWidth = 9, lineHeight = 19, rows = 24, columns = 80;
    };
    struct Interaction {
        bool selecting = false, selectionActive = false;
        size_t anchorLine = 0, focusLine = 0;
        int anchorColumn = 0, focusColumn = 0;
        bool scrollbarDragging = false, horizontalScrollbarDragging = false;
        int scrollbarThumbOffset = 0, horizontalScrollbarThumbOffset = 0;
        int wheelRemainder = 0, horizontalWheelRemainder = 0, zoomWheelRemainder = 0;
    };
    Grid& Metrics() { return grid_; }
    const Grid& Metrics() const { return grid_; }
    Interaction& Input() { return input_; }
    const Interaction& Input() const { return input_; }
    void Scroll(State& state, int lines) {
        const int maximum = std::max(0, static_cast<int>(state.terminal.DisplayLineCount()) - grid_.rows);
        state.terminalScrollOffset = std::max(0, std::min(maximum, state.terminalScrollOffset + lines));
        if (!state.terminalScrollOffset) state.terminalHasNewOutput = false;
    }
    void ScrollHorizontal(State& state, int columns) {
        const int maximum = std::max(0, static_cast<int>(state.terminal.MaximumDisplayColumns()) - grid_.columns);
        state.terminalHorizontalOffset = std::max(0, std::min(maximum, state.terminalHorizontalOffset + columns));
    }
    void ToBottom(State& state) { state.terminalScrollOffset = 0; state.terminalHasNewOutput = false; }
    void Select(size_t line, int column, bool extend) {
        if (!extend) {
            input_.anchorLine = input_.focusLine = line;
            input_.anchorColumn = input_.focusColumn = column;
            input_.selectionActive = false;
        } else {
            input_.focusLine = line; input_.focusColumn = column;
            input_.selectionActive = input_.anchorLine != line || input_.anchorColumn != column;
        }
    }
    void ClearSelection() { input_.selecting = input_.selectionActive = false; }
    std::wstring SelectionText(const State& state, bool all) const {
        if (all) return state.terminal.PlainText(false);
        return input_.selectionActive ? state.terminal.SelectedText(input_.anchorLine, input_.anchorColumn,
                                                                   input_.focusLine, input_.focusColumn) : L"";
    }
    bool PointToCell(const State& state, int x, int y, int textLeft, int padding, int right,
                     size_t& line, int& column) const {
        const auto& model = state.terminal;
        const auto totalLines = model.DisplayLineCount();
        const auto visible = static_cast<size_t>(grid_.rows + state.terminalScrollOffset);
        const auto topLine = totalLines > visible ? totalLines - visible : 0;
        const int row = std::max(0, std::min(grid_.rows - 1, (y - padding) / std::max(1, grid_.lineHeight)));
        if (!totalLines || x < textLeft || x >= right) return false;
        line = std::min(totalLines - 1, topLine + static_cast<size_t>(row));
        const auto& cells = model.DisplayLine(line).cells;
        if (cells.empty()) return false;
        const int viewColumn = (x - textLeft) / std::max(1, grid_.cellWidth);
        column = std::max(0, std::min(static_cast<int>(cells.size()) - 1, viewColumn + state.terminalHorizontalOffset));
        if (column > 0 && cells[static_cast<size_t>(column)].continuation) --column;
        return true;
    }
    void Resize(State& state, bool wrap, bool active, int previousRows) {
        auto& model = state.terminal;
        const auto total = model.DisplayLineCount();
        const auto visible = static_cast<size_t>(previousRows + state.terminalScrollOffset);
        auto read = model.CaptureAnchor(total > visible ? total - visible : 0);
        auto anchor = model.CaptureAnchor(input_.anchorLine, input_.anchorColumn);
        auto focus = model.CaptureAnchor(input_.focusLine, input_.focusColumn);
        model.SetLineWrapView(wrap); model.Resize(grid_.columns, grid_.rows);
        if (state.terminalScrollOffset > 0) {
            const auto line = model.LocateAnchor(read).first;
            state.terminalScrollOffset = std::max(0, static_cast<int>(model.DisplayLineCount() -
                std::min(line, model.DisplayLineCount())) - grid_.rows);
        }
        if (active && input_.selectionActive) {
            const auto a = model.LocateAnchor(anchor), b = model.LocateAnchor(focus);
            input_.anchorLine = a.first; input_.anchorColumn = a.second;
            input_.focusLine = b.first; input_.focusColumn = b.second;
        }
        state.terminalScrollOffset = std::min(state.terminalScrollOffset,
            std::max(0, static_cast<int>(model.DisplayLineCount()) - grid_.rows));
        state.terminalHorizontalOffset = std::max(0, std::min(state.terminalHorizontalOffset,
            std::max(0, static_cast<int>(model.MaximumDisplayColumns()) - grid_.columns)));
    }
    const TerminalModel& Preview(const TerminalModel& source, const std::wstring& text, size_t cursor) {
        if (!preview_ || source_ != &source || revision_ != source.Revision() || text_ != text || cursor_ != cursor) {
            preview_ = std::make_unique<TerminalModel>(source.PreviewInput(text, cursor));
            source_ = &source; revision_ = source.Revision(); text_ = text; cursor_ = cursor;
        }
        return *preview_;
    }
    void Clear() { preview_.reset(); source_ = nullptr; text_.clear(); }
private:
    Grid grid_;
    Interaction input_;
    const TerminalModel* source_ = nullptr;
    std::uint64_t revision_ = 0;
    size_t cursor_ = 0;
    std::wstring text_;
    std::unique_ptr<TerminalModel> preview_;
};
} // namespace serialctl
