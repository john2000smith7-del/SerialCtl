#include "TerminalModel.h"
#include "TerminalViewportController.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
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
