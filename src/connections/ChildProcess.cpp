#include "ChildProcess.h"

#include "Win32Helpers.h"

#include <algorithm>

namespace serialctl {

bool SetHandleInheritable(
    HANDLE handle, bool inheritable, const wchar_t* description, std::wstring& error) {
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        error = std::wstring(description) + L"：无效句柄。";
        return false;
    }
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT,
            inheritable ? HANDLE_FLAG_INHERIT : 0)) {
        const DWORD code = GetLastError();
        error = std::wstring(description) + L"：" + Win32ErrorMessage(code);
        return false;
    }
    return true;
}

RestrictedProcessStartup::RestrictedProcessStartup() {
    startup_.StartupInfo.cb = sizeof(startup_);
    startup_.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
}

RestrictedProcessStartup::~RestrictedProcessStartup() {
    Reset();
}

void RestrictedProcessStartup::Reset() {
    if (startup_.lpAttributeList) {
        DeleteProcThreadAttributeList(startup_.lpAttributeList);
        startup_.lpAttributeList = nullptr;
    }
    if (attributeStorage_) {
        HeapFree(GetProcessHeap(), 0, attributeStorage_);
        attributeStorage_ = nullptr;
    }
    handles_.clear();
}

bool RestrictedProcessStartup::Initialize(HANDLE standardInput, HANDLE standardOutput,
    HANDLE standardError, std::initializer_list<HANDLE> additionalHandles,
    std::wstring& error) {
    Reset();
    startup_ = {};
    startup_.StartupInfo.cb = sizeof(startup_);
    startup_.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_.StartupInfo.hStdInput = standardInput;
    startup_.StartupInfo.hStdOutput = standardOutput;
    startup_.StartupInfo.hStdError = standardError;

    const auto addHandle = [this](HANDLE handle) {
        if (handle && handle != INVALID_HANDLE_VALUE &&
            std::find(handles_.begin(), handles_.end(), handle) == handles_.end()) {
            handles_.push_back(handle);
        }
    };
    addHandle(standardInput);
    addHandle(standardOutput);
    addHandle(standardError);
    for (HANDLE handle : additionalHandles) addHandle(handle);

    if (!standardInput || standardInput == INVALID_HANDLE_VALUE ||
        !standardOutput || standardOutput == INVALID_HANDLE_VALUE ||
        !standardError || standardError == INVALID_HANDLE_VALUE) {
        error = L"启动子进程失败：标准输入输出句柄无效。";
        return false;
    }

    for (HANDLE handle : handles_) {
        DWORD flags = 0;
        if (!GetHandleInformation(handle, &flags)) {
            const DWORD code = GetLastError();
            error = L"检查子进程句柄失败：" + Win32ErrorMessage(code);
            return false;
        }
        if ((flags & HANDLE_FLAG_INHERIT) == 0) {
            error = L"启动子进程失败：允许继承的句柄未标记为可继承。";
            return false;
        }
    }

    SIZE_T attributeBytes = 0;
    SetLastError(ERROR_SUCCESS);
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    const DWORD sizingError = GetLastError();
    if (attributeBytes == 0 || sizingError != ERROR_INSUFFICIENT_BUFFER) {
        error = L"初始化子进程句柄列表失败：" + Win32ErrorMessage(sizingError);
        return false;
    }

    attributeStorage_ = HeapAlloc(GetProcessHeap(), 0, attributeBytes);
    if (!attributeStorage_) {
        error = L"初始化子进程句柄列表失败：内存不足。";
        return false;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST attributeList =
        static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage_);
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        const DWORD code = GetLastError();
        HeapFree(GetProcessHeap(), 0, attributeStorage_);
        attributeStorage_ = nullptr;
        error = L"初始化子进程句柄列表失败：" + Win32ErrorMessage(code);
        return false;
    }
    startup_.lpAttributeList = attributeList;
    if (!UpdateProcThreadAttribute(attributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
            handles_.size() * sizeof(handles_.front()), nullptr, nullptr)) {
        const DWORD code = GetLastError();
        Reset();
        error = L"设置子进程句柄列表失败：" + Win32ErrorMessage(code);
        return false;
    }
    return true;
}

LPSTARTUPINFOW RestrictedProcessStartup::StartupInfo() {
    return &startup_.StartupInfo;
}

} // namespace serialctl
