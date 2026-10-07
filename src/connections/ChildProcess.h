#pragma once

#include <windows.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace serialctl {

// Changes only HANDLE_FLAG_INHERIT and reports failures with enough context for
// callers to clean up before attempting to create a child process.
bool SetHandleInheritable(
    HANDLE handle, bool inheritable, const wchar_t* description, std::wstring& error);

// STARTUPINFOEXW wrapper which restricts inheritance to an explicit allowlist.
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST is available on Windows 7 and prevents a
// concurrently-created child from keeping another child's pipe ends alive.
class RestrictedProcessStartup final {
public:
    RestrictedProcessStartup();
    ~RestrictedProcessStartup();

    RestrictedProcessStartup(const RestrictedProcessStartup&) = delete;
    RestrictedProcessStartup& operator=(const RestrictedProcessStartup&) = delete;

    bool Initialize(HANDLE standardInput, HANDLE standardOutput, HANDLE standardError,
        std::initializer_list<HANDLE> additionalHandles, std::wstring& error);

    LPSTARTUPINFOW StartupInfo();

private:
    void Reset();

    STARTUPINFOEXW startup_{};
    void* attributeStorage_ = nullptr;
    // UpdateProcThreadAttribute requires the supplied value buffer to remain
    // valid through CreateProcess, so keep the allowlist with this wrapper.
    std::vector<HANDLE> handles_;
};

} // namespace serialctl
