#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace serialctl {

// Fixed-memory, lock-free samples for hot paths. Percentile sorting only runs
// when diagnostics are requested.
class PerformanceMetrics {
public:
    enum class Timing { Decode, Parse, Paint, UiResponse };
    struct Percentiles {
        std::uint64_t samples = 0;
        std::uint64_t p50Usec = 0, p95Usec = 0, p99Usec = 0;
    };
    struct Snapshot {
        bool enabled = false;
        std::uint64_t receivedBytes = 0;
        std::uint64_t receiveBytesPerSecond = 0;
        Percentiles decode, parse, paint, uiResponse;
    };

    PerformanceMetrics() {
        const auto* setting = std::getenv("SERIALCTL_PERF_METRICS");
        enabled_.store(!setting || *setting != '0', std::memory_order_relaxed);
    }
    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_release); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void RecordReceive(size_t bytes) {
        if (!Enabled() || !bytes) return;
        receivedBytes_.fetch_add(bytes, std::memory_order_relaxed);
        const auto second = CurrentSecond();
        auto observed = receiveSecond_.load(std::memory_order_relaxed);
        if (observed == second) {
            receiveCurrent_.fetch_add(bytes, std::memory_order_relaxed);
            return;
        }
        if (receiveSecond_.compare_exchange_strong(observed, second, std::memory_order_relaxed)) {
            const auto previous = receiveCurrent_.exchange(bytes, std::memory_order_relaxed);
            receivePrevious_.store(second == observed + 1 ? previous : 0, std::memory_order_relaxed);
        } else {
            receiveCurrent_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
    void Record(Timing timing, std::uint64_t usec) {
        if (!Enabled()) return;
        Series(timing).Record((std::max)(std::uint64_t(1), usec));
    }
    Snapshot Read() const {
        Snapshot result;
        result.enabled = Enabled();
        if (!result.enabled) return result;
        result.receivedBytes = receivedBytes_.load(std::memory_order_relaxed);
        const auto second = CurrentSecond();
        const auto recordedSecond = receiveSecond_.load(std::memory_order_relaxed);
        result.receiveBytesPerSecond = recordedSecond == second
            ? receiveCurrent_.load(std::memory_order_relaxed)
            : recordedSecond + 1 == second ? receivePrevious_.load(std::memory_order_relaxed) : 0;
        result.decode = decode_.Read(); result.parse = parse_.Read();
        result.paint = paint_.Read(); result.uiResponse = uiResponse_.Read();
        return result;
    }

private:
    static constexpr size_t Capacity = 512;
    struct SampleSeries {
        SampleSeries() { for (auto& sample : values) sample.store(0, std::memory_order_relaxed); }
        void Record(std::uint64_t value) {
            const auto index = next.fetch_add(1, std::memory_order_relaxed);
            values[index % Capacity].store(value, std::memory_order_relaxed);
        }
        Percentiles Read() const {
            const auto count = next.load(std::memory_order_acquire);
            const auto available = static_cast<size_t>((std::min)(count, std::uint64_t(Capacity)));
            const auto first = count > Capacity ? count - Capacity : 0;
            std::vector<std::uint64_t> samples;
            samples.reserve(available);
            for (size_t i = 0; i < available; ++i) {
                const auto value = values[(first + i) % Capacity].load(std::memory_order_relaxed);
                if (value) samples.push_back(value);
            }
            if (samples.empty()) return {};
            std::sort(samples.begin(), samples.end());
            const auto at = [&](unsigned percentile) {
                return samples[((samples.size() - 1) * percentile + 50) / 100];
            };
            return {samples.size(), at(50), at(95), at(99)};
        }
        std::array<std::atomic<std::uint64_t>, Capacity> values;
        std::atomic<std::uint64_t> next{0};
    };
    static std::uint64_t CurrentSecond() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    SampleSeries& Series(Timing timing) {
        if (timing == Timing::Decode) return decode_;
        if (timing == Timing::Parse) return parse_;
        if (timing == Timing::Paint) return paint_;
        return uiResponse_;
    }

    std::atomic_bool enabled_{false};
    std::atomic<std::uint64_t> receivedBytes_{0};
    std::atomic<std::uint64_t> receiveSecond_{0}, receiveCurrent_{0}, receivePrevious_{0};
    SampleSeries decode_, parse_, paint_, uiResponse_;
};

} // namespace serialctl
