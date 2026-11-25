#include "TftpPane.h"
#include "Win32Helpers.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace serialctl {
namespace {

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

std::wstring ControlText(HWND h) {
    if (!h) return {};
    const int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1);
    GetWindowTextW(h, buf.data(), len + 1);
    return buf.data();
}

bool SetTextIfChanged(HWND h, const std::wstring& value) {
    if (!h || ControlText(h) == value) return false;
    SetWindowTextW(h, value.c_str());
    return true;
}

std::string WideToUtf8(const std::wstring& w) {
    return WideToMultiByte(w, CP_UTF8);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    return MultiByteToWide(reinterpret_cast<const uint8_t*>(s.data()), s.size(), CP_UTF8);
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
        if (SUCCEEDED(dialog.ptr->Show(owner))) {
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
    if (window_) DestroyWindow(window_);
    if (surface_) DeleteObject(surface_);
    if (field_) DeleteObject(field_);
}

HWND TftpPane::Control(int id) const {
    return GetDlgItem(body_ ? body_ : window_, id);
}

bool TftpPane::Create(HWND parent, std::shared_ptr<TftpServer> server, HFONT font, bool dark, int dpi) {
    server_ = std::move(server);
    font_ = font;
    dark_ = dark;
    dpi_ = dpi;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SerialCtlTftpPane";
    RegisterClassExW(&wc);

    WNDCLASSEXW bc{sizeof(bc)};
    bc.lpfnWndProc = BodyProc;
    bc.hInstance = inst;
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bc.lpszClassName = L"SerialCtlTftpBody";
    RegisterClassExW(&bc);

    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"SerialCtlTftpPane", L"TFTP 服务",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 100, parent, nullptr, inst, this);
    if (!window_) return false;

    body_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"SerialCtlTftpBody", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100, window_, nullptr, inst, this);
    if (!body_) return false;

    auto add = [this, inst](int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                                 0, 0, 10, 10, body_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (_wcsicmp(cls, L"BUTTON") == 0) UiStyleButton(h);
        return h;
    };

    add(BindIpLabel, L"STATIC", L"监听网卡 / 服务 IP:", SS_LEFT | SS_NOPREFIX);
    add(BindIp, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    add(ToggleService, L"BUTTON", L"启动服务", BS_OWNERDRAW | WS_TABSTOP);

    add(LocalPathLabel, L"STATIC", L"本地 TFTP 目录:", SS_LEFT | SS_NOPREFIX);
    HWND localEdit = add(LocalPath, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);
    SetWindowSubclass(localEdit, FieldProc, LocalPath, reinterpret_cast<DWORD_PTR>(this));

    auto browseBtn = add(BrowseLocal, L"BUTTON", L"选择目录", BS_OWNERDRAW | WS_TABSTOP);
    UiSetFlatIconButton(browseBtn);
    UiSetButtonGlyph(browseBtn, UiGlyph::Folder, L"选择本地 TFTP 目录");

    auto refreshBtn = add(RefreshDirBtn, L"BUTTON", L"刷新", BS_OWNERDRAW | WS_TABSTOP);
    UiSetFlatIconButton(refreshBtn);
    UiSetButtonGlyph(refreshBtn, UiGlyph::Refresh, L"刷新目录文件列表");

    dirListNotice_ = L"(请选择或输入目录后自动列出文件)";
    add(DirFilesLabel, L"STATIC", L"目录文件列表 (已排除子目录):", SS_LEFT | SS_NOPREFIX);
    add(DirFilesList, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP);
    SetWindowSubclass(Control(DirFilesList), DirListProc, DirFilesList, reinterpret_cast<DWORD_PTR>(this));

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
    const auto c = UiTheme(dark_);
    surface_ = CreateSolidBrush(c.surface);
    field_ = CreateSolidBrush(c.field);

    if (!window_) return;

    for (int id = BindIpLabel; id <= DirFilesList; ++id) {
        HWND h = Control(id);
        if (!h) continue;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
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

    struct FieldPosition { int id, x, y, width, height, dropHeight; };
    std::vector<FieldPosition> positions;
    int y = D(8);
    auto place = [&](int id, int h, int w = -1, int dropHeight = 0) {
        if (w < 0) w = contentWidth;
        positions.push_back({id, 0, y, w, h, dropHeight});
        y += h + gap;
    };

    place(BindIpLabel, hLabel);
    place(BindIp, hField, -1, D(150));
    place(ToggleService, D(32), contentWidth);
    y += gap / 2;

    place(LocalPathLabel, hLabel);
    const int editW = std::max(D(60), contentWidth - btnWidth * 2 - btnGap * 2);
    positions.push_back({LocalPath, 0, y, editW, hField, 0});
    positions.push_back({BrowseLocal, editW + btnGap, y, btnWidth, hField, 0});
    positions.push_back({RefreshDirBtn, editW + btnGap * 2 + btnWidth, y, btnWidth, hField, 0});
    y += hField + gap + gap / 2;

    place(DirFilesLabel, hLabel);
    const int bottomPad = D(12);
    const int remainingHeight = bodyHeight_ - y - bottomPad;
    const int listHeight = std::max(D(140), remainingHeight);
    positions.push_back({DirFilesList, 0, y, contentWidth, listHeight, 0});
    y += listHeight + bottomPad;

    contentHeight_ = y;
    offset_ = std::clamp(offset_, 0, std::max(0, contentHeight_ - bodyHeight_));

    dirListFrame_ = {};
    UiLayoutBatch layout(true);
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

void TftpPane::ConfigureDirectory(const std::wstring& localDirectory) {
    localDir_ = localDirectory;
    if (window_) {
        suppressEditNotify_ = true;
        SetTextIfChanged(Control(LocalPath), localDir_);
        suppressEditNotify_ = false;
    }
    ApplyLocalDirectory(false, false);
}

std::wstring TftpPane::LocalDirectory() const {
    HWND edit = Control(LocalPath);
    return edit ? TrimWhitespace(ControlText(edit)) : TrimWhitespace(localDir_);
}

void TftpPane::PopulateBindIpCombo(bool preserveSelection) {
    HWND ipCombo = Control(BindIp);
    if (!ipCombo || !server_) return;

    std::wstring prevIp;
    if (preserveSelection && SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) > 0) {
        prevIp = TrimWhitespace(ControlText(ipCombo));
        const size_t spacePos = prevIp.find(L' ');
        if (spacePos != std::wstring::npos) {
            prevIp = prevIp.substr(0, spacePos);
        }
    }

    const auto ips = server_->LocalIps();
    SendMessageW(ipCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& ip : ips) {
        const std::wstring wip = Utf8ToWide(ip);
        SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wip.c_str()));
    }
    SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.0.0.0 (所有可用网卡)"));
    cachedLocalIps_ = ips;

    LRESULT targetIndex = 0;
    if (preserveSelection && bindIpExplicit_ && !prevIp.empty()) {
        LRESULT found = CB_ERR;
        if (prevIp == L"0.0.0.0") {
            found = SendMessageW(ipCombo, CB_FINDSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(L"0.0.0.0"));
        } else {
            found = SendMessageW(ipCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(prevIp.c_str()));
        }
        if (found != CB_ERR) {
            targetIndex = found;
        } else {
            bindIpExplicit_ = false;
        }
    }
    if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(ipCombo, CB_SETCURSEL, targetIndex, 0);
    }
}

