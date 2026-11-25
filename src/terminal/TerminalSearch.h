#pragma once

#include "TerminalModel.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace serialctl {

struct TerminalSearchMatch {
    size_t startLine = 0;
    int startColumn = 0; // inclusive display cell index on startLine
    size_t endLine = 0;
    int endColumn = 0;   // exclusive display cell index on endLine
};

struct TerminalSearchHighlightSpan {
    size_t line = 0;
    int startColumn = 0; // inclusive
    int endColumn = 0;   // exclusive
    bool current = false;
};

struct TerminalSearchViewportGeometry {
    int textLeft = 0;
    int textRight = 0;
    int padding = 0;
    int cellWidth = 9;
    int lineHeight = 19;
    int rows = 24;
    int columns = 80;
    bool lineWrap = true;
    size_t maxColumns = 80;
    int occludedLeft = 0;
    int occludedTop = 0;
    int occludedRight = 0;
    int occludedBottom = 0;
};

struct TerminalSearchSnapshotLine {
    std::uint32_t textOffset = 0;
    std::uint32_t charCount = 0;
    bool wrapped = false;
    bool hasContinuation = false;
    std::vector<std::uint16_t> colStarts;
    std::vector<std::uint16_t> colEnds;
};

struct TerminalSearchSnapshot {
    const void* owner = nullptr;
    std::uint64_t modelRevision = 0;
    std::uint64_t generation = 0;
    std::wstring query;
    std::wstring textBuffer;
    std::vector<TerminalSearchSnapshotLine> lines;
};

struct TerminalSearchResult {
    const void* owner = nullptr;
    std::uint64_t modelRevision = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const std::vector<TerminalSearchMatch>> matches;
};

