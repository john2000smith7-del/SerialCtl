#include "PasswordPipe.h"
#include "Win32Helpers.h"
#include <objbase.h>
#include <sddl.h>
#include <vector>

namespace serialctl {
PasswordPipe::~PasswordPipe() {
    if (!password_.empty())
        SecureZeroMemory(password_.data(), password_.size());
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIo(pipe_);
        CloseHandle(pipe_);
    }
    if (event_)
        CloseHandle(event_);
}
bool PasswordPipe::Open(const std::wstring &password, std::wstring &error) {
    if (password.find_first_of(L"\r\n") != std::wstring::npos || password.size() > 1024) {
        error = L"密码不支持换行且不能超过 1024 字符";
        return false;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    const BOOL got = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!got || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid))
        return false;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t unique[40]{};
    StringFromGUID2(guid, unique, 40);
    name_ = L"\\\\.\\pipe\\SerialCtl-AI-" + std::wstring(unique);
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    pipe_ = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 8192, 8192, 0, &sa);
    LocalFree(descriptor);
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE || !event_) {
        error = L"创建安全密码管道失败：" + Win32ErrorMessage();
        return false;
    }
    password_ = WideToMultiByte(password, CP_UTF8) + "\n";
    connect_.hEvent = event_;
    if (ConnectNamedPipe(pipe_, &connect_))
        pending_ = false;
    else if (GetLastError() == ERROR_IO_PENDING)
        pending_ = true;
    else if (GetLastError() != ERROR_PIPE_CONNECTED) {
        error = L"等待安全密码管道失败";
        return false;
    }
    return true;
}
void PasswordPipe::Poll() {
    if (password_.empty() || pipe_ == INVALID_HANDLE_VALUE)
        return;
    if (pending_) {
        DWORD ignored = 0;
        if (!GetOverlappedResult(pipe_, &connect_, &ignored, FALSE))
            return;
        pending_ = false;
    }
    // A password is at most 4 KiB, below the pipe buffer size. The write is
    // bounded; never FlushFileBuffers (a child may abort before reading).
    OVERLAPPED write{};
    write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    if (!WriteFile(pipe_, password_.data(), static_cast<DWORD>(password_.size()), &written, &write) &&
        GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(write.hEvent, 500) != WAIT_OBJECT_0)
            CancelIo(pipe_);
        GetOverlappedResult(pipe_, &write, &written, TRUE);
    }
    CloseHandle(write.hEvent);
    SecureZeroMemory(password_.data(), password_.size());
    password_.clear();
    // Keep the server end open until process cleanup so fopen/fgetline sees the
    // complete newline; no other client can connect to this instance.
}
} // namespace serialctl
