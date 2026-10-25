#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <windows.h>

namespace serialctl {
enum class PipeRead { Idle, More, Closed, Cancelled, Limit };
// Never wait for EOF or drain an unlimited producer. A caller gets control back
// every 64 KiB to service cancellation, deadlines and the password pipe.
inline PipeRead ReadProcessOutput(HANDLE pipe, std::string &output, size_t maximum,
                                  const std::atomic_bool *cancel = nullptr) {
    std::array<char, 4096> buffer{};
    for (size_t budget = 0; budget < 65536;) {
        if (cancel && cancel->load()) return PipeRead::Cancelled;
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return PipeRead::Closed;
        if (!available) return PipeRead::Idle;
        const auto count = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (output.size() > maximum || count > maximum - output.size()) return PipeRead::Limit;
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), count, &read, nullptr) || !read) return PipeRead::Closed;
        output.append(buffer.data(), read);
        budget += read;
    }
    return PipeRead::More;
}
} // namespace serialctl