class TerminalSearchEngine {
public:
    static void FoldCaseInto(std::wstring_view text, std::wstring& out) {
        out.resize(text.size());
        if (text.empty()) return;
        bool asciiOnly = true;
        const wchar_t* src = text.data();
        wchar_t* dst = out.data();
        const size_t n = text.size();
        for (size_t i = 0; i < n; ++i) {
            wchar_t ch = src[i];
            if (ch >= 0x80) {
                asciiOnly = false;
                break;
            }
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
            dst[i] = ch;
        }
        if (asciiOnly) return;
        std::copy_n(src, n, dst);
#ifdef _WIN32
        const int len = static_cast<int>(n);
        LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, dst, len, dst, len);
#else
        for (size_t i = 0; i < n; ++i) {
            if (dst[i] >= L'A' && dst[i] <= L'Z') dst[i] = static_cast<wchar_t>(dst[i] + (L'a' - L'A'));
        }
#endif
    }

    static std::wstring FoldCase(std::wstring_view text) {
        std::wstring out;
        FoldCaseInto(text, out);
        return out;
    }

    static TerminalSearchSnapshot CaptureSnapshot(const TerminalModel& model,
                                                  const void* owner,
                                                  std::uint64_t generation,
                                                  std::wstring query) {
        TerminalSearchSnapshot snap;
        snap.owner = owner;
        snap.modelRevision = model.Revision();
        snap.generation = generation;
        snap.query = std::move(query);
        const size_t lineCount = model.DisplayLineCount();
        snap.lines.resize(lineCount);
        snap.textBuffer.reserve(lineCount * 64);

        for (size_t lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
            const TerminalLine& line = model.DisplayLine(lineIdx);
            const size_t used = std::min(line.used, line.cells.size());
            auto& dst = snap.lines[lineIdx];
            dst.textOffset = static_cast<std::uint32_t>(snap.textBuffer.size());
            dst.wrapped = line.wrapped;

            bool hasContinuation = false;
            for (size_t col = 0; col < used; ++col) {
                if (line.cells[col].continuation) {
                    hasContinuation = true;
                    break;
                }
            }
            dst.hasContinuation = hasContinuation;
            if (!hasContinuation) {
                const size_t base = snap.textBuffer.size();
                snap.textBuffer.resize(base + used);
                for (size_t col = 0; col < used; ++col) {
                    snap.textBuffer[base + col] = line.cells[col].character;
                }
                dst.charCount = static_cast<std::uint32_t>(used);
            } else {
                dst.colStarts.reserve(used);
                dst.colEnds.reserve(used);
                for (size_t col = 0; col < used; ++col) {
                    const TerminalCell& cell = line.cells[col];
                    if (cell.continuation) continue;
                    const uint16_t endCol = static_cast<uint16_t>(
                        (col + 1 < used && line.cells[col + 1].continuation) ? col + 2 : col + 1);
                    snap.textBuffer.push_back(cell.character);
                    dst.colStarts.push_back(static_cast<uint16_t>(col));
                    dst.colEnds.push_back(endCol);
                }
                dst.charCount = static_cast<std::uint32_t>(dst.colStarts.size());
            }
        }
        return snap;
    }

    static std::shared_ptr<const std::vector<TerminalSearchMatch>> FindAllFromSnapshot(
        const TerminalSearchSnapshot& snap,
        const std::atomic<std::uint64_t>* activeGeneration = nullptr) {
        auto matches = std::make_shared<std::vector<TerminalSearchMatch>>();
        if (snap.query.empty() || snap.lines.empty()) return matches;
        const std::wstring needle = FoldCase(snap.query);
        const size_t needleLen = needle.size();
        const size_t lineCount = snap.lines.size();

        struct CellPos {
            size_t line;
            int col;
            int endCol;
        };
        std::wstring logicalText;
        std::wstring foldedText;
        std::vector<CellPos> cellMap;
        logicalText.reserve(256);
        foldedText.reserve(256);
        cellMap.reserve(256);

        auto flushLogicalLine = [&] {
            if (logicalText.size() >= needleLen) {
                FoldCaseInto(logicalText, foldedText);
                size_t pos = 0;
                while ((pos = foldedText.find(needle, pos)) != std::wstring::npos) {
                    const size_t endIdx = pos + needleLen - 1;
                    matches->push_back(TerminalSearchMatch{
                        cellMap[pos].line,
                        cellMap[pos].col,
                        cellMap[endIdx].line,
                        cellMap[endIdx].endCol
                    });
                    pos += needleLen;
                }
            }
            logicalText.clear();
            cellMap.clear();
        };

        for (size_t lineIdx = 0; lineIdx < lineCount; ++lineIdx) {
            if (activeGeneration && (lineIdx & 0xFF) == 0 &&
                activeGeneration->load(std::memory_order_relaxed) != snap.generation) {
                return nullptr;
            }
            const auto& line = snap.lines[lineIdx];
            const std::wstring_view lineView(snap.textBuffer.data() + line.textOffset, line.charCount);
            if (!line.wrapped && logicalText.empty() && !line.hasContinuation) {
                if (lineView.size() < needleLen) continue;
                FoldCaseInto(lineView, foldedText);
                size_t pos = 0;
                while ((pos = foldedText.find(needle, pos)) != std::wstring::npos) {
                    matches->push_back(TerminalSearchMatch{
                        lineIdx,
                        static_cast<int>(pos),
                        lineIdx,
                        static_cast<int>(pos + needleLen)
                    });
                    pos += needleLen;
                }
                continue;
            }

            logicalText.append(lineView);
            if (!line.hasContinuation) {
                for (std::uint32_t col = 0; col < line.charCount; ++col) {
                    cellMap.push_back(CellPos{lineIdx, static_cast<int>(col), static_cast<int>(col + 1)});
                }
            } else {
                for (std::uint32_t i = 0; i < line.charCount; ++i) {
                    cellMap.push_back(CellPos{
                        lineIdx,
                        static_cast<int>(line.colStarts[i]),
                        static_cast<int>(line.colEnds[i])
                    });
                }
            }
            if (!line.wrapped) {
                flushLogicalLine();
            }
        }
        flushLogicalLine();
        if (activeGeneration && activeGeneration->load(std::memory_order_relaxed) != snap.generation) {
            return nullptr;
        }
        return matches;
    }

    static std::shared_ptr<const std::vector<TerminalSearchMatch>> FindAllShared(
        const TerminalModel& model, std::wstring_view query) {
        if (query.empty() || model.DisplayLineCount() == 0) {
            return std::make_shared<std::vector<TerminalSearchMatch>>();
        }
        const auto snap = CaptureSnapshot(model, nullptr, 0, std::wstring(query));
        return FindAllFromSnapshot(snap);
    }

    static std::vector<TerminalSearchMatch> FindAll(const TerminalModel& model, std::wstring_view query) {
        return *FindAllShared(model, query);
    }

    static int InitialMatchIndex(const std::vector<TerminalSearchMatch>& matches, size_t topLine) {
        if (matches.empty()) return -1;
        const auto it = std::lower_bound(matches.begin(), matches.end(), topLine,
            [](const TerminalSearchMatch& m, size_t line) { return m.endLine < line; });
        if (it != matches.end()) return static_cast<int>(it - matches.begin());
        return 0;
    }

    static int StepMatchIndex(int currentIndex, size_t matchCount, int delta) {
        if (matchCount == 0) return -1;
        const int count = static_cast<int>(matchCount);
        if (currentIndex < 0 || currentIndex >= count) return delta < 0 ? count - 1 : 0;
        const int step = delta >= 0 ? 1 : -1;
        return (currentIndex + step + count) % count;
    }

    static std::vector<TerminalSearchHighlightSpan> CollectVisibleHighlights(
        const std::vector<TerminalSearchMatch>& matches, int currentIndex, size_t topLine, size_t bottomLine) {
        std::vector<TerminalSearchHighlightSpan> spans;
        if (matches.empty() || topLine >= bottomLine) return spans;
        auto it = std::lower_bound(matches.begin(), matches.end(), topLine,
            [](const TerminalSearchMatch& m, size_t line) { return m.endLine < line; });
        for (; it != matches.end(); ++it) {
            if (it->startLine >= bottomLine) break;
            const bool isCurrent = static_cast<int>(it - matches.begin()) == currentIndex;
            const size_t firstLine = std::max(it->startLine, topLine);
            const size_t lastLine = std::min(it->endLine, bottomLine - 1);
            for (size_t line = firstLine; line <= lastLine; ++line) {
                const int startCol = (line == it->startLine) ? it->startColumn : 0;
                const int endCol = (line == it->endLine) ? it->endColumn : 0x3fffffff;
                if (startCol < endCol) {
                    spans.push_back(TerminalSearchHighlightSpan{line, startCol, endCol, isCurrent});
                }
            }
        }
        return spans;
    }

    static bool HasOcclusion(const TerminalSearchViewportGeometry& geom) {
        return geom.occludedRight > geom.occludedLeft && geom.occludedBottom > geom.occludedTop;
    }

    static bool IsMatchVisiblyClear(const TerminalSearchMatch& match, size_t totalLines,
                                    int scrollOffset, int horizontalOffset,
                                    const TerminalSearchViewportGeometry& geom) {
        const int rows = std::max(1, geom.rows);
        const int maxOffset = std::max(0, static_cast<int>(totalLines) - rows);
        const int clampedScroll = std::clamp(scrollOffset, 0, maxOffset);
        const size_t topLine = totalLines > static_cast<size_t>(rows + clampedScroll)
            ? totalLines - static_cast<size_t>(rows + clampedScroll)
            : 0;
        const size_t bottomLine = topLine + static_cast<size_t>(rows);
        if (match.startLine < topLine || match.endLine >= bottomLine) {
            return false;
        }
        const int cellW = std::max(1, geom.cellWidth);
        const int lineH = std::max(1, geom.lineHeight);
        const int textRight = std::max(geom.textLeft + cellW, geom.textRight);
        const bool checkOcclusion = HasOcclusion(geom);

        for (size_t line = match.startLine; line <= match.endLine; ++line) {
            const int viewRow = static_cast<int>(line - topLine);
            const int y1 = geom.padding + viewRow * lineH;
            const int y2 = y1 + lineH;
            const int colStart = (line == match.startLine) ? match.startColumn : 0;
            const int colEnd = (line == match.endLine)
                ? match.endColumn
                : std::max(colStart + 1, horizontalOffset + std::max(1, geom.columns));
            const int x1 = geom.textLeft + (colStart - horizontalOffset) * cellW;
            const int x2 = geom.textLeft + (colEnd - horizontalOffset) * cellW;
            if (x1 < geom.textLeft) return false;
            if (x2 > textRight && (colEnd - colStart) * cellW <= textRight - geom.textLeft) return false;
            if (x1 >= textRight) return false;
            if (checkOcclusion) {
                const int visibleX2 = std::min(x2, textRight);
                if (x1 < geom.occludedRight && visibleX2 > geom.occludedLeft &&
                    y1 < geom.occludedBottom && y2 > geom.occludedTop) {
                    return false;
                }
            }
        }
        return true;
    }