void TftpPane::ConfigureBindIp(const std::wstring& bindIp, bool explicitChoice) {
    HWND ipCombo = Control(BindIp);
    if (!ipCombo) return;
    if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) == 0) {
        PopulateBindIpCombo(false);
    }

    const std::wstring trimmed = TrimWhitespace(bindIp);
    if (trimmed.empty() || (!explicitChoice && trimmed == L"0.0.0.0")) {
        if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) > 0) {
            SendMessageW(ipCombo, CB_SETCURSEL, 0, 0);
        }
        bindIpExplicit_ = false;
    } else if (trimmed == L"0.0.0.0") {
        LRESULT index = SendMessageW(ipCombo, CB_FINDSTRING, static_cast<WPARAM>(-1),
                                     reinterpret_cast<LPARAM>(L"0.0.0.0"));
        if (index == CB_ERR) {
            index = SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.0.0.0 (所有可用网卡)"));
        }
        if (index >= 0) {
            SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
        }
        bindIpExplicit_ = true;
    } else {
        LRESULT index = SendMessageW(ipCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                     reinterpret_cast<LPARAM>(trimmed.c_str()));
        if (index != CB_ERR) {
            SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
            bindIpExplicit_ = explicitChoice;
        } else if (server_ && server_->IsListening() && Utf8ToWide(server_->BindIp()) == trimmed) {
            index = SendMessageW(ipCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(trimmed.c_str()));
            if (index >= 0) {
                SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
            }
            bindIpExplicit_ = explicitChoice;
        } else {
            if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) > 0) {
                SendMessageW(ipCombo, CB_SETCURSEL, 0, 0);
            }
            bindIpExplicit_ = false;
        }
    }
    Refresh();
}

