#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace serialctl {
// One normal GUI per Windows user/session, independent of installation path.
// Internal CMD bridge and authenticated updater modes run before this guard.
class GuiInstance {
  public:
    enum class Result { Primary, Existing, Failed };
    ~GuiInstance() {
        if (window_ && IsWindow(window_)) RemovePropW(window_, name_.c_str());
        if (activation_) CloseHandle(activation_);
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    Result Begin() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return Result::Failed;
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> data(bytes);
        const bool queried = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != FALSE;
        CloseHandle(token);
        if (!queried) return Result::Failed;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid)) return Result::Failed;
        name_ = L"Local\\SerialCtl.Gui.v1." + std::wstring(sid);
        LocalFree(sid);
        mutex_ = CreateMutexW(nullptr, TRUE, name_.c_str());
        if (!mutex_) return Result::Failed;
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        if (!existed) owned_ = true;
        else {
            const DWORD wait = WaitForSingleObject(mutex_, 0);
            owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
            if (wait == WAIT_FAILED) return Result::Failed;
        }
        activation_ = CreateEventW(nullptr, FALSE, FALSE, (name_ + L".activate").c_str());
        if (!activation_) return Result::Failed;
        if (owned_) return Result::Primary;
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto self = reinterpret_cast<GuiInstance *>(l);
            if (!GetPropW(h, self->name_.c_str())) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            return FALSE;
        }, reinterpret_cast<LPARAM>(this));
        SetEvent(activation_); // Also handles a first instance still starting.
        return Result::Existing;
    }
    void Publish(HWND window) { window_ = window; SetPropW(window, name_.c_str(), reinterpret_cast<HANDLE>(1)); }
    HANDLE Activation() const { return activation_; }
    void Activate() const {
        if (!window_ || !IsWindow(window_)) return;
        ShowWindowAsync(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        const HWND popup = GetLastActivePopup(window_);
        SetForegroundWindow(popup && IsWindowVisible(popup) ? popup : window_);
    }
  private:
    HANDLE mutex_ = nullptr, activation_ = nullptr;
    HWND window_ = nullptr;
    bool owned_ = false;
    std::wstring name_;
};
} // namespace serialctl
