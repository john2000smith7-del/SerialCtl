#include "TerminalModel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0;
    const auto index = static_cast<size_t>((sorted.size() - 1) * fraction + 0.5);
    return sorted[(std::min)(index, sorted.size() - 1)];
}

struct Result {
    std::string name;
    size_t inputBytes = 0;
    double elapsedMs = 0;
    double p50Usec = 0, p95Usec = 0, p99Usec = 0;
    size_t historyRows = 0;
};

Result Run(const std::string& name, const std::wstring& pattern, size_t targetBytes) {
    serialctl::TerminalModel model(120, 40);
    std::vector<double> samples;
    const size_t patternBytes = pattern.size() * sizeof(wchar_t);
    const size_t repetitions = (std::max)(size_t(1), targetBytes / (std::max)(size_t(1), patternBytes));
    size_t consumed = 0;
    constexpr size_t ChunkCharacters = 4096;
    std::wstring chunk;
    chunk.reserve(ChunkCharacters + pattern.size());
    const auto started = Clock::now();
    for (size_t repetition = 0; repetition < repetitions;) {
        chunk.clear();
        while (repetition < repetitions && chunk.size() + pattern.size() <= ChunkCharacters) {
            chunk += pattern;
            ++repetition;
        }
        const auto before = Clock::now();
        model.Feed(chunk, L"");
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - before).count());
        consumed += chunk.size() * sizeof(wchar_t);
    }
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::sort(samples.begin(), samples.end());
    return {name, consumed, elapsed, Percentile(samples, .50), Percentile(samples, .95),
            Percentile(samples, .99), model.HistorySize()};
}
} // namespace

int main() {
    const size_t target = 8 * 1024 * 1024;
    const std::vector<Result> results = {
        Run("plain", L"0123456789 abcdefghijklmnopqrstuvwxyz\r\n", target),
        Run("short", L"x\r\n", target),
        Run("long", std::wstring(4090, L'x') + L"\r\n", target),
        Run("ansi", L"\x1b[38;5;196mred\x1b[0m \x1b[48;2;1;2;3mcolor\x1b[0m\r\n", target),
        Run("cursor", L"\x1b[2J\x1b[Htop\x1b[10;20Hmiddle\x1b[K", target),
        Run("unicode-wide", L"ASCII 中文 日本語 한글 🙂 end\r\n", target),
    };
    std::cout << std::fixed << std::setprecision(3) << "{\"targetBytes\":" << target << ",\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        if (i) std::cout << ',';
        const double mibPerSecond = result.elapsedMs > 0
            ? result.inputBytes / 1048576.0 / (result.elapsedMs / 1000.0) : 0;
        std::cout << "{\"name\":\"" << result.name << "\",\"inputBytes\":" << result.inputBytes
                  << ",\"elapsedMs\":" << result.elapsedMs << ",\"mibPerSecond\":" << mibPerSecond
                  << ",\"chunkP50Usec\":" << result.p50Usec << ",\"chunkP95Usec\":" << result.p95Usec
                  << ",\"chunkP99Usec\":" << result.p99Usec << ",\"historyRows\":" << result.historyRows << '}';
    }
    std::cout << "]}\n";
    return 0;
}