#ifdef _WIN32
    static bool MatchBoundingRect(const TerminalSearchMatch& match, size_t totalLines,
                                  int scrollOffset, int horizontalOffset,
                                  const TerminalSearchViewportGeometry& geom,
                                  RECT& outRect) {
        const int rows = std::max(1, geom.rows);
        const int maxOffset = std::max(0, static_cast<int>(totalLines) - rows);
        const int clampedScroll = std::clamp(scrollOffset, 0, maxOffset);
        const size_t topLine = totalLines > static_cast<size_t>(rows + clampedScroll)
            ? totalLines - static_cast<size_t>(rows + clampedScroll)
            : 0;
        const size_t bottomLine = topLine + static_cast<size_t>(rows);
        if (match.startLine < topLine || match.endLine >= bottomLine) return false;
        const int cellW = std::max(1, geom.cellWidth);
        const int lineH = std::max(1, geom.lineHeight);
        const int textRight = std::max(geom.textLeft + cellW, geom.textRight);

        int minX = textRight, maxX = geom.textLeft, minY = 0x3fffffff, maxY = -0x3fffffff;
        for (size_t line = match.startLine; line <= match.endLine; ++line) {
            const int viewRow = static_cast<int>(line - topLine);
            const int y1 = geom.padding + viewRow * lineH;
            const int y2 = y1 + lineH;
            const int colStart = (line == match.startLine) ? match.startColumn : 0;
            const int colEnd = (line == match.endLine)
                ? match.endColumn
                : std::max(colStart + 1, horizontalOffset + std::max(1, geom.columns));
            const int x1 = std::max(geom.textLeft, geom.textLeft + (colStart - horizontalOffset) * cellW);
            const int x2 = std::min(textRight, geom.textLeft + (colEnd - horizontalOffset) * cellW);
            if (x1 < x2) {
                minX = std::min(minX, x1);
                maxX = std::max(maxX, x2);
            }
            minY = std::min(minY, y1);
            maxY = std::max(maxY, y2);
        }
        if (minX >= maxX || minY >= maxY) return false;
        outRect = RECT{minX, minY, maxX, maxY};
        return true;
    }
