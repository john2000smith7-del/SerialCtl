#pragma once

#include "TftpServer.h"
#include "UiStyle.h"
#include <functional>
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
        TftpNameLabel,
        TftpName,
        PublishBtn,
        UnpublishBtn,

        PublishedLabel,
        PublishedList,
        TransfersLabel,
        TransfersStatus,
        CancelTransferBtn
    };

    ~TftpPane();

    bool Create(HWND parent, std::shared_ptr<TftpServer> server,
                SshSessionProvider sshProvider, HFONT font, bool dark, int dpi);
    HWND Handle() const { return window_; }
    void Theme(bool dark, int dpi, HFONT font);
    void Refresh();

    std::function<void()> stateChanged;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BodyProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FieldProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND Control(int id) const;
    void Layout();
    void Scroll(int amount);
    void BrowseLocalFile();
    void OnToggleService();
    void OnPublish();
    void OnUnpublish();
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
    std::wstring localNotice_;
};

} // namespace serialctl
