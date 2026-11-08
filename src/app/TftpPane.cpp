#include "TftpPane.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <algorithm>
#include <sstream>

namespace serialctl {

static int MeasureText(HWND parent, HFONT font, const std::wstring& text, int width) {
    if (text.empty() || width <= 0) return 0;
    HDC dc = GetDC(parent);
    HFONT old = reinterpret_cast<HFONT>(SelectObject(dc, font));
    RECT rc{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(parent, dc);
    return rc.bottom - rc.top;
}

static std::wstring ControlText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::vector<wchar_t> buf(len + 1);
    GetWindowTextW(h, buf.data(), len + 1);
    return buf.data();
}

static bool SetTextIfChanged(HWND h, const std::wstring& value) {
    if (!h || ControlText(h) == value) return false;
    SetWindowTextW(h, value.c_str());
    return true;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), sz, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), sz);
    return w;
}

static std::wstring ExtractBasename(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

TftpPane::~TftpPane() {
    if (window_) DestroyWindow(window_);
    if (surface_) DeleteObject(surface_);
    if (field_) DeleteObject(field_);
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
        WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100, parent, nullptr, inst, this);
    if (!window_) return false;

    body_ = CreateWindowExW(0, L"SerialCtlTftpBody", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100, window_, nullptr, inst, this);
    if (!body_) return false;

    auto add = [this, inst](int id, const wchar_t* cls, const wchar_t* text, DWORD style) {
        HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, body_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (_wcsicmp(cls, L"BUTTON") == 0) UiStyleButton(h);
        return h;
    };

    // Service controls
    add(ServiceStatus, L"STATIC", L"服务状态：未启动", SS_LEFT | SS_NOPREFIX);
    add(BindIpLabel, L"STATIC", L"监听网卡 / IP:", SS_LEFT | SS_NOPREFIX);
    add(BindIp, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    add(PortLabel, L"STATIC", L"端口:", SS_LEFT | SS_NOPREFIX);
    add(Port, L"EDIT", L"69", ES_LEFT | ES_NUMBER | WS_TABSTOP);
    add(ToggleService, L"BUTTON", L"启动服务", BS_OWNERDRAW | WS_TABSTOP);
    add(BoardTip, L"STATIC", L"开发板参考：\r\nsetenv serverip <调试机IP>\r\ntftpboot ${loadaddr} <文件名>", SS_LEFT | SS_NOPREFIX);

    // Publishing controls
    add(SourceTypeLabel, L"STATIC", L"文件来源类别:", SS_LEFT | SS_NOPREFIX);
    auto srcCombo = add(SourceType, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    SendMessageW(srcCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"本机文件"));
    SendMessageW(srcCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"远程 SSH"));
    SendMessageW(srcCombo, CB_SETCURSEL, 0, 0);

    add(LocalPathLabel, L"STATIC", L"本地文件绝对路径:", SS_LEFT | SS_NOPREFIX);
    add(LocalPath, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);
    auto browseBtn = add(BrowseLocal, L"BUTTON", L"选择文件", BS_OWNERDRAW | WS_TABSTOP);
    UiSetFlatIconButton(browseBtn);
    UiSetButtonGlyph(browseBtn, UiGlyph::Folder, L"选择本地文件");

    add(RemoteSessionLabel, L"STATIC", L"已配置的 SSH 连接:", SS_LEFT | SS_NOPREFIX);
    add(RemoteSession, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    add(RemotePathLabel, L"STATIC", L"远程文件绝对路径 (POSIX):", SS_LEFT | SS_NOPREFIX);
    add(RemotePath, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);

    add(TftpNameLabel, L"STATIC", L"TFTP 发布名 (开发板请求的文件名):", SS_LEFT | SS_NOPREFIX);
    add(TftpName, L"EDIT", L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP);

    add(PublishBtn, L"BUTTON", L"发布文件", BS_OWNERDRAW | WS_TABSTOP);
    add(UnpublishBtn, L"BUTTON", L"取消发布", BS_OWNERDRAW | WS_TABSTOP);

    // Status and Lists
    add(PublishedLabel, L"STATIC", L"已发布文件列表:", SS_LEFT | SS_NOPREFIX);
    add(PublishedList, L"EDIT", L"(无发布文件)", ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    add(TransfersLabel, L"STATIC", L"实时传输任务:", SS_LEFT | SS_NOPREFIX);
    add(TransfersStatus, L"EDIT", L"(无活动传输)", ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
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
    const auto c = UiTheme(dark_);
    surface_ = CreateSolidBrush(c.surface);
    field_ = CreateSolidBrush(c.field);

    if (!window_) return;

    for (int id = ServiceStatus; id <= CancelTransferBtn; ++id) {
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
    MoveWindow(body_, 0, 0, width, bodyHeight_, TRUE);

    const int lane = D(16);
    const int contentWidth = std::max(D(80), width - lane);
    const int hField = D(26);
    const int hLabel = D(18);
    const int gap = D(6);

    const bool isRemote = (selectedSourceType_ == 1);

    // Show/hide based on source type
    ShowWindow(Control(LocalPathLabel), !isRemote ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(LocalPath), !isRemote ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(BrowseLocal), !isRemote ? SW_SHOW : SW_HIDE);

    ShowWindow(Control(RemoteSessionLabel), isRemote ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(RemoteSession), isRemote ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(RemotePathLabel), isRemote ? SW_SHOW : SW_HIDE);
    ShowWindow(Control(RemotePath), isRemote ? SW_SHOW : SW_HIDE);

    int y = 0;
    auto place = [&](int id, int h, int w = -1, int x = 0) {
        if (w < 0) w = contentWidth;
        MoveWindow(Control(id), x, y - offset_, std::max(1, w), h, TRUE);
        y += h + gap;
    };

    // Service area
    place(ServiceStatus, D(22));
    place(BindIpLabel, hLabel);
    place(BindIp, D(150)); // dropdown list height
    {
        UiLayoutBatch b;
        b.ComboVisibleHeight(Control(BindIp), hField);
        b.Commit();
    }
    y -= (D(150) - hField); // adjust back

    place(PortLabel, hLabel);
    place(Port, hField, D(90));
    place(ToggleService, D(30), contentWidth);

    int boardTipHeight = std::max(D(36), MeasureText(body_, font_, ControlText(Control(BoardTip)), contentWidth));
    place(BoardTip, boardTipHeight);
    y += gap;

    // File publishing area
    place(SourceTypeLabel, hLabel);
    place(SourceType, D(120));
    {
        UiLayoutBatch b;
        b.ComboVisibleHeight(Control(SourceType), hField);
        b.Commit();
    }
    y -= (D(120) - hField);

    if (!isRemote) {
        place(LocalPathLabel, hLabel);
        int browseWidth = D(28);
        MoveWindow(Control(LocalPath), 0, y - offset_, contentWidth - browseWidth - D(8), hField, TRUE);
        MoveWindow(Control(BrowseLocal), contentWidth - browseWidth, y - offset_, browseWidth, hField, TRUE);
        y += hField + gap;
    } else {
        place(RemoteSessionLabel, hLabel);
        place(RemoteSession, D(120));
        {
            UiLayoutBatch b;
            b.ComboVisibleHeight(Control(RemoteSession), hField);
            b.Commit();
        }
        y -= (D(120) - hField);

        place(RemotePathLabel, hLabel);
        place(RemotePath, hField);
    }

    place(TftpNameLabel, hLabel);
    place(TftpName, hField);

    int halfBtn = (contentWidth - D(8)) / 2;
    MoveWindow(Control(PublishBtn), 0, y - offset_, halfBtn, D(30), TRUE);
    MoveWindow(Control(UnpublishBtn), halfBtn + D(8), y - offset_, halfBtn, D(30), TRUE);
    y += D(30) + gap * 2;

    // Status and Lists
    place(PublishedLabel, hLabel);
    place(PublishedList, D(75));

    place(TransfersLabel, hLabel);
    place(TransfersStatus, D(65));
    place(CancelTransferBtn, D(28));

    contentHeight_ = y + D(20);
    offset_ = std::clamp(offset_, 0, std::max(0, contentHeight_ - bodyHeight_));
}

void TftpPane::Scroll(int amount) {
    offset_ = std::clamp(offset_ + amount, 0, std::max(0, contentHeight_ - bodyHeight_));
    Layout();
    InvalidateRect(body_, nullptr, FALSE);
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
    SetTextIfChanged(Control(ServiceStatus), statusText);

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
            index = SendMessageW(ipCombo, CB_FINDSTRINGEXACT, -1, address);
            if (index == CB_ERR) index = SendMessageW(ipCombo, CB_ADDSTRING, 0, address);
        }
        if (index >= 0 && SendMessageW(ipCombo, CB_GETCURSEL, 0, 0) != index)
            SendMessageW(ipCombo, CB_SETCURSEL, index, 0);
        SetTextIfChanged(Control(Port), std::to_wstring(server_->Port()));
    }

    // Update board guide tip
    auto displayIp = listening ? listeningIp : ControlText(ipCombo);
    if (displayIp.find(L"0.0.0.0") != std::wstring::npos) {
        auto ips = server_->LocalIps();
        displayIp = ips.empty() ? L"127.0.0.1" : Utf8ToWide(ips.front());
    }
    wchar_t nameBuf[128]{};
    GetWindowTextW(Control(TftpName), nameBuf, 128);
    std::wstring targetName = (nameBuf[0] != L'\0') ? nameBuf : L"Image";
    std::wstring boardTip = displayIp.rfind(L"127.", 0) == 0
        ? L"当前地址为本机回环，外部开发板无法连接。"
        : L"开发板设置参考：\r\nsetenv serverip " + displayIp + L"\r\ntftpboot ${loadaddr} " + targetName;
    SetTextIfChanged(Control(BoardTip), boardTip);

    // 2. Populate SSH sessions combo
    if (sshProvider_) {
        cachedSshSessions_ = sshProvider_();
        HWND sessCombo = Control(RemoteSession);
        std::wstring currentSel = ControlText(sessCombo);
        SendMessageW(sessCombo, CB_RESETCONTENT, 0, 0);
        int matchIdx = -1;
        for (size_t i = 0; i < cachedSshSessions_.size(); ++i) {
            const auto& s = cachedSshSessions_[i];
            std::wstring label = s.displayName + (s.connected ? L" (已连接)" : L" (未连接)");
            SendMessageW(sessCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (!currentSel.empty() && label == currentSel) matchIdx = static_cast<int>(i);
        }
        if (matchIdx >= 0) SendMessageW(sessCombo, CB_SETCURSEL, matchIdx, 0);
        else if (!cachedSshSessions_.empty()) SendMessageW(sessCombo, CB_SETCURSEL, 0, 0);
    }

    // 3. Published files list
    auto pubItems = server_->ListPublished();
    std::wstring pubStr;
    if (pubItems.empty()) {
        pubStr = L"(无已发布文件)";
    } else {
        for (const auto& item : pubItems) {
            auto info = item.source->Info();
            pubStr += Utf8ToWide(item.tftpName) + L" [" + Utf8ToWide(info.type) + L"]: ";
            if (info.sizeKnown) {
                pubStr += std::to_wstring(info.size) + L" 字节";
            } else {
                pubStr += L"大小未知";
            }
            pubStr += L" (" + Utf8ToWide(info.status) + L")\r\n  -> " + info.displayPath + L"\r\n";
        }
    }
    SetTextIfChanged(Control(PublishedList), pubStr);

    // 4. Transfers list
    auto trans = server_->ListTransfers();
    std::wstring transStr;
    bool hasActive = false;
    if (trans.empty()) {
        transStr = L"(无传输任务)";
    } else {
        for (auto it = trans.rbegin(); it != trans.rend(); ++it) {
            if (it->phase == "transferring" || it->phase == "connecting") hasActive = true;
            transStr += Utf8ToWide(it->tftpName) + L" <- " + Utf8ToWide(it->clientIp) + L":" +
                        std::to_wstring(it->clientPort) + L" [" + Utf8ToWide(it->phase) + L"]\r\n  " +
                        std::to_wstring(it->ackedBytes) + L" / " +
                        (it->totalBytes ? std::to_wstring(it->totalBytes) : L"?") + L" 字节";
            if (it->retransmits > 0) {
                transStr += L" (重传 " + std::to_wstring(it->retransmits) + L" 次)";
            }
            if (!it->error.empty()) {
                transStr += L"\r\n  错误: " + it->error;
            }
            transStr += L"\r\n";
        }
    }
    SetTextIfChanged(Control(TransfersStatus), transStr);
    EnableWindow(Control(CancelTransferBtn), hasActive);

    if (listening || hasActive) {
        SetTimer(window_, 1, 250, nullptr);
    } else {
        KillTimer(window_, 1);
    }

    InvalidateRect(body_, nullptr, FALSE);
}

void TftpPane::BrowseLocalFile() {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = window_;
    ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0镜像与二进制 (*.bin;*.img;*.uImage;Image;*.dtb)\0*.bin;*.img;*.uImage;Image;*.dtb\0\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(Control(LocalPath), filename);
        if (ControlText(Control(TftpName)).empty()) {
            SetWindowTextW(Control(TftpName), ExtractBasename(filename).c_str());
        }
        Refresh();
    }
}

void TftpPane::OnToggleService() {
    if (!server_) return;
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

void TftpPane::OnPublish() {
    if (!server_) return;
    localNotice_.clear();

    std::wstring nameW = ControlText(Control(TftpName));
    if (nameW.empty()) {
        localNotice_ = L"发布名不能为空";
        Refresh();
        return;
    }
    std::string tftpName = WideToUtf8(nameW);

    std::wstring err;
    if (selectedSourceType_ == 0) { // Local
        std::wstring path = ControlText(Control(LocalPath));
        if (path.empty()) {
            localNotice_ = L"请选择本地文件路径";
            Refresh();
            return;
        }
        if (!server_->PublishLocal(tftpName, path, true, err)) {
            localNotice_ = err;
        } else {
            localNotice_ = L"发布成功: " + nameW;
        }
    } else { // Remote SSH
        int sel = static_cast<int>(SendMessageW(Control(RemoteSession), CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(cachedSshSessions_.size())) {
            localNotice_ = L"请先在左侧选择或建立 SSH 连接";
            Refresh();
            return;
        }
        const auto& ssh = cachedSshSessions_[sel];
        std::wstring rpath = ControlText(Control(RemotePath));
        if (rpath.empty()) {
            localNotice_ = L"请输入远程 POSIX 路径";
            Refresh();
            return;
        }
        SshRemoteConfig cfg;
        cfg.host = ssh.host;
        cfg.port = ssh.port;
        cfg.username = ssh.username;
        cfg.password = ssh.password;
        cfg.remotePath = rpath;

        if (!server_->PublishRemote(tftpName, cfg, true, err)) {
            localNotice_ = err;
        } else {
            localNotice_ = L"远程发布成功: " + nameW;
        }
    }

    Refresh();
    if (stateChanged) stateChanged();
}

void TftpPane::OnUnpublish() {
    if (!server_) return;
    localNotice_.clear();
    std::wstring nameW = ControlText(Control(TftpName));
    if (nameW.empty()) {
        localNotice_ = L"请输入要取消的发布名";
        Refresh();
        return;
    }
    std::string name = WideToUtf8(nameW);
    if (server_->Unpublish(name)) {
        localNotice_ = L"已取消发布: " + nameW;
    } else {
        localNotice_ = L"未找到该发布项";
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
    if (contentHeight_ > bodyHeight_) {
        const auto c = UiTheme(dark_);
        const int thumb = std::max(D(28), bodyHeight_ * bodyHeight_ / std::max(1, contentHeight_));
        const int y = offset_ * (bodyHeight_ - thumb) / std::max(1, contentHeight_ - bodyHeight_);
        UiBox(dc, {rc.right - D(5), y, rc.right, y + thumb}, c.border, c.border, D(2));
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
            if (wParam) self->Refresh();
            else KillTimer(hwnd, 1);
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
    case WM_MEASUREITEM: {
        if (self) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = self->D(28);
            return TRUE;
        }
        break;
    }
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
            UiBufferedPaint paint(item->hDC, item->rcItem);
            auto dc = paint.Dc();
            const auto c = UiTheme(self->dark_);
            FillRect(dc, &item->rcItem, self->surface_);
            const bool enabled = IsWindowEnabled(item->hwndItem) != FALSE;
            const bool flat = GetPropW(item->hwndItem, L"SerialCtl.FlatIconVisual") != nullptr;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            const bool hovered = GetPropW(item->hwndItem, L"SerialCtl.PowerHover") != nullptr;
            const bool isListening = self->server_ && self->server_->IsListening();
            const bool isPrimary = (item->CtlID == ToggleService && !isListening) || item->CtlID == PublishBtn;
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
                auto r = item->rcItem;
                DrawTextW(dc, ControlText(item->hwndItem).c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
            else if (id == BrowseLocal && code == BN_CLICKED) self->BrowseLocalFile();
            else if (id == PublishBtn && code == BN_CLICKED) self->OnPublish();
            else if (id == UnpublishBtn && code == BN_CLICKED) self->OnUnpublish();
            else if (id == CancelTransferBtn && code == BN_CLICKED) self->OnCancelTransfer();
            else if (id == SourceType && code == CBN_SELCHANGE) {
                self->selectedSourceType_ = static_cast<int>(SendMessageW(self->Control(SourceType), CB_GETCURSEL, 0, 0));
                self->Layout();
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

} // namespace serialctl