std::wstring TftpPane::SelectedBindIp() const {
    if (server_ && server_->IsListening()) {
        return Utf8ToWide(server_->BindIp());
    }
    const std::wstring fallbackIp = server_ ? Utf8ToWide(server_->DefaultIp()) : L"127.0.0.1";
    HWND ipCombo = Control(BindIp);
    if (!ipCombo) return fallbackIp;
    std::wstring sel = TrimWhitespace(ControlText(ipCombo));
    const size_t spacePos = sel.find(L' ');
    if (spacePos != std::wstring::npos) {
        sel = sel.substr(0, spacePos);
    }
    return sel.empty() ? fallbackIp : sel;
}

void TftpPane::Refresh() {
    if (!window_ || !server_) return;
    if (SendMessageW(Control(BindIp), CB_GETDROPPEDSTATE, 0, 0)) {
        return;
    }

    const bool listening = server_->IsListening();
    const auto listeningIp = listening ? Utf8ToWide(server_->BindIp()) : std::wstring{};

    SetTextIfChanged(Control(ToggleService), listening ? L"停止服务" : L"启动服务");
    const std::wstring toggleTip = listening
        ? (L"停止 TFTP 服务 (当前监听 " + listeningIp + L":69)")
        : L"启动 TFTP 服务 (UDP 69)";
    if (toggleTip != cachedToggleTip_) {
        cachedToggleTip_ = toggleTip;
        UiSetControlTooltip(Control(ToggleService), toggleTip.c_str());
    }
    EnableWindow(Control(BindIp), !listening);

    HWND ipCombo = Control(BindIp);
    if (SendMessageW(ipCombo, CB_GETCOUNT, 0, 0) == 0) {
        PopulateBindIpCombo(false);
    } else if (!listening) {
        const auto ips = server_->LocalIps();
        if (ips != cachedLocalIps_) {
            PopulateBindIpCombo(true);
        }
    }
    if (listening) {
        LRESULT index = CB_ERR;
        if (listeningIp == L"0.0.0.0") {
            index = SendMessageW(ipCombo, CB_FINDSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(L"0.0.0.0"));
        } else {
            const auto address = reinterpret_cast<LPARAM>(listeningIp.c_str());
            index = SendMessageW(ipCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), address);
            if (index == CB_ERR) index = SendMessageW(ipCombo, CB_ADDSTRING, 0, address);
        }
        if (index >= 0 && SendMessageW(ipCombo, CB_GETCURSEL, 0, 0) != index) {
            SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
        }
    }

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

    if (listening) {
        SetTimer(window_, 1, 500, nullptr);
    } else {
        KillTimer(window_, 1);
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

void TftpPane::ApplyLocalDirectory(bool showError, bool persistState) {
    if (Control(LocalPath)) {
        localDir_ = ControlText(Control(LocalPath));
    }
    const std::wstring trimmed = TrimWhitespace(localDir_);
    if (trimmed.empty()) {
        if (server_) server_->ClearRoot();
        dirEntries_.clear();
        dirListNotice_ = L"(请选择或输入目录后自动列出文件)";
        Refresh();
        if (persistState && stateChanged) stateChanged();
        return;
    }

    std::vector<TftpDirectoryEntry> entries;
    std::wstring err;
    if (!ListLocalTftpDirectory(trimmed, entries, err)) {
        if (server_) server_->ClearRoot();
        dirEntries_.clear();
        dirListNotice_ = L"(无法读取目录: " + err + L")";
        Refresh();
        if (persistState && stateChanged) stateChanged();
        if (showError && IsWindowVisible(window_)) {
            MessageBoxW(window_, err.c_str(), L"TFTP 服务", MB_ICONWARNING | MB_OK);
        }
        return;
    }

    if (server_) {
        std::wstring rootErr;
        if (!server_->SetLocalRoot(trimmed, rootErr)) {
            dirEntries_.clear();
            dirListNotice_ = L"(无法设置根目录: " + rootErr + L")";
            Refresh();
            if (persistState && stateChanged) stateChanged();
            if (showError && IsWindowVisible(window_)) {
                MessageBoxW(window_, rootErr.c_str(), L"TFTP 服务", MB_ICONWARNING | MB_OK);
            }
            return;
        }
    }

    dirEntries_ = std::move(entries);
    dirListNotice_ = dirEntries_.empty() ? L"(目录下无常规文件，已排除子目录)" : L"";
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

void TftpPane::RefreshDirectory(bool userTriggered) {
    if (userTriggered && !UiTryAcquireActionCooldown(Control(RefreshDirBtn), RefreshDirBtn, 300)) return;
    ApplyLocalDirectory(userTriggered, userTriggered);
}

void TftpPane::OnToggleService() {
    if (!server_) return;
    if (!UiTryAcquireActionCooldown(Control(ToggleService), ToggleService, 300)) return;
    if (server_->IsListening()) {
        server_->Stop();
    } else {
        const std::string ip = WideToUtf8(SelectedBindIp());
        std::wstring err;
        if (!server_->Start(ip, 69, err)) {
            if (IsWindowVisible(window_)) {
                MessageBoxW(window_, err.c_str(), L"TFTP 服务", MB_ICONWARNING | MB_OK);
            }
        }
    }
    Refresh();
    if (stateChanged) stateChanged();
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
                if (!TrimWhitespace(self->LocalDirectory()).empty()) {
                    self->ApplyLocalDirectory(false, false);
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
    case WM_DESTROY: {
        KillTimer(hwnd, 1);
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
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
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
            SetTextColor(dc, c.text);
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
            const bool isDanger = (item->CtlID == ToggleService && isListening);
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
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (self) {
            if (id == ToggleService && code == BN_CLICKED) self->OnToggleService();
            else if (id == BrowseLocal && code == BN_CLICKED) self->BrowseLocalDirectory();
            else if (id == RefreshDirBtn && code == BN_CLICKED) self->RefreshDirectory(true);
            else if (id == BindIp && code == CBN_SELCHANGE) {
                self->bindIpExplicit_ = true;
                self->Refresh();
                if (self->stateChanged) self->stateChanged();
            } else if (id == BindIp && code == CBN_CLOSEUP) {
                self->Refresh();
            } else if (id == LocalPath && code == EN_CHANGE && !self->suppressEditNotify_) {
                self->OnLocalPathTyped();
            } else if (id == LocalPath && code == EN_KILLFOCUS && !self->suppressEditNotify_) {
                const bool focusingBrowse = (GetFocus() == self->Control(BrowseLocal));
                self->ApplyLocalDirectory(!focusingBrowse, !focusingBrowse);
            }
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (self) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
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
