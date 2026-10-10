#include "CmdConsoleBridge.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <thread>
#include <windows.h>

namespace serialctl {
namespace {
bool ReadExact(HANDLE input, void *data, size_t size) {
    auto bytes = static_cast<char *>(data);
    while (size) {
        DWORD count = 0;
        if (!ReadFile(input, bytes, static_cast<DWORD>(size), &count, nullptr) || !count)
            return false;
        bytes += count;
        size -= count;
    }
    return true;
}
void Report(HANDLE output, const char *text) {
    DWORD written = 0;
    WriteFile(output, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}
bool FeedConsole(HANDLE input, HANDLE console, HANDLE process) {
    for (;;) {
        std::uint32_t size = 0;
        if (!ReadExact(input, &size, sizeof(size)))
            return false;
        // Only local UTF-16 IPC, not a second network application protocol.
        if (!size || size > 262144 || size % sizeof(wchar_t))
            return false;
        std::wstring text(size / sizeof(wchar_t), L'\0');
        if (!ReadExact(input, text.data(), size))
            return false;
        size_t offset = 0;
        while (offset < text.size()) {
            DWORD waiting = 0;
            if (!GetNumberOfConsoleInputEvents(console, &waiting))
                return false;
            if (waiting >= 16384) {
                // Bounded input backpressure while CMD/its child is busy. Stdout
                // still uses ReadFile arrival, never this input capacity check.
                if (WaitForSingleObject(process, 10) == WAIT_OBJECT_0)
                    return false;
                continue;
            }
            std::array<INPUT_RECORD, 512> records{};
            DWORD count = 0;
            const DWORD available = std::min<DWORD>(records.size(), 16384 - waiting);
            while (offset < text.size() && count < available) {
                const auto ch = text[offset++];
                if (ch == L'\n' && offset >= 2 && text[offset - 2] == L'\r')
                    continue;
                if ((ch < 32 && ch != L'\r' && ch != L'\n' && ch != L'\t') || ch == 127)
                    return false;
                auto &record = records[count++];
                record.EventType = KEY_EVENT;
                record.Event.KeyEvent.bKeyDown = TRUE;
                record.Event.KeyEvent.wRepeatCount = 1;
                record.Event.KeyEvent.wVirtualKeyCode = (ch == L'\r' || ch == L'\n') ? VK_RETURN : 0;
                record.Event.KeyEvent.uChar.UnicodeChar = ch == L'\n' ? L'\r' : ch;
            }
            DWORD sent = 0;
            while (sent < count) {
                DWORD written = 0;
                if (!WriteConsoleInputW(console, records.data() + sent, count - sent, &written) || !written)
                    return false;
                sent += written;
            }
        }
    }
}
} // namespace
int RunCmdConsoleBridge() {
    static_assert(sizeof(wchar_t) == 2, "Native Win32 console input is UTF-16");
    const auto input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE || GetFileType(output) != FILE_TYPE_PIPE)
        return 2;
    // The parent created a hidden, private Win7 console. Avoid inheriting its IPC
    // input in CMD: that would retain the pipe and reintroduce byte-wise decoding.
    SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto console = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        Report(output, "SerialCtl CMD console unavailable\r\n");
        return 3;
    }
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring exe = std::wstring(system) + L"\\cmd.exe";
    std::wstring command = QuoteCommandLineArgument(exe) + L" /D /Q /F:OFF /K";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = console;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION child{};
    // This dedicated process owns only these inheritable standard handles.
    // Legacy console handles must inherit within the same console on Win7;
    // they must not be duplicated across consoles/placed in a kernel handle list.
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) {
        CloseHandle(console);
        Report(output, "SerialCtl CMD creation failed\r\n");
        return 4;
    }
    CloseHandle(child.hThread);
    std::thread feeder([&] {
        if (!FeedConsole(input, console, child.hProcess))
            TerminateProcess(child.hProcess, 1);
    });
    WaitForSingleObject(child.hProcess, INFINITE);
    CancelSynchronousIo(reinterpret_cast<HANDLE>(feeder.native_handle()));
    feeder.join();
    DWORD result = 0;
    GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    CloseHandle(console);
    return static_cast<int>(result);
}
} // namespace serialctl
