#pragma once
#include "Connection.h"
#include <objbase.h>
#include <sddl.h>

namespace serialctl {
// One operation per instance. The caller retains the file and cancel event until
// Transfer returns, including cancellation completion. Read and write use distinct
// instances, so an idle read never serializes an interactive write.
class CancellableIo {
public:
    CancellableIo() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~CancellableIo() { if (event_) CloseHandle(event_); }
    CancellableIo(const CancellableIo&) = delete;
    CancellableIo& operator=(const CancellableIo&) = delete;
    bool Transfer(HANDLE file, void* data, DWORD size, bool write, HANDLE cancel, DWORD& count) {
        count = 0;
        if (!event_ || !cancel) { SetLastError(ERROR_INVALID_HANDLE); return false; }
        if (WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) {
            SetLastError(ERROR_OPERATION_ABORTED); return false;
        }
        ResetEvent(event_);
        OVERLAPPED io{};
        io.hEvent = event_;
        const BOOL immediate = write ? WriteFile(file, data, size, &count, &io)
                                     : ReadFile(file, data, size, &count, &io);
        if (immediate) return true;
        if (GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[]{cancel, event_};
        const DWORD signal = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (signal != WAIT_OBJECT_0 + 1) {
            CancelIoEx(file, &io);
            GetOverlappedResult(file, &io, &count, TRUE);
            SetLastError(ERROR_OPERATION_ABORTED);
            return false;
        }
        return GetOverlappedResult(file, &io, &count, FALSE) != FALSE;
    }
private:
    HANDLE event_ = nullptr;
};

// CreatePipe cannot provide overlapped parent handles. The child still gets a
// normal inheritable byte pipe; restrict the named endpoint to this user/SYSTEM.
inline bool CreateChildIoPipe(HANDLE& parent, HANDLE& child, bool parentWrites) {
    parent = child = nullptr;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    const BOOL got = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!got || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return false;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    GUID guid{};
    wchar_t unique[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, unique, 40)) { LocalFree(descriptor); return false; }
    const std::wstring name = L"\\\\.\\pipe\\SerialCtl-IO-" + std::wstring(unique);
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    parent = CreateNamedPipeW(name.c_str(), (parentWrites ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, &sa);
    sa.bInheritHandle = TRUE;
    if (parent != INVALID_HANDLE_VALUE)
        child = CreateFileW(name.c_str(), parentWrites ? GENERIC_READ : GENERIC_WRITE, 0, &sa,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD code = GetLastError();
    LocalFree(descriptor);
    if (parent != INVALID_HANDLE_VALUE && child && child != INVALID_HANDLE_VALUE) return true;
    if (parent && parent != INVALID_HANDLE_VALUE) CloseHandle(parent);
    parent = child = nullptr;
    SetLastError(code);
    return false;
}
} // namespace serialctl
