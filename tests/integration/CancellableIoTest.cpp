#include "CancellableIo.h"
#include <future>
#include <iostream>
using namespace serialctl;
using namespace std::chrono_literals;
int main() {
    for (int i = 0; i < 64; ++i) {
        HANDLE writer = nullptr, childRead = nullptr, reader = nullptr, childWrite = nullptr;
        if (!CreateChildIoPipe(writer, childRead, true) || !CreateChildIoPipe(reader, childWrite, false)) return 1;
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CancellableIo readIo, writeIo;
        auto blockedRead = std::async(std::launch::async, [&] { char byte; DWORD n; return readIo.Transfer(reader, &byte, 1, false, stop, n); });
        if (blockedRead.wait_for(10ms) != std::future_status::timeout) return 2;
        char byte = 'X'; DWORD n = 0;
        if (!writeIo.Transfer(writer, &byte, 1, true, stop, n) || n != 1) return 3;
        char received; if (!ReadFile(childRead, &received, 1, &n, nullptr) || received != 'X') return 4;
        auto blockedWrite = std::async(std::launch::async, [&] { Bytes bytes(1024 * 1024); DWORD count; return writeIo.Transfer(writer, bytes.data(), static_cast<DWORD>(bytes.size()), true, stop, count); });
        if (blockedWrite.wait_for(10ms) != std::future_status::timeout) return 5;
        SetEvent(stop);
        if (blockedWrite.wait_for(1s) != std::future_status::ready || blockedRead.wait_for(1s) != std::future_status::ready || blockedWrite.get() || blockedRead.get()) return 6;
        // Cancellation already signaled before submission is also deterministic.
        if (writeIo.Transfer(writer, &byte, 1, true, stop, n) || GetLastError() != ERROR_OPERATION_ABORTED) return 7;
        for (HANDLE handle : {writer, childRead, reader, childWrite, stop}) CloseHandle(handle);
    }
    std::cout << "64 full-duplex, blocked write/read and pre-submission cancellation cycles passed\n";
    return 0;
}
