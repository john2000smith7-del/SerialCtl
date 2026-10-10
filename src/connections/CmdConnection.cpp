#include "CmdConnection.h"
#include "ChildProcess.h"
#include "Win32Helpers.h"
#include <array>
#include <cstring>
#include <stdexcept>
namespace serialctl {
namespace {
std::mutex consoleQueryMutex;
}
bool CmdConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    Stop();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, outWrite = nullptr;
    auto fail = [&] {
        if (inRead)
            CloseHandle(inRead);
        if (outWrite)
            CloseHandle(outWrite);
        Stop();
        return false;
    };
    if (!CreatePipe(&inRead, &input_, &sa, 0) || !CreatePipe(&output_, &outWrite, &sa, 0)) {
        error = Win32ErrorMessage();
        return fail();
    }
    if (!SetHandleInheritable(input_, false, L"CMD stdin", error) ||
        !SetHandleInheritable(output_, false, L"CMD stdout", error))
        return fail();
    RestrictedProcessStartup startup;
    if (!startup.Initialize(inRead, outWrite, outWrite, {}, error))
        return fail();
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring exe = module;
    const auto slash = exe.find_last_of(L"\\/");
    // Tests exercise the same installed application bridge, not a test worker.
    if (exe.substr(slash + 1) == L"cmd_connection_test.exe")
        exe = exe.substr(0, slash + 1) + L"serialctl.exe";
    std::wstring command = QuoteCommandLineArgument(exe) + L" --serialctl-cmd-bridge";
    PROCESS_INFORMATION pi{};
    startup.StartupInfo()->dwFlags |= STARTF_USESHOWWINDOW;
    startup.StartupInfo()->wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_CONSOLE | CREATE_SUSPENDED, nullptr, nullptr,
                        startup.StartupInfo(), &pi)) {
        error = Win32ErrorMessage();
        return fail();
    }
    process_ = pi.hProcess;
    processId_ = pi.dwProcessId;
    inputCodePage_ = outputCodePage_ = GetOEMCP();
    manualCodePage_ = knownCodePage_ = false;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) ||
        !AssignProcessToJobObject(job_, process_)) {
        // Win7 nested jobs cannot reliably contain the whole process tree.
        error = L"无法建立 CMD 进程树隔离，请在普通桌面启动程序";
        TerminateProcess(process_, 1);
        CloseHandle(pi.hThread);
        return fail();
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    inRead = outWrite = nullptr;
    data_ = std::move(data);
    status_ = std::move(status);
    connected_ = true;
    stopping_ = false;
    reader_ = std::thread(&CmdConnection::Read, this);
    return true;
}
void CmdConnection::Stop() {
    stopping_ = true;
    connected_ = false;
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (process_)
        TerminateProcess(process_, 0);
    if (reader_.joinable()) {
        CancelSynchronousIo(reinterpret_cast<HANDLE>(reader_.native_handle()));
        reader_.join();
    }
    std::lock_guard<std::mutex> writer(write_);
    for (HANDLE *handle : {&input_, &output_, &process_})
        if (*handle) {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    data_ = {};
    status_ = {};
}
Bytes CmdConnection::PrepareInput(const Bytes &bytes) {
    static_assert(sizeof(wchar_t) == 2, "Win32 UTF-16 console bridge");
    if (bytes.empty())
        return {};
    const auto page = InputCodePage();
    auto flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    int size = MultiByteToWideChar(page, flags, reinterpret_cast<const char *>(bytes.data()),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0 || size > 131072)
        throw std::runtime_error("CMD_ENCODING_INVALID");
    std::wstring text(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(page, flags, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()),
                            text.data(), size) != size)
        throw std::runtime_error("CMD_ENCODING_INVALID");
    const std::uint32_t payload = static_cast<std::uint32_t>(text.size() * sizeof(wchar_t));
    Bytes packet(sizeof(payload) + payload);
    std::memcpy(packet.data(), &payload, sizeof(payload));
    std::memcpy(packet.data() + sizeof(payload), text.data(), payload);
    return packet;
}
bool CmdConnection::Send(const Bytes &bytes, std::wstring &error) {
    try {
        return SendPrepared(PrepareInput(bytes), error);
    } catch (...) {
        error = L"CMD_ENCODING_INVALID";
        return false;
    }
}
bool CmdConnection::SendPrepared(const Bytes &bytes, std::wstring &error) {
    std::lock_guard<std::mutex> lock(write_);
    if (!connected_) {
        error = L"CMD 已结束";
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(input_, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
            !written) {
            error = Win32ErrorMessage();
            return false;
        }
        offset += written;
    }
    return true;
}
void CmdConnection::ReadCodePages() {
    std::lock_guard<std::mutex> lock(consoleQueryMutex);
    if (manualCodePage_)
        return;
    // Query the real hidden Win7 console after chcp. Never detach an existing
    // caller console; users launching from a console can select encoding manually.
    if (GetConsoleWindow() || !processId_ || !AttachConsole(processId_))
        return;
    auto in = GetConsoleCP(), out = GetConsoleOutputCP();
    FreeConsole();
    if (in)
        inputCodePage_ = in;
    if (out)
        outputCodePage_ = out;
    knownCodePage_ = in != 0 && out != 0;
}
void CmdConnection::OverrideTextCodePage(unsigned page) {
    {
        std::lock_guard<std::mutex> lock(consoleQueryMutex);
        manualCodePage_ = page != 0;
        knownCodePage_ = false;
        inputCodePage_ = outputCodePage_ = page ? page : GetOEMCP();
    }
    if (!page)
        ReadCodePages();
}

void CmdConnection::Read() {
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(output_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count)
            break;
        ReadCodePages();
        if (data_)
            data_(Bytes(buffer.begin(), buffer.begin() + count));
    }
    connected_ = false;
    if (!stopping_ && status_)
        status_(L"CMD 已结束", false);
}
} // namespace serialctl
