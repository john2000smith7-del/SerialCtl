#pragma once

#include "TftpServer.h"
#include "UiStyle.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace serialctl {

class TftpPane {
public:
    enum ControlId {
        BindIpLabel = 100,
        BindIp,
        ToggleService,

        LocalPathLabel,
        LocalPath,
        BrowseLocal,
        RefreshDirBtn,

        DirFilesLabel,
        DirFilesList
    };

    ~TftpPane();

    bool Create(HWND parent, std::shared_ptr<TftpServer> server, HFONT font, bool dark, int dpi);
    HWND Handle() const { return window_; }
    void Theme(bool dark, int dpi, HFONT font);
    void Refresh();

    void ConfigureDirectory(const std::wstring& localDirectory);
    std::wstring LocalDirectory() const;
    void ConfigureBindIp(const std::wstring& bindIp, bool explicitChoice = true);
    std::wstring SelectedBindIp() const;
    bool IsBindIpExplicit() const { return bindIpExplicit_; }

    std::function<void()> stateChanged;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BodyProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FieldProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK DirListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND Control(int id) const;
    void PopulateBindIpCombo(bool preserveSelection);
    void Layout();
    void Scroll(int amount);
    void BrowseLocalDirectory();
    void ApplyLocalDirectory(bool showError, bool persistState);
    void OnLocalPathTyped();
    void RefreshDirectory(bool userTriggered);
    void OnToggleService();
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

    std::wstring localDir_;
    std::vector<TftpDirectoryEntry> dirEntries_;
    std::wstring dirListNotice_;
    std::wstring cachedDirListSignature_;
    RECT dirListFrame_{};
    bool dirListDragging_ = false;
    int dirListDragOffset_ = 0;
    std::wstring cachedToggleTip_;
    bool suppressEditNotify_ = false;
    bool bindIpExplicit_ = false;
    std::vector<std::string> cachedLocalIps_;
};

} // namespace serialctl
