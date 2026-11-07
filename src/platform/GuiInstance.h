#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace serialctl {
// Exactly one normal GUI instance per entire Windows machine (Global mutex).
// Within the same session, activating the existing instance is requested.
// Across different sessions / RDP sessions, informs the user and cleanly exits.
class GuiInstance {
  public:
    enum class Result { Primary, Existing, CrossSessionExisting, Failed };
    explicit GuiInstance(const std::wstring &customPrefix = L"", const std::wstring &customSessionSuffix = L"")
        : prefix_(customPrefix), sessionSuffix_(customSessionSuffix) {
        if (prefix_.empty()) {
            wchar_t env[256]{};
            if (GetEnvironmentVariableW(L"SERIALCTL_GUI_INSTANCE_PREFIX", env, 256) > 0) {
                prefix_ = env;
            }
        }
    }
    GuiInstance(const GuiInstance &) = delete;
    GuiInstance &operator=(const GuiInstance &) = delete;
    ~GuiInstance() {
        if (window_ && IsWindow(window_)) RemovePropW(window_, name_.c_str());
        if (activation_) CloseHandle(activation_);
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
        if (globalOwned_) ReleaseMutex(globalMutex_);
        if (globalMutex_) CloseHandle(globalMutex_);
    }
    Result Begin() {
        std::wstring sidStr = sessionSuffix_;
        if (sidStr.empty()) {
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
            sidStr = sid;
            LocalFree(sid);
        }

        globalName_ = L"Global\\" + prefix_ + L"SerialCtl.Gui.v1";
        name_ = L"Local\\" + prefix_ + L"SerialCtl.Gui.v1." + sidStr;

        PSECURITY_DESCRIPTOR pSd = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)",
                SDDL_REVISION_1, &pSd, nullptr)) {
            sa.lpSecurityDescriptor = pSd;
        }

        globalMutex_ = CreateMutexW(&sa, TRUE, globalName_.c_str());
        DWORD globalErr = GetLastError();
        if (pSd) LocalFree(pSd);

        if (!globalMutex_ && globalErr == ERROR_ACCESS_DENIED) {
            globalMutex_ = OpenMutexW(SYNCHRONIZE, FALSE, globalName_.c_str());
            if (globalMutex_) globalErr = ERROR_ALREADY_EXISTS;
        }

        if (!globalMutex_) return Result::Failed;

        if (globalErr != ERROR_ALREADY_EXISTS) {
            globalOwned_ = true;
        } else {
            const DWORD wait = WaitForSingleObject(globalMutex_, 0);
            globalOwned_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
            if (wait == WAIT_FAILED) return Result::Failed;
        }

        mutex_ = CreateMutexW(nullptr, TRUE, name_.c_str());
        if (!mutex_) return Result::Failed;
        const bool localExisted = (GetLastError() == ERROR_ALREADY_EXISTS);
        if (!localExisted) {
            owned_ = true;
        } else {
            const DWORD wait = WaitForSingleObject(mutex_, 0);
            owned_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
            if (wait == WAIT_FAILED) return Result::Failed;
        }

        activation_ = CreateEventW(nullptr, FALSE, FALSE, (name_ + L".activate").c_str());
        if (!activation_) return Result::Failed;
        const bool activationExisted = (GetLastError() == ERROR_ALREADY_EXISTS);

        if (globalOwned_ && owned_) return Result::Primary;

        struct EnumContext {
            const std::wstring *name;
            bool found;
        } ctx{&name_, false};

        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto *ctx = reinterpret_cast<EnumContext *>(l);
            if (!GetPropW(h, ctx->name->c_str())) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            ctx->found = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.found || localExisted || activationExisted) {
            SetEvent(activation_);
            return Result::Existing;
        }

        if (owned_) {
            ReleaseMutex(mutex_);
            owned_ = false;
        }
        if (mutex_) {
            CloseHandle(mutex_);
            mutex_ = nullptr;
        }
        if (activation_) {
            CloseHandle(activation_);
            activation_ = nullptr;
        }
        return Result::CrossSessionExisting;
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
    HANDLE globalMutex_ = nullptr, mutex_ = nullptr, activation_ = nullptr;
    HWND window_ = nullptr;
    bool globalOwned_ = false, owned_ = false;
    std::wstring prefix_, sessionSuffix_, globalName_, name_;
};
} // namespace serialctl