#endif

    static std::pair<int, int> ScrollOffsetsForMatch(
        const TerminalSearchMatch& match, size_t totalLines,
        int currentScrollOffset, int currentHorizontalOffset,
        const TerminalSearchViewportGeometry& geom) {
        const int rows = std::max(1, geom.rows);
        const int columns = std::max(1, geom.columns);
        const int maxScroll = std::max(0, static_cast<int>(totalLines) - rows);
        const int maxH = geom.lineWrap ? 0 : std::max(0, static_cast<int>(geom.maxColumns) - columns);
        const int clampedScroll = std::clamp(currentScrollOffset, 0, maxScroll);
        const int clampedH = std::clamp(currentHorizontalOffset, 0, maxH);

        if (IsMatchVisiblyClear(match, totalLines, clampedScroll, clampedH, geom)) {
            return {clampedScroll, clampedH};
        }

        const int lineH = std::max(1, geom.lineHeight);
        const int cellW = std::max(1, geom.cellWidth);
        int firstClearRow = 0;
        if (HasOcclusion(geom) && geom.occludedBottom > geom.padding) {
            firstClearRow = std::min(
                std::max(0, rows - 1),
                (geom.occludedBottom - geom.padding + lineH - 1) / lineH);
        }

        const size_t matchSpan = match.endLine >= match.startLine ? (match.endLine - match.startLine + 1) : 1;
        int targetRow = std::clamp(rows / 2, firstClearRow, std::max(0, rows - 1));
        if (matchSpan > 1 && targetRow + static_cast<int>(matchSpan) > rows) {
            targetRow = std::max(firstClearRow, rows - static_cast<int>(matchSpan));
            targetRow = std::clamp(targetRow, 0, std::max(0, rows - 1));
        }

        const size_t desiredTop = match.startLine > static_cast<size_t>(targetRow)
            ? match.startLine - static_cast<size_t>(targetRow)
            : 0;
        const size_t clampedTop = std::min(desiredTop, static_cast<size_t>(maxScroll));
        const int newScroll = std::clamp(maxScroll - static_cast<int>(clampedTop), 0, maxScroll);

        int newH = clampedH;
        if (!geom.lineWrap) {
            if (match.startLine != match.endLine) {
                newH = 0;
            } else {
                const size_t actualTop = totalLines > static_cast<size_t>(rows + newScroll)
                    ? totalLines - static_cast<size_t>(rows + newScroll)
                    : 0;
                const int viewRow = static_cast<int>(match.startLine - actualTop);
                const int y1 = geom.padding + viewRow * lineH;
                const int y2 = y1 + lineH;
                const bool rowOccluded = HasOcclusion(geom) && y1 < geom.occludedBottom && y2 > geom.occludedTop;
                const int availableRight = rowOccluded && geom.occludedLeft > geom.textLeft
                    ? std::min(geom.textRight, geom.occludedLeft)
                    : geom.textRight;
                const int availableCols = std::max(1, (availableRight - geom.textLeft) / cellW);
                if (match.startColumn < newH || match.endColumn > newH + availableCols) {
                    newH = std::clamp(std::max(0, match.startColumn - availableCols / 4), 0, maxH);
                }
            }
        }

        return {newScroll, newH};
    }

    static int ScrollOffsetForMatch(const TerminalSearchMatch& match, size_t totalLines, int rows, int currentScrollOffset) {
        TerminalSearchViewportGeometry geom;
        geom.rows = std::max(1, rows);
        geom.columns = 80;
        geom.textLeft = 0;
        geom.textRight = geom.columns * geom.cellWidth;
        return ScrollOffsetsForMatch(match, totalLines, currentScrollOffset, 0, geom).first;
    }

    static int HorizontalOffsetForMatch(const TerminalSearchMatch& match, size_t maxColumns, int columns, int currentHorizontalOffset) {
        columns = std::max(1, columns);
        const int maxOffset = std::max(0, static_cast<int>(maxColumns) - columns);
        const int clampedCurrent = std::clamp(currentHorizontalOffset, 0, maxOffset);
        if (match.startLine != match.endLine) {
            return 0;
        }
        if (match.startColumn >= clampedCurrent && match.endColumn <= clampedCurrent + columns) {
            return clampedCurrent;
        }
        const int desired = std::max(0, match.startColumn - columns / 4);
        return std::clamp(desired, 0, maxOffset);
    }
};

