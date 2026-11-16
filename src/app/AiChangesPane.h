#pragma once

#include "PaintBuffer.h"
#include "UiMetrics.h"
#include "UiStyle.h"
#include "WorkspaceActivity.h"
#include "LocalWorkspace.h"

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace serialctl {

// Embedded child pane hosted inside the AI right sidebar under the "修改" tab.
// Uses a two-card rounded layout synchronized with the 2-row top card of the
// Terminal / Compile tabs: Row 2 hosts the AI/Git scope dropdown,
// narrowed changed-file dropdown, editable search dropdown ("search"
// placeholder), and prev/next match buttons; Card 2 renders the sticky file
// header, line-number gutter, git-style +/- bands, VS Code-like syntax
// highlighting, collapsible unchanged sections (+N more lines), and selection.
class AiChangesPane {
  public:
    enum class Scope { CurrentTurn, All, GitStatus, GitHistory };
    enum class ViewMode { Diff, Content };

    enum ControlId {
        ModeComboBox = 2101,
        FileComboBox = 2102,
        SearchComboBox = 2103,
        PrevMatchButton = 2104,
        NextMatchButton = 2105,
        ContentView = 2106
    };

    ~AiChangesPane();

    bool Create(HWND parent, HFONT font, bool dark, int dpi);
    HWND Handle() const { return window_; }

    void Theme(bool dark, int dpi, HFONT font);
    void SetWrap(bool wrap);
    bool Wrap() const { return wrap_; }

    void SetCopyCallback(std::function<void(const std::wstring &text, const wchar_t *notice)> callback);
    void SetGitWorkspace(std::shared_ptr<ai::LocalWorkspaceService> workspace);
    void SetChanges(const std::vector<ai::FileChangeRecord> &records, std::uint64_t currentTurnSeq,
                    const std::string &selectChangeId = {});
    void SetCurrentTurnSeq(std::uint64_t currentTurnSeq);
    void FocusChange(const std::string &changeId);

    void SetScope(Scope scope);
    Scope CurrentScope() const { return scope_; }
    void SetViewMode(ViewMode mode);
    ViewMode CurrentViewMode() const { return viewMode_; }

    void SetSearchQuery(const std::wstring &query);
    void StepSearchMatch(int delta);
    size_t SearchMatchCount() const { return searchMatches_.size(); }
    size_t ActiveSearchMatchIndex() const { return activeMatchIndex_; }
    bool ExpandCollapsedBlock(size_t blockIndex);

    size_t TotalCount() const { return records_.size(); }
    size_t CurrentTurnCount() const;
    std::uint64_t CurrentTurnSeq() const { return currentTurnSeq_; }
    std::string SelectedChangeId() const { return selectedChangeId_; }
    int TopCardHeight() const {
        return D(6) + Ui::CompactHeight + D(6);
    }

  private:
    enum class DiffLineKind { Normal, Header, Hunk, Added, Removed, Collapsed };

    struct LogicalLine {
        DiffLineKind kind = DiffLineKind::Normal;
        int oldLine = 0;
        int newLine = 0;
        size_t collapsedIndex = SIZE_MAX;
        size_t hiddenCount = 0;
        bool startsInBlockComment = false;
        std::wstring text;
    };

    struct CollapsedSection {
        bool expanded = false;
        std::vector<LogicalLine> lines;
    };

    struct DisplayRow {
        size_t logicalIndex = 0;
        size_t logicalOffset = 0;
        DiffLineKind kind = DiffLineKind::Normal;
        int oldLine = 0;
        int newLine = 0;
        size_t collapsedIndex = SIZE_MAX;
        size_t collapsedInnerIndex = SIZE_MAX;
        bool continuation = false;
        bool startsInBlockComment = false;
        std::wstring text;
    };

    struct SearchMatch {
        bool inCollapsed = false;
        size_t collapsedIndex = SIZE_MAX;
        size_t row = 0;
        size_t startCol = 0;
        size_t length = 0;
    };

    struct TextPoint {
        size_t row = 0;
        size_t col = 0;
        bool operator==(const TextPoint &other) const { return row == other.row && col == other.col; }
        bool operator!=(const TextPoint &other) const { return !(*this == other); }
        bool operator<(const TextPoint &other) const {
            return row < other.row || (row == other.row && col < other.col);
        }
    };

    static LRESULT CALLBACK HostProc(HWND h, UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK SearchEditSubclassProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK ViewerProc(HWND h, UINT message, WPARAM w, LPARAM l);

    int D(int value) const { return MulDiv(value, dpi_, 96); }
    int HeaderHeightPx() const;
    int GutterWidthPx() const;
    COLORREF TrayColor() const;
    COLORREF Card1FillColor() const;
    void ApplyChildTheme();
    void RebuildFonts();
    void DestroyFonts();
    void UpdateButtons();
    void SyncModeComboSelection();
    void CommitSearchHistory(const std::wstring &query);
    void UpdateSearchControls();
    void RebuildFilteredList(const std::string &preferSelectId = {});
    bool IsGitScope() const;
    const std::vector<ai::FileChangeRecord> &VisibleRecords() const;
    void RefreshGitChanges();
    void RebuildLogicalLines();
    void RebuildDisplayRows();
    void RebuildSearchMatches(bool jumpToFirst);
    const ai::FileChangeRecord *SelectedRecord() const;

    void Layout();
    void PaintHost(HDC dc);
    void PaintViewer(HDC dc);

    void ScrollViewerY(int deltaPx);
    void ScrollViewerX(int deltaPx);
    void EnsureViewerRowVisible(size_t row);

    RECT ViewerScrollbarThumbRect(const RECT &client) const;
    RECT CollapsedPillRectForRow(int top, int bottom, int gutterW, int clipRight, const std::wstring &text, HDC dc) const;
    TextPoint ViewerHitTest(int x, int y) const;
    std::wstring ViewerSelectedText() const;
    std::wstring ViewerFullText() const;
    bool HasViewerSelection() const { return hasSelection_ && selAnchor_ != selFocus_; }
    void CopyText(const std::wstring &text, const wchar_t *notice);
    void CopySelectionOrAll();

    static DiffLineKind ClassifyLine(const std::string &line, bool diffMode);
    static std::wstring ToWideText(const std::string &utf8);

    HWND window_ = nullptr;
    HWND modeCombo_ = nullptr;
    HWND fileCombo_ = nullptr;
    HWND searchCombo_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND prevMatchButton_ = nullptr;
    HWND nextMatchButton_ = nullptr;
    HWND viewer_ = nullptr;

    UiFieldStyle fieldStyle_{&dark_, nullptr, 96, true};

    HFONT uiFont_ = nullptr;
    HFONT ownedUiFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT boldSmallFont_ = nullptr;
    HFONT codeFont_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr;
    HBRUSH card1Brush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;

    PaintBuffer viewerBuffer_;

    bool dark_ = false;
    bool wrap_ = true;
    int dpi_ = 96;

    Scope scope_ = Scope::CurrentTurn;
    std::wstring compactScopeLabel_ = L"DIFF";
    ViewMode viewMode_ = ViewMode::Diff;
    std::uint64_t currentTurnSeq_ = 0;

    std::vector<ai::FileChangeRecord> records_;
    std::shared_ptr<ai::LocalWorkspaceService> gitWorkspace_;
    ai::GitFileChanges gitChanges_;
    std::future<ai::GitFileChanges> gitQuery_;
    std::wstring gitRoot_;
    std::wstring gitQueryRoot_;
    Scope gitQueryScope_ = Scope::CurrentTurn;
    ULONGLONG nextGitRefresh_ = 0;
    std::vector<size_t> filteredIndices_;
    std::string selectedChangeId_;

    std::vector<LogicalLine> logicalLines_;
    std::vector<CollapsedSection> collapsedSections_;
    std::vector<DisplayRow> rows_;
    int maxLineNumber_ = 0;
    int viewerScrollY_ = 0;
    int viewerScrollX_ = 0;
    int viewerMaxLinePx_ = 0;
    int viewerRowHeight_ = 20;
    int viewerCharWidth_ = 8;
    bool viewerDraggingThumb_ = false;
    int viewerDragGrabOffset_ = 0;

    std::wstring searchQuery_;
    std::vector<std::wstring> searchHistory_;
    std::vector<SearchMatch> searchMatches_;
    size_t activeMatchIndex_ = 0;
    bool suppressSearchNotify_ = false;
    RECT card1Rect_{};

    bool hasSelection_ = false;
    bool selectingText_ = false;
    TextPoint selAnchor_{};
    TextPoint selFocus_{};

    std::function<void(const std::wstring &text, const wchar_t *notice)> copyCallback_;
};

} // namespace serialctl
