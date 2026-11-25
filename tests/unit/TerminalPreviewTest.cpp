#include "TerminalModel.h"
#include "TerminalViewportController.h"
#include "TerminalSearch.h"
#include "SerialShortcut.h"
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <filesystem>
#endif
using namespace serialctl;
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main() {
    try {
        const auto f2 = SerialF2Payload();
        Require(std::string(f2.begin(), f2.end()) == "232start", "serial F2 payload is exact ASCII");
        for (unsigned bit = 0; bit < 32; ++bit) {
            const std::uint32_t flipped = FlipRegisterBit(0, bit);
            Require(flipped == (std::uint32_t{1} << bit), "every register bit is addressable");
            Require(FlipRegisterBit(flipped, bit) == 0, "register bit toggles both ways");
        }
        Require(FlipRegisterBit(0xA5A5A5A5u, 32) == 0xA5A5A5A5u, "out of range bit ignored");
        TerminalModel source(80, 24);
        std::wstring history;
        for (int i = 0; i < 20000; ++i) history += std::wstring(60, L'x') + L"\r\n";
        source.Feed(history + L"> ", L"T"); source.SetLineWrapView(false);
        for (size_t i = 0; i < source.DisplayLineCount(); ++i) source.DisplayLine(i);
        Require(source.HistorySize() > 19000, "benchmark must retain large history");
        std::vector<double> samples;
        for (int i = 0; i < 30; ++i) {
            const auto start = std::chrono::steady_clock::now();
            auto draft = source.PreviewInput(L"中文long draft", 4);
            Require(draft.PlainText(false).find(L"> 中文long draft") != std::wstring::npos, "draft text");
            samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        TerminalViewportController viewport;
        const auto& first = viewport.Preview(source, L"abc", 1);
        const auto* cached = &first;
        Require(&viewport.Preview(source, L"abc", 1) == cached, "unchanged repaint reuses preview");
        source.Feed(L"changed", L"");
        Require(viewport.Preview(source, L"abc", 1).PlainText(false).find(L"changedabc") != std::wstring::npos, "device revision invalidates preview");
        auto snapshot = source.PreviewInput(L"immutable", 3);
        const auto saved = snapshot.PlainText(false);
        source.Clear(); source.Resize(3, 4); source.Feed(L"replacement", L"");
        Require(snapshot.PlainText(false) == saved, "preview snapshot owns immutable historical rows");
        for (int columns : {1, 2, 3, 80, 132}) {
            source.Resize(columns, 4); source.Feed(L"中文abcdef\r\n\x1b[?1049h中文xyz", L"");
            Require(source.MaximumDisplayColumns() == static_cast<size_t>(columns), "alternate grid width invariant");
            source.Feed(L"\x1b[?1049l", L""); source.SetLineWrapView(true);
            for (size_t i = 0; i < source.DisplayLineCount(); ++i)
                Require(source.DisplayLine(i).cells.size() <= static_cast<size_t>(columns), "wrapped row width invariant");
            Require(source.MaximumDisplayColumns() == static_cast<size_t>(columns), "wrapped maximum invariant");
        }
        TerminalViewportController::State view;
        viewport.Metrics() = {9, 19, 2, 8};
        view.terminal.Resize(8, 2);
        view.terminal.Feed(L"one\r\ntwo\r\nthree\r\nfour", L"");
        viewport.Scroll(view, 100000);
        Require(view.terminalScrollOffset == static_cast<int>(view.terminal.DisplayLineCount()) - 2,
                "viewport clamps upward history scrolling");
        view.terminalHasNewOutput = true; viewport.ToBottom(view);
        Require(!view.terminalScrollOffset && !view.terminalHasNewOutput, "viewport bottom clears unread-output marker");
        TerminalModel wideErase(80, 3);
        wideErase.Feed(L"中", L"");
        wideErase.Feed(L"\b \b", L"");
        const auto& erasedRow = wideErase.DisplayLine(0);
        Require(erasedRow.cells[0].character == L' ' && !erasedRow.cells[1].continuation,
                "wide serial character partial erase leaves no orphan half-cell");
        view.terminal.Clear(); view.terminal.Feed(L"中A", L"");
        size_t hitLine = 0; int hitColumn = -1;
        Require(viewport.PointToCell(view, 8 + 9, 8, 8, 8, 200, hitLine, hitColumn) && hitColumn == 0,
                "wide-character continuation hit maps to its leading cell");
        viewport.Select(hitLine, 0, false); viewport.Select(hitLine, 2, true);
        Require(viewport.SelectionText(view, false) == L"中A", "owned selection preserves wide text");
        viewport.ClearSelection();
        Require(viewport.SelectionText(view, false).empty(), "cleared viewport selection cannot copy stale text");

        // TerminalSearchEngine unit tests
        {
            TerminalModel searchModel(12, 3, 100);
            searchModel.SetLineWrapView(true);
            searchModel.Feed(L"first Error\r\n[0-9]+ err\r\n中文ERROR!\r\nabcdefghijklMNO\r\nlast error", L"");
            const auto matches = TerminalSearchEngine::FindAll(searchModel, L"error");
            Require(matches.size() == 3, "case-insensitive search across scrollback and screen");
            Require(matches[0].startLine == 0 && matches[0].startColumn == 6 && matches[0].endColumn == 11,
                    "first match coordinates in history");
            Require(matches[1].startLine == 2 && matches[1].startColumn == 4 && matches[1].endColumn == 9,
                    "match after wide characters uses display cell columns");

            const auto literalMatches = TerminalSearchEngine::FindAll(searchModel, L"[0-9]+");
            Require(literalMatches.size() == 1 && literalMatches[0].startLine == 1 && literalMatches[0].startColumn == 0,
                    "search matches regex metacharacters literally");

            const auto wrappedMatches = TerminalSearchEngine::FindAll(searchModel, L"klMN");
            Require(wrappedMatches.size() == 1 && wrappedMatches[0].startLine < wrappedMatches[0].endLine,
                    "search matches across soft-wrapped display lines");

            const auto wideMatches = TerminalSearchEngine::FindAll(searchModel, L"中文");
            Require(wideMatches.size() == 1 && wideMatches[0].startColumn == 0 && wideMatches[0].endColumn == 4,
                    "wide character match endColumn includes continuation cells");

            Require(TerminalSearchEngine::StepMatchIndex(0, 3, 1) == 1, "step next advances");
            Require(TerminalSearchEngine::StepMatchIndex(2, 3, 1) == 0, "step next wraps to 0");
            Require(TerminalSearchEngine::StepMatchIndex(0, 3, -1) == 2, "step prev wraps to last");
            Require(TerminalSearchEngine::StepMatchIndex(-1, 0, 1) == -1, "step on empty returns -1");

            Require(TerminalSearchEngine::InitialMatchIndex(matches, 2) == 1, "initial match picks first at or after topLine");
            Require(TerminalSearchEngine::InitialMatchIndex(matches, 999) == 0, "initial match wraps to 0 when past all");

            const auto spans = TerminalSearchEngine::CollectVisibleHighlights(matches, 1, 2, 4);
            Require(spans.size() == 1 && spans[0].line == 2 && spans[0].current, "visible highlights mark only current match");

            const int scrollToTop = TerminalSearchEngine::ScrollOffsetForMatch(matches[0], searchModel.DisplayLineCount(), 3, 0);
            Require(scrollToTop > 0, "scroll offset moves up to off-screen history match");
            const int stayPut = TerminalSearchEngine::ScrollOffsetForMatch(matches.back(), searchModel.DisplayLineCount(), 3, 0);
            Require(stayPut == 0, "scroll offset unchanged when match already visible");

            // Occlusion geometry tests (top-right floating search card)
            {
                TerminalSearchViewportGeometry geom;
                geom.padding = 8;
                geom.textLeft = 8;
                geom.cellWidth = 9;
                geom.lineHeight = 19;
                geom.rows = 10;
                geom.columns = 80;
                geom.textRight = 8 + 80 * 9;
                geom.lineWrap = true;
                geom.maxColumns = 80;
                // Search card covers top-right rows 0..1 (x: 400..720, y: 6..42)
                geom.occludedLeft = 400;
                geom.occludedTop = 6;
                geom.occludedRight = 720;
                geom.occludedBottom = 42;

                const size_t totalLines = 40;
                // Viewport at scrollOffset=0 shows lines 30..39.
                // Line 30 is row 0. Right-side match at cols 50..55 (x: 458..503) is covered!
                const TerminalSearchMatch occludedTopRight{30, 50, 30, 55};
                Require(!TerminalSearchEngine::IsMatchVisiblyClear(occludedTopRight, totalLines, 0, 0, geom),
                        "top-right match under search card is detected as occluded");
                const auto scrolled = TerminalSearchEngine::ScrollOffsetsForMatch(occludedTopRight, totalLines, 0, 0, geom);
                Require(scrolled.first > 0, "occluded top-right match scrolls viewport to uncover match");
                Require(TerminalSearchEngine::IsMatchVisiblyClear(occludedTopRight, totalLines, scrolled.first, scrolled.second, geom),
                        "match is visibly clear after scrolling out from under search card");

                // Left-side match on row 0 (cols 2..8, x: 26..80) is NOT covered by card
                const TerminalSearchMatch clearTopLeft{30, 2, 30, 8};
                Require(TerminalSearchEngine::IsMatchVisiblyClear(clearTopLeft, totalLines, 0, 0, geom),
                        "top-left match outside search card remains visibly clear");
                const auto unchanged = TerminalSearchEngine::ScrollOffsetsForMatch(clearTopLeft, totalLines, 0, 0, geom);
                Require(unchanged.first == 0, "non-occluded top-left match does not cause unnecessary scroll");

                // Soft-wrapped match spanning line 30 (cols 75..80) -> line 31 (cols 0..4)
                const TerminalSearchMatch wrappedOccluded{30, 75, 31, 4};
                Require(!TerminalSearchEngine::IsMatchVisiblyClear(wrappedOccluded, totalLines, 0, 0, geom),
                        "soft-wrapped match starting under card is detected as occluded");
                const auto wrappedScrolled = TerminalSearchEngine::ScrollOffsetsForMatch(wrappedOccluded, totalLines, 0, 0, geom);
                Require(TerminalSearchEngine::IsMatchVisiblyClear(wrappedOccluded, totalLines, wrappedScrolled.first, wrappedScrolled.second, geom),
                        "soft-wrapped match becomes visibly clear after scroll");

                // High-DPI / narrow window where card covers 2 rows (y: 9..72, lineHeight=28 -> 3 rows)
                TerminalSearchViewportGeometry hidpiGeom = geom;
                hidpiGeom.cellWidth = 14;
                hidpiGeom.lineHeight = 28;
                hidpiGeom.columns = 32;
                hidpiGeom.textRight = 8 + 32 * 14;
                hidpiGeom.occludedLeft = 180;
                hidpiGeom.occludedTop = 9;
                hidpiGeom.occludedRight = hidpiGeom.textRight;
                hidpiGeom.occludedBottom = 72;
                const TerminalSearchMatch hidpiOccluded{31, 20, 31, 25}; // row 1 on narrow 32-col view
                Require(!TerminalSearchEngine::IsMatchVisiblyClear(hidpiOccluded, totalLines, 0, 0, hidpiGeom),
                        "high-DPI narrow window detects row-1 occlusion under card");
                const auto hidpiScrolled = TerminalSearchEngine::ScrollOffsetsForMatch(hidpiOccluded, totalLines, 0, 0, hidpiGeom);
                Require(TerminalSearchEngine::IsMatchVisiblyClear(hidpiOccluded, totalLines, hidpiScrolled.first, hidpiScrolled.second, hidpiGeom),
                        "high-DPI narrow window scrolls match clear of card");

                // Earliest preserved line (24-row viewport, 100-line model, line 0 right-aligned match)
                TerminalSearchViewportGeometry fullGeom = geom;
                fullGeom.rows = 24;
                const size_t fullLines = 100;
                const TerminalSearchMatch earliestLineMatch{0, 60, 0, 70};
                const auto earliestScrolled = TerminalSearchEngine::ScrollOffsetsForMatch(earliestLineMatch, fullLines, 0, 0, fullGeom);
                Require(earliestScrolled.first == 76, "earliest preserved line scrolls to very top");
                Require(!TerminalSearchEngine::IsMatchVisiblyClear(earliestLineMatch, fullLines, earliestScrolled.first, earliestScrolled.second, fullGeom),
                        "earliest preserved line right-side match remains occluded at top-right card");
                RECT avoidRect{};
                Require(TerminalSearchEngine::MatchBoundingRect(earliestLineMatch, fullLines, earliestScrolled.first, earliestScrolled.second, fullGeom, avoidRect),
                        "bounding rect computed for occluded earliest line match");
                Require(avoidRect.bottom <= fullGeom.occludedBottom && avoidRect.right > fullGeom.occludedLeft,
                        "avoid rect pinpoints top-right occluded region so card can relocate below it");

                // Later match at same column (line 10, cols 60..70) can scroll clear without moving card
                const TerminalSearchMatch laterMatch{10, 60, 10, 70};
                const auto laterScrolled = TerminalSearchEngine::ScrollOffsetsForMatch(laterMatch, fullLines, 0, 0, fullGeom);
                Require(TerminalSearchEngine::IsMatchVisiblyClear(laterMatch, fullLines, laterScrolled.first, laterScrolled.second, fullGeom),
                        "later match at same column scrolls cleanly below top-right card");

                // No scroll history (24-row viewport, 10-line model, line 0 right-aligned match)
                const size_t shortLines = 10;
                const auto noHistoryScrolled = TerminalSearchEngine::ScrollOffsetsForMatch(earliestLineMatch, shortLines, 0, 0, fullGeom);
                Require(noHistoryScrolled.first == 0, "no-scroll-history viewport stays at offset 0");
                Require(!TerminalSearchEngine::IsMatchVisiblyClear(earliestLineMatch, shortLines, noHistoryScrolled.first, noHistoryScrolled.second, fullGeom),
                        "no-scroll-history top-right match triggers card avoidance");
                RECT noHistoryAvoid{};
                Require(TerminalSearchEngine::MatchBoundingRect(earliestLineMatch, shortLines, 0, 0, fullGeom, noHistoryAvoid),
                        "bounding rect computed for no-scroll-history top-right match");
            }

            // Long history + off-UI-thread TerminalSearchWorker coalescing/cancellation/responsiveness (20k lines, ~1.48M matches)
            {
                TerminalModel bigModel(80, 24, 20000);
                std::wstring chunk;
                chunk.reserve(80 * 200);
                const std::wstring row(74, L'x');
                for (int i = 0; i < 20000; ++i) {
                    chunk += row;
                    chunk += L"\r\n";
                    if (chunk.size() >= 16000) {
                        bigModel.Feed(chunk, L"");
                        chunk.clear();
                    }
                }
                if (!chunk.empty()) bigModel.Feed(chunk, L"");

                const auto snapStart = std::chrono::steady_clock::now();
                auto snap1 = TerminalSearchEngine::CaptureSnapshot(bigModel, &bigModel, 1, L"x");
                const double snapMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - snapStart).count();

                const auto scanStart = std::chrono::steady_clock::now();
                const auto sharedMatches = TerminalSearchEngine::FindAllFromSnapshot(snap1);
                const double scanMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - scanStart).count();
                Require(sharedMatches && sharedMatches->size() >= 1400000,
                        "20k-line long history finds ~1.48M single-char matches");

                // Verify zero-copy sharing via shared_ptr
                const auto* rawPtr = sharedMatches.get();
                auto sharedCopy = sharedMatches;
                Require(sharedCopy.get() == rawPtr && sharedCopy.use_count() >= 2,
                        "match list is shared without copying vector");

                // Real TerminalSearchWorker scheduling: rapid burst coalescing + stale cancellation
                std::mutex readyMutex;
                std::condition_variable readyCv;
                std::atomic<int> readyCount{0};
                TerminalSearchWorker worker;
                worker.Start([&] {
                    readyCount.fetch_add(1, std::memory_order_release);
                    std::lock_guard<std::mutex> lock(readyMutex);
                    readyCv.notify_all();
                });

                const int sessionA = 1;
                const int sessionB = 2;
                for (std::uint64_t g = 1; g <= 5; ++g) {
                    worker.Submit(TerminalSearchEngine::CaptureSnapshot(bigModel, &sessionA, g, L"x"));
                }
                // Feed continuous output and submit generation 6
                for (int burst = 0; burst < 10; ++burst) {
                    bigModel.Feed(L"xxxx\r\n", L"");
                }
                worker.Submit(TerminalSearchEngine::CaptureSnapshot(bigModel, &sessionA, 6, L"x"));

                std::optional<TerminalSearchResult> workerResult;
                {
                    std::unique_lock<std::mutex> lock(readyMutex);
                    const bool ok = readyCv.wait_for(lock, std::chrono::seconds(5), [&] {
                        workerResult = worker.ConsumeResult();
                        return workerResult.has_value() && workerResult->generation == 6;
                    });
                    Require(ok && workerResult.has_value(), "worker delivers latest coalesced generation 6");
                }
                Require(workerResult->owner == &sessionA && workerResult->matches &&
                        workerResult->matches->size() > sharedMatches->size(),
                        "coalesced worker result reflects continuous output");

                // Verify Cancel() drops in-flight/ready stale results (e.g., on close or session switch)
                worker.Submit(TerminalSearchEngine::CaptureSnapshot(bigModel, &sessionA, 7, L"x"));
                worker.Cancel();
                worker.Submit(TerminalSearchEngine::CaptureSnapshot(bigModel, &sessionB, 8, L"x"));
                std::optional<TerminalSearchResult> sessionBResult;
                {
                    std::unique_lock<std::mutex> lock(readyMutex);
                    const bool ok = readyCv.wait_for(lock, std::chrono::seconds(5), [&] {
                        sessionBResult = worker.ConsumeResult();
                        return sessionBResult.has_value() && sessionBResult->generation == 8;
                    });
                    Require(ok && sessionBResult.has_value() && sessionBResult->owner == &sessionB,
                            "worker isolates session B after Cancel() and rejects stale session A");
                }
                worker.Stop();

                const int stepped = TerminalSearchEngine::StepMatchIndex(0, workerResult->matches->size(), -1);
                Require(stepped == static_cast<int>(workerResult->matches->size() - 1),
                        "wrap-around navigation works on large match set");
                const auto vis = TerminalSearchEngine::CollectVisibleHighlights(
                    *workerResult->matches, stepped, bigModel.DisplayLineCount() - 24, bigModel.DisplayLineCount());
                Require(!vis.empty(), "visible highlights extracted in O(log N + visible) from large match set");
                std::cout << "{\"bigSearchMatches\":" << sharedMatches->size()
                          << ",\"snapshotMs\":" << snapMs
                          << ",\"workerScanMs\":" << scanMs << "}\n";
            }
        }
        std::sort(samples.begin(), samples.end());
        nlohmann::json result{{"samplesMs", samples}, {"p50Ms", samples[15]}, {"maxMs", samples.back()},
            {"logicalInputLines", 20000}, {"iterations", 30}, {"method", "preview creation and complete text validation; 80x24 grid, 60 chars per history row"}};
        std::cout << result.dump() << '\n';
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS memory{}; GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        result["peakWorkingSetBytes"] = memory.PeakWorkingSetSize;
        wchar_t directory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768)) std::ofstream(std::filesystem::path(directory) / L"terminal-preview.json") << result.dump(2);
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