class TerminalSearchWorker {
public:
    using NotifyCallback = std::function<void()>;

    TerminalSearchWorker() = default;
    ~TerminalSearchWorker() { Stop(); }
    TerminalSearchWorker(const TerminalSearchWorker&) = delete;
    TerminalSearchWorker& operator=(const TerminalSearchWorker&) = delete;

    void Start(NotifyCallback onReady) {
        std::lock_guard<std::mutex> lock(mutex_);
        onReady_ = std::move(onReady);
        if (thread_.joinable()) return;
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            activeGeneration_.fetch_add(1, std::memory_order_release);
            pending_.reset();
            ready_.reset();
            cv_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        activeGeneration_.fetch_add(1, std::memory_order_release);
        pending_.reset();
        ready_.reset();
    }

    void Submit(TerminalSearchSnapshot snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        activeGeneration_.store(snapshot.generation, std::memory_order_release);
        pending_ = std::move(snapshot);
        if (!thread_.joinable()) {
            stop_ = false;
            thread_ = std::thread([this] { Run(); });
        }
        cv_.notify_one();
    }

    std::optional<TerminalSearchResult> ConsumeResult() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) return std::nullopt;
        auto result = std::move(ready_);
        ready_.reset();
        return result;
    }

private:
    void Run() {
        while (true) {
            TerminalSearchSnapshot job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || pending_.has_value(); });
                if (stop_) return;
                job = std::move(*pending_);
                pending_.reset();
            }

            if (activeGeneration_.load(std::memory_order_acquire) != job.generation) {
                continue;
            }
            auto matches = TerminalSearchEngine::FindAllFromSnapshot(job, &activeGeneration_);
            if (!matches) {
                continue;
            }

            NotifyCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_ || activeGeneration_.load(std::memory_order_acquire) != job.generation) {
                    continue;
                }
                ready_ = TerminalSearchResult{
                    job.owner,
                    job.modelRevision,
                    job.generation,
                    std::move(matches)
                };
                cb = onReady_;
            }
            if (cb) cb();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stop_ = false;
    std::atomic<std::uint64_t> activeGeneration_{0};
    std::optional<TerminalSearchSnapshot> pending_;
    std::optional<TerminalSearchResult> ready_;
    NotifyCallback onReady_;
};

} // namespace serialctl
