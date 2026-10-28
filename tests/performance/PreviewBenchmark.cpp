#include "TerminalModel.h"
#include "json.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
PROCESS_MEMORY_COUNTERS Memory() {
    PROCESS_MEMORY_COUNTERS result{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &result, sizeof(result)))
        throw std::runtime_error("Cannot read process memory counters");
    return result;
}
}
int main() {
    try {
        serialctl::TerminalModel source(80, 24);
        std::wstring history;
        for (int i = 0; i < 20000; ++i) history += std::wstring(60, L'x') + L"\r\n";
        source.Feed(history + L"> ", L"T");
        source.SetLineWrapView(false);
        for (size_t i = 0; i < source.DisplayLineCount(); ++i) source.DisplayLine(i);
        if (source.HistorySize() < 19000) throw std::runtime_error("History fixture was truncated");
        const auto before = Memory();
        SIZE_T maxWorkingSet = before.WorkingSetSize;
        std::vector<double> samples;
        for (int i = 0; i < 30; ++i) {
            const auto started = std::chrono::steady_clock::now();
            auto preview = source.PreviewInput(L"中文long draft", 4);
            samples.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count());
            const auto memory = Memory();
            maxWorkingSet = (std::max)(maxWorkingSet, memory.WorkingSetSize);
            if (preview.PlainText(false).find(L"> 中文long draft") == std::wstring::npos)
                throw std::runtime_error("Preview text differs");
        }
        const auto after = Memory();
        std::sort(samples.begin(), samples.end());
        nlohmann::json result{{"historyInputLines", 20000}, {"retainedHistoryRows", source.HistorySize()},
            {"iterations", 30}, {"samplesMs", samples}, {"p50Ms", samples[15]}, {"p95Ms", samples[28]},
            {"workingBeforeBytes", before.WorkingSetSize}, {"maxWorkingDuringPreviewBytes", maxWorkingSet},
            {"peakWorkingSetBytes", after.PeakWorkingSetSize},
            {"scope", "Windows C++ model, 80x24, 20000 input lines; creation timed, full text checked outside timing"}};
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
