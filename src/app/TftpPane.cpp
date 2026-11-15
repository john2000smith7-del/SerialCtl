#include "TftpPane.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <thread>

namespace serialctl {

namespace {
constexpr UINT kMsgRemoteDirDone = WM_APP + 10;

struct AsyncRemoteListingResult {
    uint64_t requestId = 0;
    std::wstring remoteDir;
    std::string sessionId;
    SshRemoteConfig config;
    bool ok = false;
    std::vector<TftpDirectoryEntry> entries;
    std::wstring error;
};

std::wstring TrimWhitespace(const std::wstring& value) {
    const size_t start = value.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::wstring FormatByteSize(uint64_t bytes) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes < 1024ULL) {
        return std::to_wstring(bytes) + L" B";
    }
    if (bytes < 1024ULL * 1024ULL) {
        oss << (static_cast<double>(bytes) / 1024.0) << L" KB";
        return oss.str();
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        oss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MB";
        return oss.str();
    }
    oss << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
    return oss.str();
}

int MeasureText(HWND parent, HFONT font, const std::wstring& text, int width) {
    if (text.empty() || width <= 0) return 0;
    HDC dc = GetDC(parent);
    HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, font));
    RECT rc{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(parent, dc);
    return rc.bottom - rc.top;
}

std::wstring ControlText(HWND h) {
    if (!h) return {};
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::vector<wchar_t> buf(len + 1);
    GetWindowTextW(h, buf.data(), len + 1);
    return buf.data();
}

bool SetTextIfChanged(HWND h, const std::wstring& value) {
    if (!h || ControlText(h) == value) return false;
    SetWindowTextW(h, value.c_str());
    return true;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), sz, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), sz);
    return w;
}

template <typename T>
struct ComReleaser {
    T* ptr = nullptr;
    ~ComReleaser() {
        if (ptr) ptr->Release();
    }
    ComReleaser() = default;
    ComReleaser(const ComReleaser&) = delete;
    ComReleaser& operator=(const ComReleaser&) = delete;
};

std::wstring PickFolderDialog(HWND owner, const std::wstring& initialDir) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninit = (coHr == S_OK || coHr == S_FALSE);
    struct CoGuard {
        bool uninit = false;
        ~CoGuard() {
            if (uninit) CoUninitialize();
        }
    } coGuard{shouldUninit};

    ComReleaser<IFileOpenDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IFileOpenDialog), reinterpret_cast<void**>(&dialog.ptr))) && dialog.ptr) {
        DWORD opts = 0;
        if (SUCCEEDED(dialog.ptr->GetOptions(&opts))) {
            dialog.ptr->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        }
        dialog.ptr->SetTitle(L"选择 TFTP 根目录");
        const std::wstring trimmed = TrimWhitespace(initialDir);
        if (!trimmed.empty()) {
            ComReleaser<IShellItem> folderItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(trimmed.c_str(), nullptr,
                                                     __uuidof(IShellItem),
                                                     reinterpret_cast<void**>(&folderItem.ptr))) && folderItem.ptr) {
                dialog.ptr->SetFolder(folderItem.ptr);
            }
        }
        const HRESULT showHr = dialog.ptr->Show(owner);
        if (SUCCEEDED(showHr)) {
            ComReleaser<IShellItem> result;
            if (SUCCEEDED(dialog.ptr->GetResult(&result.ptr)) && result.ptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(result.ptr->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    std::wstring selected(path);
                    CoTaskMemFree(path);
                    return selected;
                }
            }
        }
        return {};
    }

    wchar_t buffer[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择 TFTP 根目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    const bool got = SHGetPathFromIDListW(pidl, buffer) != FALSE;
    CoTaskMemFree(pidl);
    return got ? std::wstring(buffer) : std::wstring{};
}
} // namespace

TftpPane::~TftpPane() {
    BeginShutdown();
    if (window_) DestroyWindow(window_);
    WaitForBackground();
    if (surface_) DeleteObject(surface_);
    if (field_) DeleteObject(field_);
    if (monoFont_) DeleteObject(monoFont_);
}

void TftpPane::BeginShutdown() {
    remoteListSeq_.fetch_add(1);
    if (remoteListShared_) {
        std::lock_guard<std::mutex> lock(remoteListShared_->mutex);
        remoteListShared_->cancel.store(true);
        remoteListShared_->hwnd = nullptr;
    }
    if (window_) {
        MSG pending{};
        while (PeekMessageW(&pending, window_, kMsgRemoteDirDone, kMsgRemoteDirDone, PM_REMOVE)) {
            delete reinterpret_cast<AsyncRemoteListingResult*>(pending.lParam);
        }
    }
}

void TftpPane::WaitForBackground() {
    remoteListWorkers_.Drain();
}

HWND TftpPane::Control(int id) const {
    return GetDlgItem(body_ ? body_ : window_, id);
}

