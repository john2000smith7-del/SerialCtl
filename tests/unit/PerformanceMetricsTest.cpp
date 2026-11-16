#include "PerformanceMetrics.h"

#include <cstdlib>
#include <iostream>

#define Check(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main() {
    serialctl::PerformanceMetrics metrics;
    metrics.SetEnabled(true);
    for (std::uint64_t value = 1; value <= 1000; ++value)
        metrics.Record(serialctl::PerformanceMetrics::Timing::Parse, value);
    metrics.RecordReceive(4096);
    const auto snapshot = metrics.Read();
    Check(snapshot.enabled && snapshot.receivedBytes == 4096);
    Check(snapshot.parse.samples == 512);
    Check(snapshot.parse.p50Usec >= 700 && snapshot.parse.p95Usec >= 970 && snapshot.parse.p99Usec >= 990);
    metrics.SetEnabled(false);
    metrics.RecordReceive(2);
    Check(!metrics.Read().enabled);
    std::cout << "Performance metrics tests passed\n";
    return 0;
}
