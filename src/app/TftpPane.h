#pragma once

#include "TftpServer.h"
#include "UiStyle.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace serialctl {

struct TftpSshSessionInfo {
    std::string sessionId;
    std::wstring displayName;
    std::wstring host;
    uint16_t port = 22;
    std::wstring username;
    std::wstring password;
    bool connected = false;
};

class TftpPane {
public:
    using SshSessionProvider = std::function<std::vector<TftpSshSessionInfo>()>;

    enum ControlId {
        ServiceStatus = 100,
        BindIpLabel,
        BindIp,
        PortLabel,
        Port,
        ToggleService,
        BoardTip,

        SourceTypeLabel,
        SourceType,
        LocalPathLabel,
        LocalPath,
        BrowseLocal,
        RemoteSessionLabel,
        RemoteSession,
        RemotePathLabel,
        RemotePath,
        RefreshDirBtn,

        DirFilesLabel,
        DirFilesList,
        TransfersLabel,
        TransferCount,
        TransferDetail,
        CancelTransferBtn
    };

    struct AsyncRemoteShared {
        std::mutex mutex;
        HWND hwnd = nullptr;
    };

    ~TftpPane();

    bool Create(HWND parent, std::shared_ptr<TftpServer> server,
                SshSessionProvider sshProvider, HFONT font, bool dark, int dpi);
    HWND Handle() const { return window_; }
    void Theme(bool dark, int dpi, HFONT font);
    void Refresh();
    void ConfigureDirectories(const std::wstring& rootMode,
                              const std::wstring& localDirectory,
                              const std::wstring& remoteDirectory);
    std::wstring RootMode() const;
    std::wstring LocalDirectory() const;
    std::wstring RemoteDirectory() const;

    std::function<void()> stateChanged;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BodyProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FieldProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND Control(int id) const;
    void Layout();
    void Scroll(int amount);
    void InvalidateProgress();
    void BrowseLocalDirectory();
    void ApplySelectedSource(bool userTriggered);
    void ApplyLocalDirectory(bool showError, bool persistState);
    void OnLocalPathTyped();
    void ApplyRemoteDirectory(bool allowAsyncFetch, bool persistState);
    void OnRemoteDirDone(LPARAM lParam);
    void RefreshDirectory(bool userTriggered);
    void OnToggleService();
    void OnCancelTransfer();
    void Paint(HDC dc);

    int D(int n) const { return MulDiv(n, dpi_, 96); }

    HWND window_ = nullptr;
    HWND body_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH surface_ = nullptr;
    HBRUSH field_ = nullptr;
    bool dark_ = false;
    int dpi_ = 96;
    int offset_ = 0;
    int contentHeight_ = 0;
    int bodyHeight_ = 0;

    UiFieldStyle style_{};
    std::shared_ptr<TftpServer> server_;
    SshSessionProvider sshProvider_;
    std::vector<TftpSshSessionInfo> cachedSshSessions_;

    int selectedSourceType_ = 0; // 0 = Local, 1 = Remote SSH
    std::string selectedRemoteSessionId_;
    std::wstring localDir_;
    std::wstring remoteDir_;
    std::vector<TftpDirectoryEntry> dirEntries_;
    std::wstring dirListNotice_;
    std::wstring localNotice_;
    bool suppressEditNotify_ = false;

    std::shared_ptr<AsyncRemoteShared> remoteListShared_;
    std::atomic<uint64_t> remoteListSeq_{0};
    bool remoteListingPending_ = false;
    std::string lastAutoQueriedKey_;

    int progressLeft_ = 0;
    int progressTop_ = -1;
    int progressRight_ = 0;
    uint64_t displayBytes_ = 0;
    uint64_t displayTotal_ = 0;
    bool transferActive_ = false;
    bool transferHasError_ = false;
    uint64_t lastProgressBytes_ = 0;
    uint64_t lastProgressTotal_ = 0;
    bool lastProgressActive_ = false;
};

} // namespace serialctl