bool TftpPane::Create(HWND parent, std::shared_ptr<TftpServer> server,
                      SshSessionProvider sshProvider, HFONT font, bool dark, int dpi) {
    server_ = std::move(server);
    sshProvider_ = std::move(sshProvider);
    font_ = font;
    dark_ = dark;
    dpi_ = dpi;
    remoteListShared_ = std::make_shared<AsyncRemoteShared>();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SerialCtlTftpPane";
    RegisterClassExW(&wc);

    WNDCLASSEXW bc{sizeof(bc)};
    bc.lpfnWndProc = BodyProc;
    bc.hInstance = inst;
    bc.lpszClassName = L"SerialCtlTftpBody";
    RegisterClassExW(&bc);

    window_ = CreateWindowExW(0, L"SerialCtlTftpPane", L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 100, parent, nullptr, inst, this);
    if (!window_) return false;
    {
        std::lock_guard<std::mutex> lock(remoteListShared_->mutex);
        remoteListShared_->hwnd = window_;
    }

    body_ = CreateWindowExW(0, L"SerialCtlTftpBody", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100, window_, nullptr, inst, this);
    if (!body_) return false;

    auto add = [this, inst](int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                                 0, 0, 10, 10, body_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (_wcsicmp(cls, L"BUTTON") == 0) UiStyleButton(h);
        return h;
    };

    // Service controls
    auto serviceStatus = add(ServiceStatus, L"STATIC", L"服务状态：未启动", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL | SS_NOTIFY);
    UiSetControlTooltip(serviceStatus, L"服务状态：未启动");
    add(BindIpLabel, L"STATIC", L"监听网卡 / IP:", SS_LEFT | SS_NOPREFIX);
    add(BindIp, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    add(PortLabel, L"STATIC", L"端口:", SS_LEFT | SS_NOPREFIX);
    add(Port, L"EDIT", L"69", ES_LEFT | ES_NUMBER | WS_TABSTOP);
    add(ToggleService, L"BUTTON", L"启动服务", BS_OWNERDRAW | WS_TABSTOP);
    add(BoardTip, L"STATIC", L"开发板设置参考：\r\nsetenv serverip <调试机IP>\r\ntftpboot ${loadaddr} <文件名>", SS_LEFT | SS_NOPREFIX);

    // Directory publishing controls
    add(SourceTypeLabel, L"STATIC", L"文件来源类别:", SS_LEFT | SS_NOPREFIX);
    auto srcCombo = add(SourceType, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    SendMessageW(srcCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"本机目录"));
    SendMessageW(srcCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"远程 SSH 目录"));
    SendMessageW(srcCombo, CB_SETCURSEL, 0, 0);

    add(LocalPathLabel, L"STATIC", L"本地 TFTP 目录:", SS_LEFT | SS_NOPREFIX);
    HWND localEdit = add(LocalPath, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);
    SetWindowSubclass(localEdit, FieldProc, LocalPath, reinterpret_cast<DWORD_PTR>(this));
    auto browseBtn = add(BrowseLocal, L"BUTTON", L"选择目录", BS_OWNERDRAW | WS_TABSTOP);
    UiSetFlatIconButton(browseBtn);
    UiSetButtonGlyph(browseBtn, UiGlyph::Folder, L"选择本地 TFTP 目录");

    add(RemoteSessionLabel, L"STATIC", L"已配置的 SSH 连接:", SS_LEFT | SS_NOPREFIX);
    add(RemoteSession, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    add(RemotePathLabel, L"STATIC", L"远程 TFTP 目录 (POSIX):", SS_LEFT | SS_NOPREFIX);
    HWND remoteEdit = add(RemotePath, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);
    SetWindowSubclass(remoteEdit, FieldProc, RemotePath, reinterpret_cast<DWORD_PTR>(this));

    auto refreshBtn = add(RefreshDirBtn, L"BUTTON", L"刷新", BS_OWNERDRAW | WS_TABSTOP);
    UiSetFlatIconButton(refreshBtn);
    UiSetButtonGlyph(refreshBtn, UiGlyph::Refresh, L"刷新目录文件列表");

    // Directory file list & Transfer progress
    dirListNotice_ = L"(请选择或输入目录后自动列出文件)";
    add(DirFilesLabel, L"STATIC", L"目录文件列表 (已排除子目录):", SS_LEFT | SS_NOPREFIX);
    add(DirFilesList, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP);
    SetWindowSubclass(Control(DirFilesList), DirListProc, DirFilesList, reinterpret_cast<DWORD_PTR>(this));
    add(TransfersLabel, L"STATIC", L"实时传输任务:", SS_LEFT | SS_NOPREFIX);
    add(TransferCount, L"STATIC", L"等待传输", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    auto transferDetail = add(TransferDetail, L"STATIC", L"(无活动传输)", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL | SS_NOTIFY);
    UiSetControlTooltip(transferDetail, L"(无活动传输)");
    add(CancelTransferBtn, L"BUTTON", L"取消当前传输", BS_OWNERDRAW | WS_TABSTOP);

    Theme(dark_, dpi_, font_);
    Refresh();
    return true;
}

void TftpPane::Theme(bool dark, int dpi, HFONT font) {
    dark_ = dark;
    dpi_ = dpi;
    font_ = font;
    style_ = {&dark_, font_, dpi_, false};

    if (surface_) DeleteObject(surface_);
    if (field_) DeleteObject(field_);
    if (monoFont_) { DeleteObject(monoFont_); monoFont_ = nullptr; }
    monoFont_ = CreateFontW(-D(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    const auto c = UiTheme(dark_);
    surface_ = CreateSolidBrush(c.surface);
    field_ = CreateSolidBrush(c.field);

    if (!window_) return;

    for (int id = ServiceStatus; id <= CancelTransferBtn; ++id) {
        HWND h = Control(id);
        if (!h) continue;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(id == TransferCount && monoFont_ ? monoFont_ : font_), FALSE);
        wchar_t cls[32]{};
        GetClassNameW(h, cls, 32);
        if (_wcsicmp(cls, L"EDIT") == 0 || _wcsicmp(cls, L"COMBOBOX") == 0) {
            UiStyleField(h, &style_);
            SetPropW(h, L"SerialCtl.FieldSurface", reinterpret_cast<HANDLE>(1));
        }
    }

    Layout();
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void TftpPane::Layout() {
    if (!body_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max(1L, client.right);
    bodyHeight_ = std::max(1L, client.bottom);
    UiLayoutBatch viewport(true);
    viewport.Move(body_, 0, 0, width, bodyHeight_);
    viewport.Commit();

    const int lane = D(16);
    const int contentWidth = std::max(D(80), width - lane);
    const int hField = D(28);
    const int hLabel = D(20);
    const int gap = D(8);
    const int btnWidth = D(28);
    const int btnGap = D(8);

    const bool isRemote = (selectedSourceType_ == 1);

    UiLayoutBatch layout(true);
    for (int id : {LocalPathLabel, LocalPath, BrowseLocal})
        layout.Show(Control(id), isRemote ? SW_HIDE : SW_SHOW);
    for (int id : {RemoteSessionLabel, RemoteSession, RemotePathLabel, RemotePath})
        layout.Show(Control(id), isRemote ? SW_SHOW : SW_HIDE);

    struct FieldPosition { int id, x, y, width, height, dropHeight; };
    std::vector<FieldPosition> positions;
    int y = D(8);
    auto place = [&](int id, int h, int w = -1, int dropHeight = 0) {
        if (w < 0) w = contentWidth;
        positions.push_back({id, 0, y, w, h, dropHeight});
        y += h + gap;
    };

    // Service area
    place(ServiceStatus, D(40));
    place(BindIpLabel, hLabel);
    place(BindIp, hField, -1, D(150));

    place(PortLabel, hLabel);
    place(Port, hField, D(92));
    place(ToggleService, D(32), contentWidth);

    int boardTipHeight = std::max(D(36), MeasureText(body_, font_, ControlText(Control(BoardTip)), contentWidth));
    place(BoardTip, boardTipHeight);
    y += gap;

    // Directory selection area
    place(SourceTypeLabel, hLabel);
    place(SourceType, hField, -1, D(120));

    if (!isRemote) {
        place(LocalPathLabel, hLabel);
        const int editW = std::max(D(60), contentWidth - btnWidth * 2 - btnGap * 2);
        positions.push_back({LocalPath, 0, y, editW, hField, 0});
        positions.push_back({BrowseLocal, editW + btnGap, y, btnWidth, hField, 0});
        positions.push_back({RefreshDirBtn, editW + btnGap * 2 + btnWidth, y, btnWidth, hField, 0});
        y += hField + gap;
    } else {
        place(RemoteSessionLabel, hLabel);
        place(RemoteSession, hField, -1, D(120));

        place(RemotePathLabel, hLabel);
        const int editW = std::max(D(60), contentWidth - btnWidth - btnGap);
        positions.push_back({RemotePath, 0, y, editW, hField, 0});
        positions.push_back({RefreshDirBtn, editW + btnGap, y, btnWidth, hField, 0});
        y += hField + gap;
    }
    y += gap;

    // Directory files and transfer progress
    place(DirFilesLabel, hLabel);
    place(DirFilesList, D(108));
    y += gap;

    place(TransfersLabel, hLabel);
    progressLeft_ = 0;
    progressRight_ = contentWidth;
    const int progressLogicalY = y;
    y += D(6) + gap;
    place(TransferCount, hLabel);
    place(TransferDetail, D(36));
    place(CancelTransferBtn, D(32));

    contentHeight_ = y + D(20);
    offset_ = std::clamp(offset_, 0, std::max(0, contentHeight_ - bodyHeight_));
    progressTop_ = progressLogicalY - offset_;

    dirListFrame_ = {};
    for (const auto& field : positions) {
        const auto control = Control(field.id);
        if (field.id == DirFilesList) {
            dirListFrame_ = {field.x, field.y - offset_, field.x + std::max(1, field.width),
                             field.y - offset_ + field.height};
            const int inset = D(2);
            layout.Move(control, dirListFrame_.left + inset, dirListFrame_.top + inset,
                        std::max(1, static_cast<int>(dirListFrame_.right - dirListFrame_.left) - inset * 2),
                        std::max(1, static_cast<int>(dirListFrame_.bottom - dirListFrame_.top) - inset * 2));
        } else {
            layout.Move(control, field.x, field.y - offset_, std::max(1, field.width),
                        field.dropHeight ? field.dropHeight : field.height);
        }
        if (field.dropHeight) layout.ComboVisibleHeight(control, field.height);
    }
    layout.Commit();
    RedrawWindow(body_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void TftpPane::Scroll(int amount) {
    offset_ = std::clamp(offset_ + amount, 0, std::max(0, contentHeight_ - bodyHeight_));
    Layout();
    InvalidateRect(body_, nullptr, FALSE);
}

void TftpPane::InvalidateProgress() {
    if (!body_ || progressTop_ < 0 || progressRight_ <= progressLeft_) return;
    RECT rc{progressLeft_, progressTop_, progressRight_, progressTop_ + D(6)};
    InvalidateRect(body_, &rc, FALSE);
}

void TftpPane::ConfigureDirectories(const std::wstring& rootMode,
                                      const std::wstring& localDirectory,
                                      const std::wstring& remoteDirectory) {
    localDir_ = localDirectory;
    remoteDir_ = remoteDirectory;
    selectedSourceType_ = (rootMode == L"remote") ? 1 : 0;

    if (window_) {
        suppressEditNotify_ = true;
        SetTextIfChanged(Control(LocalPath), localDir_);
        SetTextIfChanged(Control(RemotePath), remoteDir_);
        SendMessageW(Control(SourceType), CB_SETCURSEL, selectedSourceType_, 0);
        suppressEditNotify_ = false;
        Layout();
    }

    if (selectedSourceType_ == 0) {
        ApplyLocalDirectory(false, false);
    } else {
        ApplyRemoteDirectory(false, false);
    }
}

std::wstring TftpPane::RootMode() const {
    return (selectedSourceType_ == 1) ? L"remote" : L"local";
}

std::wstring TftpPane::LocalDirectory() const {
    HWND edit = Control(LocalPath);
    return edit ? TrimWhitespace(ControlText(edit)) : TrimWhitespace(localDir_);
}

std::wstring TftpPane::RemoteDirectory() const {
    HWND edit = Control(RemotePath);
    return edit ? TrimWhitespace(ControlText(edit)) : TrimWhitespace(remoteDir_);
}

void TftpPane::Refresh() {
    if (!window_ || !server_) return;
    if (SendMessageW(Control(BindIp), CB_GETDROPPEDSTATE, 0, 0) ||
        SendMessageW(Control(SourceType), CB_GETDROPPEDSTATE, 0, 0) ||
        SendMessageW(Control(RemoteSession), CB_GETDROPPEDSTATE, 0, 0)) {
        return;
    }

    // 1. Service state
    bool listening = server_->IsListening();
    const auto listeningIp = listening ? Utf8ToWide(server_->BindIp()) : std::wstring{};
    std::string state = server_->State();
    std::wstring statusText = L"服务状态：";
    if (state == "transferring") {
        statusText += L"正在传输 · " + std::to_wstring(server_->ActiveTransferCount()) + L" 个活动任务";
    } else if (listening) {
        statusText += L"监听中 (" + listeningIp + L":" + std::to_wstring(server_->Port()) + L")";
    } else {
        statusText += L"未启动";
    }
    if (!localNotice_.empty()) {
        statusText += L" [" + localNotice_ + L"]";
    }
    if (SetTextIfChanged(Control(ServiceStatus), statusText)) {
        UiSetControlTooltip(Control(ServiceStatus), statusText.c_str());
    }

    SetTextIfChanged(Control(ToggleService), listening ? L"停止服务" : L"启动服务");
    EnableWindow(Control(BindIp), !listening);
    EnableWindow(Control(Port), !listening);

    // Populate BindIp combo if empty
    HWND ipCombo = Control(BindIp);
    if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) == 0) {
        SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.0.0.0 (所有可用网卡)"));
        auto ips = server_->LocalIps();
        for (const auto& ip : ips) {
            std::wstring wip = Utf8ToWide(ip);
            SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wip.c_str()));
        }
        SendMessageW(ipCombo, CB_SETCURSEL, 0, 0);
    }
    if (listening) {
        LRESULT index = 0;
        if (listeningIp != L"0.0.0.0") {
            const auto address = reinterpret_cast<LPARAM>(listeningIp.c_str());
            index = SendMessageW(ipCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), address);
            if (index == CB_ERR) index = SendMessageW(ipCombo, CB_ADDSTRING, 0, address);
        }
        if (index >= 0 && SendMessageW(ipCombo, CB_GETCURSEL, 0, 0) != index)
            SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
        SetTextIfChanged(Control(Port), std::to_wstring(server_->Port()));
    }

    // 2. Populate SSH sessions combo (preserving selectedRemoteSessionId_ across status changes)
    if (sshProvider_) {
        cachedSshSessions_ = sshProvider_();
        HWND sessCombo = Control(RemoteSession);
        std::vector<std::wstring> desiredLabels;
        desiredLabels.reserve(cachedSshSessions_.size());
        int matchIdx = -1;
        int firstConnectedIdx = -1;
        for (size_t i = 0; i < cachedSshSessions_.size(); ++i) {
            const auto& s = cachedSshSessions_[i];
            desiredLabels.push_back(s.displayName + (s.connected ? L" (已连接)" : L" (未连接)"));
            if (!selectedRemoteSessionId_.empty() && s.sessionId == selectedRemoteSessionId_) {
                matchIdx = static_cast<int>(i);
            }
            if (firstConnectedIdx < 0 && s.connected) {
                firstConnectedIdx = static_cast<int>(i);
            }
        }
        if (matchIdx < 0 && !cachedSshSessions_.empty()) {
            matchIdx = (firstConnectedIdx >= 0) ? firstConnectedIdx : 0;
            selectedRemoteSessionId_ = cachedSshSessions_[matchIdx].sessionId;
        } else if (cachedSshSessions_.empty()) {
            selectedRemoteSessionId_.clear();
        }

        bool comboChanged = (SendMessageW(sessCombo, CB_GETCOUNT, 0, 0) != static_cast<LRESULT>(desiredLabels.size()));
        if (!comboChanged) {
            for (size_t i = 0; i < desiredLabels.size(); ++i) {
                const LRESULT len = SendMessageW(sessCombo, CB_GETLBTEXTLEN, i, 0);
                if (len < 0 || static_cast<size_t>(len) != desiredLabels[i].size()) {
                    comboChanged = true;
                    break;
                }
                std::wstring existing(static_cast<size_t>(len) + 1, L'\0');
                SendMessageW(sessCombo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(existing.data()));
                existing.resize(static_cast<size_t>(len));
                if (existing != desiredLabels[i]) {
                    comboChanged = true;
                    break;
                }
            }
        }
        if (comboChanged) {
            SendMessageW(sessCombo, CB_RESETCONTENT, 0, 0);
            for (const auto& label : desiredLabels) {
                SendMessageW(sessCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        if (matchIdx >= 0 && SendMessageW(sessCombo, CB_GETCURSEL, 0, 0) != matchIdx) {
            SendMessageW(sessCombo, CB_SETCURSEL, matchIdx, 0);
        }
    }

    // 3. Directory file list & board tip
    auto displayIp = listening ? listeningIp : ControlText(ipCombo);
    if (displayIp.find(L"0.0.0.0") != std::wstring::npos) {
        auto ips = server_->LocalIps();
        displayIp = ips.empty() ? L"127.0.0.1" : Utf8ToWide(ips.front());
    }
    const std::wstring sampleName = !dirEntries_.empty() ? Utf8ToWide(dirEntries_.front().name) : L"<文件名>";
    const std::wstring boardTip = displayIp.rfind(L"127.", 0) == 0
        ? L"当前地址为本机回环，外部开发板无法连接。"
        : L"开发板设置参考：\r\nsetenv serverip " + displayIp + L"\r\ntftpboot ${loadaddr} " + sampleName;
    const bool boardTipChanged = SetTextIfChanged(Control(BoardTip), boardTip);

    std::wstring filesLabel = L"目录文件列表 (已排除子目录):";
    if (!dirEntries_.empty()) {
        filesLabel = L"目录文件列表 (共 " + std::to_wstring(dirEntries_.size()) + L" 个文件，不含子目录):";
    }
    SetTextIfChanged(Control(DirFilesLabel), filesLabel);

    std::wstring signature;
    if (dirEntries_.empty()) {
        signature = L"notice:" + (dirListNotice_.empty() ? L"(目录下无常规文件，已排除子目录)" : dirListNotice_);
    } else {
        signature = L"entries:" + std::to_wstring(dirEntries_.size()) + L":";
        for (const auto& entry : dirEntries_) {
            signature += Utf8ToWide(entry.name) + L"|" +
                (entry.sizeKnown ? std::to_wstring(entry.size) : L"?") + L"\n";
        }
    }
    if (signature != cachedDirListSignature_) {
        cachedDirListSignature_ = signature;
        HWND list = Control(DirFilesList);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        if (dirEntries_.empty()) {
            const std::wstring notice = dirListNotice_.empty() ? L"(目录下无常规文件，已排除子目录)" : dirListNotice_;
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(notice.c_str()));
        } else {
            for (const auto& entry : dirEntries_) {
                const std::wstring name = Utf8ToWide(entry.name);
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    }

    // 4. Transfer progress (Xfer style)
    auto trans = server_->ListTransfers();
    bool hasActive = false;
    const TftpTransferStatus* latest = nullptr;
    for (auto it = trans.rbegin(); it != trans.rend(); ++it) {
        if (it->phase == "transferring" || it->phase == "connecting") {
            hasActive = true;
            if (!latest) latest = &(*it);
            break;
        }
    }
    if (!latest && !trans.empty()) {
        latest = &trans.back();
    }

    uint64_t barBytes = 0;
    uint64_t barTotal = 0;
    std::wstring countText = L"等待传输";
    std::wstring detailText = L"(无活动传输)";
    bool detailError = false;

    if (latest) {
        if (hasActive) {
            barBytes = latest->ackedBytes;
            barTotal = latest->totalBytes;
            if (latest->totalBytes > 0) {
                countText = std::to_wstring(latest->ackedBytes) + L" / " +
                    std::to_wstring(latest->totalBytes) + L" 字节";
            } else {
                countText = std::to_wstring(latest->ackedBytes) + L" 字节 (正在读取...)";
            }
            if (latest->retransmits > 0) {
                countText += L" · 重传 " + std::to_wstring(latest->retransmits) + L" 次";
            }
        } else if (latest->phase == "completed") {
            barBytes = (latest->totalBytes == 0 && latest->ackedBytes == 0) ? 1 : latest->ackedBytes;
            barTotal = (latest->totalBytes == 0 && latest->ackedBytes == 0) ? 1 : std::max(latest->totalBytes, latest->ackedBytes);
            countText = L"传输完成 (" + std::to_wstring(latest->ackedBytes) + L" 字节)";
        } else if (latest->phase == "cancelled") {
            barBytes = latest->ackedBytes;
            barTotal = latest->totalBytes;
            countText = L"已取消 (" + std::to_wstring(latest->ackedBytes) + L" 字节)";
        } else {
            barBytes = latest->ackedBytes;
            barTotal = latest->totalBytes;
            detailError = true;
            countText = L"传输失败 (" + std::to_wstring(latest->ackedBytes) + L" 字节)";
        }

        detailText = Utf8ToWide(latest->tftpName) + L" <- " + Utf8ToWide(latest->clientIp) + L":" +
            std::to_wstring(latest->clientPort) + L" [" + Utf8ToWide(latest->phase) + L"]";
        if (!latest->error.empty()) {
            detailError = true;
            detailText += L"\r\n" + latest->error;
        }
    }

    transferActive_ = hasActive;
    displayBytes_ = barBytes;
    displayTotal_ = barTotal;
    const bool detailColorChanged = (transferHasError_ != detailError);
    transferHasError_ = detailError;

    SetTextIfChanged(Control(TransferCount), countText);
    if (SetTextIfChanged(Control(TransferDetail), detailText)) {
        UiSetControlTooltip(Control(TransferDetail), detailText.c_str());
    } else if (detailColorChanged && Control(TransferDetail)) {
        InvalidateRect(Control(TransferDetail), nullptr, FALSE);
    }
    EnableWindow(Control(CancelTransferBtn), hasActive);

    const bool progressChanged = (lastProgressActive_ != transferActive_ ||
                                  lastProgressBytes_ != displayBytes_ ||
                                  lastProgressTotal_ != displayTotal_);
    lastProgressActive_ = transferActive_;
    lastProgressBytes_ = displayBytes_;
    lastProgressTotal_ = displayTotal_;

    if (hasActive) {
        SetTimer(window_, 1, 100, nullptr);
    } else if (listening) {
        SetTimer(window_, 1, 250, nullptr);
    } else {
        KillTimer(window_, 1);
    }

    if (boardTipChanged) {
        Layout();
    } else if (progressChanged) {
        InvalidateProgress();
    }
}

void TftpPane::BrowseLocalDirectory() {
    const std::wstring picked = PickFolderDialog(window_, ControlText(Control(LocalPath)));
    if (picked.empty()) return;

    suppressEditNotify_ = true;
    SetTextIfChanged(Control(LocalPath), picked);
    suppressEditNotify_ = false;
    localDir_ = picked;
    ApplyLocalDirectory(true, true);
}

void TftpPane::ApplySelectedSource(bool userTriggered) {
    if (selectedSourceType_ == 0) {
        remoteListingPending_ = false;
        ++remoteListSeq_;
        ApplyLocalDirectory(userTriggered, userTriggered);
    } else {
        ApplyRemoteDirectory(userTriggered, userTriggered);
    }
}

void TftpPane::ApplyLocalDirectory(bool showError, bool persistState) {
    if (Control(LocalPath)) {
        localDir_ = ControlText(Control(LocalPath));
    }
    const std::wstring trimmed = TrimWhitespace(localDir_);
    if (trimmed.empty()) {
        if (selectedSourceType_ == 0 && server_) server_->ClearRoot();
        dirEntries_.clear();
        dirListNotice_ = L"(请选择或输入目录后自动列出文件)";
        localNotice_.clear();
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    std::vector<TftpDirectoryEntry> entries;
    std::wstring err;
    if (!ListLocalTftpDirectory(trimmed, entries, err)) {
        if (selectedSourceType_ == 0 && server_) server_->ClearRoot();
        dirEntries_.clear();
        dirListNotice_ = L"(无法读取目录: " + err + L")";
        localNotice_ = showError ? err : L"";
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    if (selectedSourceType_ == 0 && server_) {
        std::wstring rootErr;
        if (!server_->SetLocalRoot(trimmed, rootErr)) {
            dirEntries_.clear();
            dirListNotice_ = L"(无法设置根目录: " + rootErr + L")";
            localNotice_ = showError ? rootErr : L"";
            Refresh();
            if (persistState && stateChanged) stateChanged();
            return;
        }
    }

    dirEntries_ = std::move(entries);
    dirListNotice_ = dirEntries_.empty() ? L"(目录下无常规文件，已排除子目录)" : L"";
    localNotice_.clear();
    Refresh();
    if (persistState && stateChanged) stateChanged();
}

void TftpPane::OnLocalPathTyped() {
    localDir_ = ControlText(Control(LocalPath));
    const std::wstring trimmed = TrimWhitespace(localDir_);
    if (trimmed.empty()) {
        ApplyLocalDirectory(false, true);
        return;
    }
    const DWORD attrs = GetFileAttributesW(trimmed.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::wstring normalized, err;
        if (ValidateLocalTftpDirectory(trimmed, normalized, err)) {
            ApplyLocalDirectory(false, true);
        }
    }
}

void TftpPane::ApplyRemoteDirectory(bool allowAsyncFetch, bool persistState) {
    if (Control(RemotePath)) {
        remoteDir_ = ControlText(Control(RemotePath));
    }
    std::wstring trimmed = TrimWhitespace(remoteDir_);
    while (trimmed.size() > 1 && trimmed.back() == L'/') {
        trimmed.pop_back();
    }

    if (trimmed.empty()) {
        remoteListingPending_ = false;
        ++remoteListSeq_;
        if (selectedSourceType_ == 1 && server_) server_->ClearRoot();
        dirEntries_.clear();
        dirListNotice_ = L"(请输入远程 POSIX 目录路径，例如 /tftpboot)";
        localNotice_.clear();
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    if (!allowAsyncFetch) {
        if (dirEntries_.empty() && dirListNotice_.empty()) {
            dirListNotice_ = L"(点击刷新或按回车以加载远程目录文件列表)";
        }
        Refresh();
        return;
    }

    if (sshProvider_) {
        cachedSshSessions_ = sshProvider_();
    }
    const TftpSshSessionInfo* session = nullptr;
    for (const auto& s : cachedSshSessions_) {
        if (s.sessionId == selectedRemoteSessionId_) {
            session = &s;
            break;
        }
    }
    if (!session && !cachedSshSessions_.empty()) {
        for (const auto& s : cachedSshSessions_) {
            if (s.connected) {
                session = &s;
                break;
            }
        }
        if (!session) session = &cachedSshSessions_.front();
        selectedRemoteSessionId_ = session->sessionId;
    }
    if (!session) {
        remoteListingPending_ = false;
        ++remoteListSeq_;
        if (selectedSourceType_ == 1 && server_) server_->ClearRoot();
        dirEntries_.clear();
        localNotice_ = L"请先配置并选择一个 SSH 连接";
        dirListNotice_ = L"(未选择可用 SSH 连接)";
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    SshRemoteConfig cfg;
    cfg.host = session->host;
    cfg.port = session->port;
    cfg.username = session->username;
    cfg.password = session->password;
    cfg.remotePath = trimmed;

    std::wstring err;
    if (!ValidateRemoteTftpDirectoryConfig(cfg, err)) {
        remoteListingPending_ = false;
        ++remoteListSeq_;
        if (selectedSourceType_ == 1 && server_) server_->ClearRoot();
        dirEntries_.clear();
        localNotice_ = err;
        dirListNotice_ = L"(远程目录配置无效: " + err + L")";
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    if (selectedSourceType_ == 1 && server_) {
        std::wstring rootErr;
        server_->SetRemoteRoot(cfg, rootErr);
    }

    const uint64_t reqId = ++remoteListSeq_;
    remoteListingPending_ = true;
    dirEntries_.clear();
    dirListNotice_ = L"(正在读取远程目录文件...)";
    localNotice_.clear();
    lastAutoQueriedKey_ = session->sessionId + ":" + std::to_string(session->connected ? 1 : 0) + ":" + WideToUtf8(cfg.remotePath);
    Refresh();
    if (persistState && stateChanged) stateChanged();

    auto shared = remoteListShared_;
    std::string sessionId = session->sessionId;
    if (shared) {
        shared->cancel.store(false);
        remoteListWorkers_.Retire([shared, reqId, cfg = std::move(cfg), sessionId = std::move(sessionId)]() mutable {
            if (shared->cancel.load()) return;
            auto result = std::make_unique<AsyncRemoteListingResult>();
            result->requestId = reqId;
            result->remoteDir = cfg.remotePath;
            result->sessionId = std::move(sessionId);
            result->config = cfg;
            result->ok = ListRemoteTftpDirectory(cfg, result->entries, result->error);
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (!shared->cancel.load() && shared->hwnd &&
                PostMessageW(shared->hwnd, kMsgRemoteDirDone, 0, reinterpret_cast<LPARAM>(result.get()))) {
                result.release();
            }
        });
    }
}

void TftpPane::OnRemoteDirDone(LPARAM lParam) {
    std::unique_ptr<AsyncRemoteListingResult> res(reinterpret_cast<AsyncRemoteListingResult*>(lParam));
    if (!res || res->requestId != remoteListSeq_.load()) {
        return;
    }
    remoteListingPending_ = false;
    if (selectedSourceType_ != 1) {
        return;
    }
    if (res->ok) {
        dirEntries_ = std::move(res->entries);
        dirListNotice_ = dirEntries_.empty() ? L"(目录下无常规文件，已排除子目录)" : L"";
        localNotice_.clear();
        if (server_) {
            std::wstring err;
            server_->SetRemoteRoot(res->config, err);
        }
    } else {
        dirEntries_.clear();
        localNotice_ = res->error;
        dirListNotice_ = L"(无法列出远程目录: " + res->error + L")";
    }
    Refresh();
}

void TftpPane::RefreshDirectory(bool userTriggered) {
    if (userTriggered && !UiTryAcquireActionCooldown(Control(RefreshDirBtn), RefreshDirBtn, 300)) return;
    ApplySelectedSource(userTriggered);
}

void TftpPane::OnToggleService() {
    if (!server_) return;
    if (!UiTryAcquireActionCooldown(Control(ToggleService), ToggleService, 300)) return;
    localNotice_.clear();
    if (server_->IsListening()) {
        server_->Stop();
    } else {
        std::wstring selIp = ControlText(Control(BindIp));
        auto pos = selIp.find(L' ');
        if (pos != std::wstring::npos) selIp = selIp.substr(0, pos);
        std::string ip = WideToUtf8(selIp);

        std::wstring portStr = ControlText(Control(Port));
        uint16_t port = 69;
        try {
            if (!portStr.empty()) port = static_cast<uint16_t>(std::stoi(portStr));
        } catch (...) {
            port = 69;
        }

        std::wstring err;
        if (!server_->Start(ip, port, err)) {
            localNotice_ = err;
        }
    }
    Refresh();
    if (stateChanged) stateChanged();
}

void TftpPane::OnCancelTransfer() {
    if (!server_) return;
    auto trans = server_->ListTransfers();
    for (const auto& t : trans) {
        if (t.phase == "transferring" || t.phase == "connecting") {
            server_->CancelTransfer(t.transferId);
            break;
        }
    }
    Refresh();
}

void TftpPane::Paint(HDC target) {
    RECT rc{};
    GetClientRect(body_, &rc);
    UiBufferedPaint paint(target, rc);
    HDC dc = paint.Dc();
    FillRect(dc, &rc, surface_);

    const auto c = UiTheme(dark_);
    if (dirListFrame_.right > dirListFrame_.left && dirListFrame_.bottom > dirListFrame_.top) {
        UiBox(dc, dirListFrame_, c.field, c.border, D(8));
    }
    if (progressTop_ >= 0 && progressRight_ > progressLeft_) {
        UiDrawProgressBar(dc, progressLeft_, progressTop_, progressRight_, dpi_,
                          displayBytes_, displayTotal_, transferActive_ && displayTotal_ == 0,
                          c.raised, c.accent);
    }

    if (contentHeight_ > bodyHeight_) {
        const int thumb = std::max(D(28), bodyHeight_ * bodyHeight_ / std::max(1, contentHeight_));
        const int y = offset_ * (bodyHeight_ - thumb) / std::max(1, contentHeight_ - bodyHeight_);
        UiDrawScrollbarThumb(dc, {rc.right - D(5), y, rc.right, y + thumb}, dpi_, c);
    }
}

LRESULT CALLBACK TftpPane::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<TftpPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        if (self && self->surface_) FillRect(reinterpret_cast<HDC>(wParam), &r, self->surface_);
        return 1;
    }
    case WM_SIZE: {
        if (self) self->Layout();
        return 0;
    }
    case WM_SHOWWINDOW: {
        if (self) {
            if (wParam) {
                self->Refresh();
                if (self->selectedSourceType_ == 0 && !TrimWhitespace(self->LocalDirectory()).empty()) {
                    self->ApplyLocalDirectory(false, false);
                } else if (self->selectedSourceType_ == 1 && !self->remoteListingPending_) {
                    const std::wstring rdir = self->RemoteDirectory();
                    if (!rdir.empty() && self->dirEntries_.empty()) {
                        for (const auto& s : self->cachedSshSessions_) {
                            if (s.sessionId == self->selectedRemoteSessionId_ && s.connected) {
                                const std::string key = s.sessionId + ":1:" + WideToUtf8(rdir);
                                if (self->lastAutoQueriedKey_ != key) {
                                    self->ApplyRemoteDirectory(true, false);
                                }
                                break;
                            }
                        }
                    }
                }
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            } else {
                KillTimer(hwnd, 1);
            }
        }
        return 0;
    }
    case WM_TIMER: {
        if (self) self->Refresh();
        return 0;
    }
    case kMsgRemoteDirDone: {
        if (self) {
            self->OnRemoteDirDone(lParam);
        } else {
            delete reinterpret_cast<AsyncRemoteListingResult*>(lParam);
        }
        return 0;
    }
    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        if (self && self->remoteListShared_) {
            std::lock_guard<std::mutex> lock(self->remoteListShared_->mutex);
            self->remoteListShared_->hwnd = nullptr;
        }
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, kMsgRemoteDirDone, kMsgRemoteDirDone, PM_REMOVE)) {
            delete reinterpret_cast<AsyncRemoteListingResult*>(pending.lParam);
        }
        return 0;
    }
    case WM_CTLCOLORBTN: {
        if (self) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            const auto c = UiTheme(self->dark_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, c.text);
            SetBkColor(dc, c.surface);
            return reinterpret_cast<LRESULT>(self->surface_);
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (self) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            self->Scroll(-delta / 2);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TftpPane::BodyProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto self = reinterpret_cast<TftpPane*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (self) self->Paint(reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_MEASUREITEM: {
        if (self) {
            auto mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            mis->itemHeight = mis->CtlID == DirFilesList ? self->D(30) : self->D(28);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        if (self) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            const auto c = UiTheme(self->dark_);
            wchar_t cls[24]{};
            GetClassNameW(ctrl, cls, 24);
            const bool isEdit = (msg == WM_CTLCOLOREDIT) || (_wcsicmp(cls, L"EDIT") == 0);
            if (isEdit) {
                const bool enabled = IsWindowEnabled(ctrl) != FALSE;
                const bool readOnly = (GetWindowLongPtrW(ctrl, GWL_STYLE) & ES_READONLY) != 0;
                SetTextColor(dc, (!enabled || readOnly) ? c.muted : c.text);
                SetBkColor(dc, c.field);
                return reinterpret_cast<INT_PTR>(self->field_);
            }
            if (ctrl == self->Control(DirFilesList)) {
                SetTextColor(dc, c.text);
                SetBkColor(dc, c.field);
                return reinterpret_cast<INT_PTR>(self->field_);
            }
            SetBkMode(dc, TRANSPARENT);
            if (ctrl == self->Control(TransferDetail)) {
                SetTextColor(dc, self->transferHasError_ ? c.danger : c.muted);
            } else {
                SetTextColor(dc, c.text);
            }
            SetBkColor(dc, c.surface);
            return reinterpret_cast<INT_PTR>(self->surface_);
        }
        break;
    }
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (self) {
            if (item->CtlType == ODT_COMBOBOX) {
                UiDrawComboItem(*item, self->style_);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == DirFilesList) {
                if (item->itemID == static_cast<UINT>(-1)) return TRUE;
                UiBufferedPaint paint(item->hDC, item->rcItem);
                auto dc = paint.Dc();
                const auto c = UiTheme(self->dark_);
                const RECT row = item->rcItem;
                if (self->dirEntries_.empty()) {
                    HBRUSH bg = CreateSolidBrush(c.field);
                    FillRect(dc, &row, bg);
                    DeleteObject(bg);
                    const std::wstring notice = self->dirListNotice_.empty() ? L"(目录下无常规文件，已排除子目录)" : self->dirListNotice_;
                    const auto old = SelectObject(dc, self->font_);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, c.muted);
                    RECT textRect{row.left + self->D(8), row.top, row.right - self->D(8), row.bottom};
                    DrawTextW(dc, notice.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(dc, old);
                    return TRUE;
                }
                if (item->itemID >= self->dirEntries_.size()) return TRUE;
                const auto& entry = self->dirEntries_[item->itemID];
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                HBRUSH rowBrush = CreateSolidBrush(selected ? c.accent : c.field);
                FillRect(dc, &row, rowBrush);
                DeleteObject(rowBrush);
                // Reuse SFTP's UiGlyph::Document icon
                const COLORREF iconColor = selected ? RGB(255, 255, 255) : c.muted;
                const int iconSize = self->D(18);
                const int iconBoxLeft = row.left + self->D(4);
                const int iconBoxRight = row.left + self->D(28);
                const int ix = (iconBoxLeft + iconBoxRight - iconSize) / 2;
                const int iy = (row.top + row.bottom - iconSize) / 2;
                UiDrawGlyph(dc, {ix, iy, ix + iconSize, iy + iconSize}, iconColor, UiGlyph::Document);

                const std::wstring sizeText = entry.sizeKnown ? FormatByteSize(entry.size) : L"大小未知";
                RECT listRc{};
                GetClientRect(item->hwndItem, &listRc);
                const int itemH = std::max(1, static_cast<int>(row.bottom - row.top));
                const int count = static_cast<int>(SendMessageW(item->hwndItem, LB_GETCOUNT, 0, 0));
                const int pageItems = std::max(1, static_cast<int>(listRc.bottom) / itemH);
                const int rightInset = count > pageItems ? self->D(8) : 0;
                const int sizeWidth = self->D(68);
                const auto oldFont = SelectObject(dc, self->font_);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, selected ? RGB(255, 255, 255) : c.text);
                RECT nameRect{row.left + self->D(30), row.top, row.right - rightInset - sizeWidth - self->D(10), row.bottom};
                DrawTextW(dc, Utf8ToWide(entry.name).c_str(), -1, &nameRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SetTextColor(dc, selected ? RGB(235, 240, 255) : c.muted);
                RECT sizeRect{row.right - rightInset - sizeWidth - self->D(6), row.top, row.right - rightInset - self->D(8), row.bottom};
                DrawTextW(dc, sizeText.c_str(), -1, &sizeRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, oldFont);

                HPEN divider = CreatePen(PS_SOLID, 1, c.border);
                HPEN oldDivider = reinterpret_cast<HPEN>(SelectObject(dc, divider));
                MoveToEx(dc, row.left, row.bottom - 1, nullptr);
                LineTo(dc, row.right, row.bottom - 1);
                SelectObject(dc, oldDivider);
                DeleteObject(divider);
                return TRUE;
            }
            UiBufferedPaint paint(item->hDC, item->rcItem);
            auto dc = paint.Dc();
            const auto c = UiTheme(self->dark_);
            FillRect(dc, &item->rcItem, self->surface_);
            const bool enabled = IsWindowEnabled(item->hwndItem) != FALSE;
            const bool flat = GetPropW(item->hwndItem, L"SerialCtl.FlatIconVisual") != nullptr;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            const bool hovered = GetPropW(item->hwndItem, L"SerialCtl.PowerHover") != nullptr;
            const bool isListening = self->server_ && self->server_->IsListening();
            const bool isPrimary = (item->CtlID == ToggleService && !isListening);
            const bool isDanger = (item->CtlID == ToggleService && isListening) || item->CtlID == CancelTransferBtn;
            const auto v = flat ? UiFlatIconVisual(c, c.surface, enabled, pressed, hovered)
                                : UiButtonVisual(c, enabled, pressed, hovered, isPrimary);
            if (!flat || v.fill != c.surface)
                UiBox(dc, item->rcItem, v.fill, flat ? v.fill : v.border, UiCapsuleRadius(item->rcItem));
            UiButtonKeyboardFocus(dc, item->rcItem, item->itemState, self->dpi_, flat ? UiFocusInk(self->dark_) : v.ink);
            const COLORREF inkColor = isDanger && enabled ? c.danger : v.ink;
            if (!UiDrawButtonGlyph(dc, *item, inkColor, self->dpi_)) {
                const auto old = SelectObject(dc, self->font_);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, inkColor);
                auto r = UiCapsuleContentRect(item->rcItem, self->dpi_);
                DrawTextW(dc, ControlText(item->hwndItem).c_str(), -1, &r,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(dc, old);
            }
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (self) {
            if (id == ToggleService && code == BN_CLICKED) self->OnToggleService();
            else if (id == BrowseLocal && code == BN_CLICKED) self->BrowseLocalDirectory();
            else if (id == RefreshDirBtn && code == BN_CLICKED) self->RefreshDirectory(true);
            else if (id == CancelTransferBtn && code == BN_CLICKED) self->OnCancelTransfer();
            else if (id == SourceType && code == CBN_SELCHANGE) {
                self->selectedSourceType_ = static_cast<int>(SendMessageW(self->Control(SourceType), CB_GETCURSEL, 0, 0));
                self->Layout();
                self->ApplySelectedSource(true);
            } else if (id == RemoteSession && code == CBN_SELCHANGE) {
                const int idx = static_cast<int>(SendMessageW(self->Control(RemoteSession), CB_GETCURSEL, 0, 0));
                if (idx >= 0 && static_cast<size_t>(idx) < self->cachedSshSessions_.size()) {
                    self->selectedRemoteSessionId_ = self->cachedSshSessions_[idx].sessionId;
                }
                if (self->selectedSourceType_ == 1) {
                    self->ApplyRemoteDirectory(true, true);
                }
            } else if ((id == BindIp || id == SourceType || id == RemoteSession) && code == CBN_CLOSEUP) {
                self->Refresh();
            } else if (id == LocalPath && code == EN_CHANGE && !self->suppressEditNotify_) {
                self->OnLocalPathTyped();
            } else if (id == LocalPath && code == EN_KILLFOCUS && !self->suppressEditNotify_) {
                const bool focusingBrowse = (GetFocus() == self->Control(BrowseLocal));
                self->ApplyLocalDirectory(!focusingBrowse, !focusingBrowse);
            } else if (id == RemotePath && code == EN_CHANGE && !self->suppressEditNotify_) {
                self->remoteDir_ = ControlText(self->Control(RemotePath));
            } else if (id == RemotePath && code == EN_KILLFOCUS && !self->suppressEditNotify_) {
                self->ApplyRemoteDirectory(true, true);
            }
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (self) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            self->Scroll(-delta / 2);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TftpPane::FieldProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto self = reinterpret_cast<TftpPane*>(dwRefData);
    switch (msg) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, FieldProc, uIdSubclass);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN && self) {
            if (uIdSubclass == LocalPath) self->ApplyLocalDirectory(true, true);
            else if (uIdSubclass == RemotePath) self->ApplyRemoteDirectory(true, true);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == L'\r' || wParam == L'\n') return 0;
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TftpPane::DirListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto self = reinterpret_cast<TftpPane*>(dwRefData);
    auto getMetrics = [hwnd, self](int& count, int& pageItems, int& maxTop, int& topIndex, int& thumbY, int& thumbH, int& clientH) -> bool {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        clientH = rc.bottom - rc.top;
        count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
        const int itemH = std::max(1, static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0)));
        pageItems = std::max(1, clientH / itemH);
        if (count <= pageItems || clientH <= 0) return false;
        maxTop = std::max(1, count - pageItems);
        topIndex = std::clamp(static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0)), 0, maxTop);
        thumbH = std::max(self->D(24), clientH * pageItems / count);
        thumbY = topIndex * std::max(0, clientH - thumbH) / maxTop;
        return true;
    };
    if (self && msg == WM_MOUSEWHEEL) {
        int count = 0, pageItems = 0, maxTop = 0, topIndex = 0, thumbY = 0, thumbH = 0, clientH = 0;
        if (getMetrics(count, pageItems, maxTop, topIndex, thumbY, thumbH, clientH)) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = std::max(1, std::abs(delta) / WHEEL_DELTA);
            const int target = std::clamp(topIndex + (delta > 0 ? -1 : 1) * notches * 3, 0, maxTop);
            SendMessageW(hwnd, LB_SETTOPINDEX, target, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
            return 0;
        }
        self->Scroll(-GET_WHEEL_DELTA_WPARAM(wParam) / 2);
        return 0;
    }
    if (self && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int count = 0, pageItems = 0, maxTop = 0, topIndex = 0, thumbY = 0, thumbH = 0, clientH = 0;
        if (getMetrics(count, pageItems, maxTop, topIndex, thumbY, thumbH, clientH) && pt.x >= rc.right - self->D(8)) {
            SetFocus(hwnd);
            if (pt.y >= thumbY && pt.y < thumbY + thumbH) {
                self->dirListDragging_ = true;
                self->dirListDragOffset_ = pt.y - thumbY;
                SetCapture(hwnd);
            } else {
                const int target = std::clamp(topIndex + (pt.y < thumbY ? -pageItems : pageItems), 0, maxTop);
                SendMessageW(hwnd, LB_SETTOPINDEX, target, 0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
    }
    if (self && msg == WM_MOUSEMOVE && self->dirListDragging_) {
        int count = 0, pageItems = 0, maxTop = 0, topIndex = 0, thumbY = 0, thumbH = 0, clientH = 0;
        if (getMetrics(count, pageItems, maxTop, topIndex, thumbY, thumbH, clientH)) {
            const int travel = std::max(1, clientH - thumbH);
            const int desired = std::clamp(static_cast<int>(GET_Y_LPARAM(lParam)) - self->dirListDragOffset_, 0, travel);
            const int target = std::clamp(desired * maxTop / travel, 0, maxTop);
            SendMessageW(hwnd, LB_SETTOPINDEX, target, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        return 0;
    }
    if (self && ((msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) && self->dirListDragging_)) {
        self->dirListDragging_ = false;
        self->dirListDragOffset_ = 0;
        if (msg == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (msg == WM_LBUTTONUP) return 0;
    }
    const LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (self && msg == WM_PAINT) {
        int count = 0, pageItems = 0, maxTop = 0, topIndex = 0, thumbY = 0, thumbH = 0, clientH = 0;
        if (getMetrics(count, pageItems, maxTop, topIndex, thumbY, thumbH, clientH)) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC dc = GetDC(hwnd);
            if (dc) {
                const auto c = UiTheme(self->dark_);
                UiDrawScrollbarThumb(dc, {rc.right - self->D(5), thumbY, rc.right, thumbY + thumbH}, self->dpi_, c);
                ReleaseDC(hwnd, dc);
            }
        }
    } else if (msg == WM_KEYDOWN || msg == WM_SIZE || msg == LB_SETTOPINDEX) {
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == LB_ADDSTRING || msg == LB_DELETESTRING || msg == LB_RESETCONTENT) {
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, DirListProc, uIdSubclass);
    return res;
}

} // namespace serialctl
