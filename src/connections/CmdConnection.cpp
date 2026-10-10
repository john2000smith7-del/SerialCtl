#include "CmdConnection.h"
#include "ChildProcess.h"
#include "Win32Helpers.h"
#include <array>
namespace serialctl
{
bool CmdConnection::Start(DataCallback data, StatusCallback status, std::wstring &error)
{
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
    if (!CreatePipe(&inRead, &input_, &sa, 0) || !CreatePipe(&output_, &outWrite, &sa, 0))
    {
        error = Win32ErrorMessage();
        return fail();
    }
    if (!SetHandleInheritable(input_, false, L"CMD stdin", error) ||
        !SetHandleInheritable(output_, false, L"CMD stdout", error))
        return fail();
    RestrictedProcessStartup startup;
    if (!startup.Initialize(inRead, outWrite, outWrite, {}, error))
        return fail();
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring exe = std::wstring(system) + L"\\cmd.exe";
    std::wstring command = L"\"" + exe + L"\" /D /Q /K";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        nullptr, startup.StartupInfo(), &pi))
    {
        error = Win32ErrorMessage();
        return fail();
    }
    process_ = pi.hProcess;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ ||
        !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) ||
        !AssignProcessToJobObject(job_, process_))
    {
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
void CmdConnection::Stop()
{
    stopping_ = true;
    connected_ = false;
    if (job_)
    {
        CloseHandle(job_);
        job_ = nullptr;
    }
    if (process_)
        TerminateProcess(process_, 0);
    if (reader_.joinable())
    {
        CancelSynchronousIo(reinterpret_cast<HANDLE>(reader_.native_handle()));
        reader_.join();
    }
    for (HANDLE *handle : {&input_, &output_, &process_})
        if (*handle)
        {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    data_ = {};
    status_ = {};
}
bool CmdConnection::Send(const Bytes &bytes, std::wstring &error)
{
    std::lock_guard<std::mutex> lock(write_);
    if (!connected_)
    {
        error = L"CMD 已结束";
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        if (!WriteFile(input_, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset),
                       &written, nullptr) ||
            !written)
        {
            error = Win32ErrorMessage();
            return false;
        }
        offset += written;
    }
    return true;
}
void CmdConnection::Read()
{
    std::array<std::uint8_t, 16384> buffer{};
    for (;;)
    {
        DWORD count = 0;
        if (!ReadFile(output_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) ||
            !count)
            break;
        if (data_)
            data_(Bytes(buffer.begin(), buffer.begin() + count));
    }
    connected_ = false;
    if (!stopping_ && status_)
        status_(L"CMD 已结束", false);
}
} // namespace serialctl
