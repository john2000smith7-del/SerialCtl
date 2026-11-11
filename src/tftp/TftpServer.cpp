#include "TftpServer.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>
#include <sstream>

namespace serialctl {

static bool ParseSafeRelativeTftpPath(const std::string& raw, std::vector<std::string>& segments,
                                      std::string& normalizedName) {
    segments.clear();
    normalizedName.clear();
    if (raw.empty()) return false;
    for (unsigned char ch : raw) {
        if (ch < 32 || ch == ':') return false;
    }
    if (raw.front() == '/' || raw.front() == '\\' || raw.back() == '/' || raw.back() == '\\') {
        return false;
    }
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t sep = raw.find_first_of("/\\", pos);
        const std::string seg = (sep == std::string::npos) ? raw.substr(pos) : raw.substr(pos, sep - pos);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') {
            segments.clear();
            return false;
        }
        segments.push_back(seg);
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) normalizedName += '/';
        normalizedName += segments[i];
    }
    return !segments.empty();
}

static std::string WideToAscii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128 ? static_cast<char>(c) : '?');
    return s;
}

static void SendTftpError(SOCKET s, const sockaddr_in& to, uint16_t code, const std::string& msg) {
    std::vector<uint8_t> pkt(4 + msg.size() + 1);
    pkt[0] = 0; pkt[1] = 5; // opcode ERROR
    pkt[2] = static_cast<uint8_t>(code >> 8);
    pkt[3] = static_cast<uint8_t>(code & 0xFF);
    std::memcpy(pkt.data() + 4, msg.c_str(), msg.size() + 1);
    sendto(s, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

class TftpTransferSession : public std::enable_shared_from_this<TftpTransferSession> {
public:
    TftpTransferSession(uint64_t id, SOCKET s, sockaddr_in clientAddr, std::string tftpName,
                        std::shared_ptr<ITftpFileSource> source, size_t blksize,
                        uint32_t timeoutSec, bool tsizeRequested,
                        std::vector<std::pair<std::string, std::string>> oackOptions)
        : id_(id), socket_(s), clientAddr_(clientAddr), tftpName_(std::move(tftpName)),
          source_(std::move(source)), blksize_(blksize), timeoutSec_(timeoutSec),
          tsizeRequested_(tsizeRequested), oackOptions_(std::move(oackOptions)) {
        startTimeMs_ = GetTickCount64();
        totalBytes_ = source_->Info().sizeKnown ? source_->Info().size : 0;
        phase_ = "connecting";
    }

    ~TftpTransferSession() {
        Cancel();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    void Start() {
        workerThread_ = std::thread(&TftpTransferSession::Run, this);
    }

    void Cancel() {
        cancelled_ = true;
        if (source_) {
            source_->Cancel();
        }
    }

    bool IsActive() const {
        std::lock_guard<std::mutex> lock(statusMutex_);
        return phase_ == "connecting" || phase_ == "transferring";
    }

    TftpTransferStatus Status() const {
        std::lock_guard<std::mutex> lock(statusMutex_);
        TftpTransferStatus st;
        st.transferId = id_;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr_.sin_addr, ip, sizeof(ip));
        st.clientIp = ip;
        st.clientPort = ntohs(clientAddr_.sin_port);
        st.tftpName = tftpName_;
        st.phase = phase_;
        st.sentBytes = sentBytes_;
        st.ackedBytes = ackedBytes_;
        st.totalBytes = totalBytes_;
        st.retransmits = retransmits_;
        st.error = error_;
        st.startTimeMs = startTimeMs_;
        st.finishTimeMs = finishTimeMs_;

        uint64_t now = finishTimeMs_ ? finishTimeMs_ : GetTickCount64();
        uint64_t elapsedMs = (now > startTimeMs_) ? (now - startTimeMs_) : 1;
        st.speedBps = static_cast<double>(ackedBytes_) * 1000.0 / static_cast<double>(elapsedMs);
        return st;
    }

    uint64_t Id() const { return id_; }

private:
    void Run() {
        std::wstring err;
        if (!source_->Open(err)) {
            const auto info = source_->Info();
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                phase_ = "failed";
                error_ = err.empty() ? L"无法打开文件源" : err;
                finishTimeMs_ = GetTickCount64();
            }
            if (info.notFound) {
                SendTftpError(socket_, clientAddr_, 1, "File not found: " + tftpName_);
            } else {
                SendTftpError(socket_, clientAddr_, 2, "Access violation: open source failed");
            }
            return;
        }

        {
            const auto info = source_->Info();
            std::lock_guard<std::mutex> lock(statusMutex_);
            if (info.sizeKnown) {
                totalBytes_ = info.size;
                if (tsizeRequested_) {
                    const bool hasTsize = std::any_of(
                        oackOptions_.begin(), oackOptions_.end(),
                        [](const auto& opt) { return opt.first == "tsize"; });
                    if (!hasTsize) {
                        oackOptions_.emplace_back("tsize", std::to_string(info.size));
                    }
                }
            }
            phase_ = "transferring";
        }

        // If options were negotiated, send OACK and wait for ACK 0
        if (!oackOptions_.empty()) {
            std::vector<uint8_t> oackPkt;
            oackPkt.push_back(0); oackPkt.push_back(6); // opcode OACK
            for (const auto& opt : oackOptions_) {
                for (char c : opt.first) oackPkt.push_back(static_cast<uint8_t>(c));
                oackPkt.push_back(0);
                for (char c : opt.second) oackPkt.push_back(static_cast<uint8_t>(c));
                oackPkt.push_back(0);
            }

            bool oackAcked = false;
            uint32_t retries = 0;
            while (!cancelled_ && !oackAcked) {
                sendto(socket_, reinterpret_cast<const char*>(oackPkt.data()),
                       static_cast<int>(oackPkt.size()), 0,
                       reinterpret_cast<const sockaddr*>(&clientAddr_), sizeof(clientAddr_));

                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(socket_, &fds);
                timeval tv{};
                tv.tv_sec = timeoutSec_ ? timeoutSec_ : 2;
                tv.tv_usec = 0;

                int sel = select(0, &fds, nullptr, nullptr, &tv);
                if (sel <= 0) {
                    retries++;
                    {
                        std::lock_guard<std::mutex> lock(statusMutex_);
                        retransmits_++;
                    }
                    if (retries > 5) {
                        std::lock_guard<std::mutex> lock(statusMutex_);
                        phase_ = "failed";
                        error_ = L"OACK 确认超时";
                        finishTimeMs_ = GetTickCount64();
                        source_->Close();
                        return;
                    }
                    continue;
                }

                sockaddr_in from{};
                int fromLen = sizeof(from);
                uint8_t reply[512];
                int recvd = recvfrom(socket_, reinterpret_cast<char*>(reply), sizeof(reply), 0,
                                     reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (recvd >= 4) {
                    uint16_t op = (reply[0] << 8) | reply[1];
                    if (op == 4) { // ACK
                        uint16_t blk = (reply[2] << 8) | reply[3];
                        if (blk == 0) {
                            oackAcked = true;
                            break;
                        }
                    } else if (op == 5) { // ERROR
                        std::lock_guard<std::mutex> lock(statusMutex_);
                        phase_ = "cancelled";
                        error_ = L"客户端取消传输";
                        finishTimeMs_ = GetTickCount64();
                        source_->Close();
                        return;
                    }
                }
            }
        }

        // Data transfer loop
        uint64_t currentOffset = 0;
        uint32_t blockCount = 1;
        std::vector<uint8_t> readBuf(blksize_);
        bool finished = false;

        while (!finished && !cancelled_) {
            size_t bytesRead = 0;
            if (!source_->Read(currentOffset, readBuf.data(), blksize_, bytesRead, err)) {
                std::lock_guard<std::mutex> lock(statusMutex_);
                phase_ = "failed";
                error_ = err.empty() ? L"读取文件源失败" : err;
                finishTimeMs_ = GetTickCount64();
                SendTftpError(socket_, clientAddr_, 2, "Read error: " + WideToAscii(err));
                source_->Close();
                return;
            }

            // Construct DATA packet
            uint16_t blk16 = static_cast<uint16_t>(blockCount & 0xFFFF);
            std::vector<uint8_t> dataPkt(4 + bytesRead);
            dataPkt[0] = 0; dataPkt[1] = 3; // opcode DATA
            dataPkt[2] = static_cast<uint8_t>(blk16 >> 8);
            dataPkt[3] = static_cast<uint8_t>(blk16 & 0xFF);
            if (bytesRead > 0) {
                std::memcpy(dataPkt.data() + 4, readBuf.data(), bytesRead);
            }

            bool blockAcked = false;
            uint32_t retries = 0;

            while (!cancelled_ && !blockAcked) {
                sendto(socket_, reinterpret_cast<const char*>(dataPkt.data()),
                       static_cast<int>(dataPkt.size()), 0,
                       reinterpret_cast<const sockaddr*>(&clientAddr_), sizeof(clientAddr_));
                {
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    sentBytes_ = currentOffset + bytesRead;
                }

                while (!cancelled_ && !blockAcked) {
                    fd_set fds;
                    FD_ZERO(&fds);
                    FD_SET(socket_, &fds);
                    timeval tv{};
                    tv.tv_sec = timeoutSec_ ? timeoutSec_ : 2;
                    tv.tv_usec = 0;

                    int sel = select(0, &fds, nullptr, nullptr, &tv);
                    if (sel <= 0) {
                        retries++;
                        {
                            std::lock_guard<std::mutex> lock(statusMutex_);
                            retransmits_++;
                        }
                        if (retries > 5) {
                            std::lock_guard<std::mutex> lock(statusMutex_);
                            phase_ = "failed";
                            error_ = L"等待 ACK 超时 (超过5次重试)";
                            finishTimeMs_ = GetTickCount64();
                            source_->Close();
                            return;
                        }
                        break; // Timeout: retransmit data block
                    }

                    sockaddr_in from{};
                    int fromLen = sizeof(from);
                    uint8_t reply[512];
                    int recvd = recvfrom(socket_, reinterpret_cast<char*>(reply), sizeof(reply), 0,
                                         reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (recvd >= 4) {
                        if (from.sin_addr.s_addr != clientAddr_.sin_addr.s_addr ||
                            from.sin_port != clientAddr_.sin_port) {
                            SendTftpError(socket_, from, 5, "Unknown transfer ID");
                            continue;
                        }

                        uint16_t op = (reply[0] << 8) | reply[1];
                        if (op == 4) { // ACK
                            uint16_t ackBlk = (reply[2] << 8) | reply[3];
                            // Support wrap-around: 16-bit block numbers wrap around naturally
                            bool match = (ackBlk == blk16);

                            if (match) {
                                blockAcked = true;
                                currentOffset += bytesRead;
                                {
                                    std::lock_guard<std::mutex> lock(statusMutex_);
                                    ackedBytes_ = currentOffset;
                                }
                                blockCount++;

                                // If payload was less than blksize, this was the final block!
                                if (bytesRead < blksize_) {
                                    if (totalBytes_ > 0 && currentOffset != totalBytes_) {
                                        std::lock_guard<std::mutex> lock(statusMutex_);
                                        phase_ = "failed";
                                        error_ = L"传输提前中断：读取字节数未达到文件总大小 (premature EOF)";
                                        finishTimeMs_ = GetTickCount64();
                                        SendTftpError(socket_, clientAddr_, 0, "Premature EOF: incomplete file transfer");
                                        source_->Close();
                                        return;
                                    }
                                    finished = true;
                                    std::lock_guard<std::mutex> lock(statusMutex_);
                                    phase_ = "completed";
                                    finishTimeMs_ = GetTickCount64();
                                }
                                break;
                            } else {
                                // Duplicate or previous ACK: ignore per RFC 1350 and keep waiting without retransmitting
                                continue;
                            }
                        } else if (op == 5) { // ERROR
                            std::string errMsg;
                            if (recvd > 4) {
                                errMsg = std::string(reinterpret_cast<char*>(reply + 4), recvd - 4);
                            }
                            std::lock_guard<std::mutex> lock(statusMutex_);
                            phase_ = "cancelled";
                            error_ = errMsg.empty() ? L"客户端发送 ERROR 取消" :
                                std::wstring(errMsg.begin(), errMsg.end());
                            finishTimeMs_ = GetTickCount64();
                            source_->Close();
                            return;
                        }
                    }
                }
            }
        }

        source_->Close();
        if (cancelled_ && phase_ != "completed") {
            std::lock_guard<std::mutex> lock(statusMutex_);
            phase_ = "cancelled";
            error_ = L"用户取消传输";
            finishTimeMs_ = GetTickCount64();
        }
    }

    uint64_t id_ = 0;
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in clientAddr_{};
    std::string tftpName_;
    std::shared_ptr<ITftpFileSource> source_;
    size_t blksize_ = 512;
    uint32_t timeoutSec_ = 2;
    bool tsizeRequested_ = false;
    std::vector<std::pair<std::string, std::string>> oackOptions_;

    std::thread workerThread_;
    std::atomic<bool> cancelled_{false};

    mutable std::mutex statusMutex_;
    std::string phase_;
    uint64_t sentBytes_ = 0;
    uint64_t ackedBytes_ = 0;
    uint64_t totalBytes_ = 0;
    uint32_t retransmits_ = 0;
    std::wstring error_;
    uint64_t startTimeMs_ = 0;
    uint64_t finishTimeMs_ = 0;
};

// ============================================================================
// TftpServer
// ============================================================================

TftpServer::TftpServer() = default;

TftpServer::~TftpServer() {
    Stop();
}

std::vector<std::string> TftpServer::EnumerateLocalIps() {
    std::vector<std::string> ips;
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                if (p->ai_family == AF_INET && p->ai_addr) {
                    auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    char ip[INET_ADDRSTRLEN] = {};
                    if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) {
                        if (std::strcmp(ip, "127.0.0.1") != 0 &&
                            std::find(ips.begin(), ips.end(), std::string(ip)) == ips.end()) {
                            ips.push_back(ip);
                        }
                    }
                }
            }
            freeaddrinfo(res);
        }
    }
    if (ips.empty()) {
        ips.push_back("127.0.0.1");
    }
    return ips;
}

std::vector<std::string> TftpServer::LocalIps() const {
    return EnumerateLocalIps();
}

bool TftpServer::Start(const std::string& bindIp, uint16_t port, std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        error = L"TFTP 服务已经在运行";
        return false;
    }

    const auto requestedIp = bindIp.empty() ? "0.0.0.0" : bindIp;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (InetPtonA(AF_INET, requestedIp.c_str(), &addr.sin_addr) != 1) {
        error = L"无效的 TFTP 监听 IPv4 地址。";
        return false;
    }

    bindIp_ = requestedIp;
    port_ = port ? port : 69;

    listener_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (listener_ == INVALID_SOCKET) {
        error = L"创建 UDP 套接字失败: " + std::to_wstring(WSAGetLastError());
        return false;
    }

    addr.sin_port = htons(port_);

    if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
        if (err == WSAEADDRINUSE) {
            error = L"端口 " + std::to_wstring(port_) + L" 已被占用。请在设置中修改端口或关闭冲突服务。";
        } else if (err == WSAEACCES) {
            error = L"端口 " + std::to_wstring(port_) + L" 权限不足（标准 69 端口需要管理员权限，建议以管理员运行或改用非特权端口如 1069/6969）。";
        } else {
            error = L"绑定 UDP 端口失败: 错误代码 " + std::to_wstring(err);
        }
        return false;
    }

    running_ = true;
    stopping_ = false;
    listenerThread_ = std::thread(&TftpServer::ListenerThread, this);
    return true;
}

void TftpServer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        stopping_ = true;
        running_ = false;
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        for (auto& session : sessions_) {
            session->Cancel();
        }
    }

    if (listenerThread_.joinable()) {
        listenerThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    CleanCompletedSessions();
}

bool TftpServer::IsListening() const {
    return running_.load();
}

std::string TftpServer::State() const {
    if (!running_) return "stopped";
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : sessions_) {
        if (s->IsActive()) return "transferring";
    }
    return "listening";
}

std::string TftpServer::BindIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindIp_;
}

uint16_t TftpServer::Port() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

bool TftpServer::PublishLocal(const std::string& tftpName, const std::wstring& localPath,
                             bool allowOverwrite, std::wstring& error) {
    if (tftpName.empty()) {
        error = L"发布名不能为空";
        return false;
    }
    if (tftpName.find("..") != std::string::npos || tftpName.find('/') != std::string::npos ||
        tftpName.find('\\') != std::string::npos || tftpName.find(':') != std::string::npos) {
        error = L"发布名不能包含路径分隔符或 '..'";
        return false;
    }

    auto source = std::make_shared<LocalTftpFileSource>(localPath);
    if (source->Info().status != "Ready") {
        error = source->Info().error.empty() ? L"本地文件无法读取" : source->Info().error;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!allowOverwrite && publishedFiles_.find(tftpName) != publishedFiles_.end()) {
        error = L"TFTP 发布名已存在: " + std::wstring(tftpName.begin(), tftpName.end());
        return false;
    }

    publishedFiles_[tftpName] = source;
    return true;
}

bool TftpServer::PublishRemote(const std::string& tftpName, const SshRemoteConfig& config,
                              bool allowOverwrite, std::wstring& error) {
    if (tftpName.empty()) {
        error = L"发布名不能为空";
        return false;
    }
    if (tftpName.find("..") != std::string::npos || tftpName.find('/') != std::string::npos ||
        tftpName.find('\\') != std::string::npos || tftpName.find(':') != std::string::npos) {
        error = L"发布名不能包含路径分隔符或 '..'";
        return false;
    }

    auto source = std::make_shared<RemoteSshTftpFileSource>(config);
    if (source->Info().status != "Ready") {
        error = source->Info().error.empty() ? L"远程文件无法访问" : source->Info().error;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!allowOverwrite && publishedFiles_.find(tftpName) != publishedFiles_.end()) {
        error = L"TFTP 发布名已存在: " + std::wstring(tftpName.begin(), tftpName.end());
        return false;
    }

    publishedFiles_[tftpName] = source;
    return true;
}

bool TftpServer::PublishCustom(const std::string& tftpName, std::shared_ptr<ITftpFileSource> source,
                               bool allowOverwrite, std::wstring& error) {
    if (tftpName.empty() || !source) {
        error = L"发布名或数据源无效";
        return false;
    }
    if (tftpName.find("..") != std::string::npos || tftpName.find('/') != std::string::npos ||
        tftpName.find('\\') != std::string::npos || tftpName.find(':') != std::string::npos) {
        error = L"发布名不能包含路径分隔符或 '..'";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!allowOverwrite && publishedFiles_.find(tftpName) != publishedFiles_.end()) {
        error = L"TFTP 发布名已存在: " + std::wstring(tftpName.begin(), tftpName.end());
        return false;
    }

    publishedFiles_[tftpName] = std::move(source);
    return true;
}

bool TftpServer::Unpublish(const std::string& tftpName) {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishedFiles_.erase(tftpName) > 0;
}

std::vector<TftpPublishedItem> TftpServer::ListPublished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TftpPublishedItem> list;
    list.reserve(publishedFiles_.size());
    for (const auto& pair : publishedFiles_) {
        list.push_back({pair.first, pair.second});
    }
    return list;
}

size_t TftpServer::PublishedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishedFiles_.size();
}

bool TftpServer::SetLocalRoot(const std::wstring& localDirectory, std::wstring& error) {
    std::wstring normalized;
    if (!ValidateLocalTftpDirectory(localDirectory, normalized, error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    rootConfig_.mode = TftpRootMode::Local;
    rootConfig_.localDirectory = std::move(normalized);
    rootConfig_.remoteConfig = {};
    return true;
}

bool TftpServer::SetRemoteRoot(const SshRemoteConfig& config, std::wstring& error) {
    SshRemoteConfig cfg = config;
    if (!ValidateRemoteTftpDirectoryConfig(cfg, error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    rootConfig_.mode = TftpRootMode::Remote;
    rootConfig_.localDirectory.clear();
    rootConfig_.remoteConfig = std::move(cfg);
    return true;
}

void TftpServer::ClearRoot() {
    std::lock_guard<std::mutex> lock(mutex_);
    rootConfig_ = {};
}

TftpRootConfig TftpServer::RootConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rootConfig_;
}

std::vector<TftpTransferStatus> TftpServer::ListTransfers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TftpTransferStatus> list;
    list.reserve(sessions_.size());
    for (const auto& s : sessions_) {
        list.push_back(s->Status());
    }
    return list;
}

bool TftpServer::CancelTransfer(uint64_t transferId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : sessions_) {
        if (s->Id() == transferId) {
            s->Cancel();
            return true;
        }
    }
    return false;
}

size_t TftpServer::ActiveTransferCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& s : sessions_) {
        if (s->IsActive()) count++;
    }
    return count;
}

void TftpServer::CleanCompletedSessions() {
    // Keep up to 64 recent completed sessions for history/diagnostics
    size_t activeCount = 0;
    for (const auto& s : sessions_) {
        if (s->IsActive()) activeCount++;
    }
    if (sessions_.size() > activeCount + 64) {
        auto it = std::remove_if(sessions_.begin(), sessions_.end(),
            [](const std::shared_ptr<TftpTransferSession>& s) {
                return !s->IsActive();
            });
        sessions_.erase(it, sessions_.end());
    }
}

void TftpServer::ListenerThread() {
    std::vector<uint8_t> packet(2048);

    while (running_) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);

        int recvd = recvfrom(listener_, reinterpret_cast<char*>(packet.data()),
                             static_cast<int>(packet.size()), 0,
                             reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (recvd < 4) {
            if (!running_) break;
            continue;
        }

        uint16_t opcode = (packet[0] << 8) | packet[1];

        // Security check: WRQ strictly prohibited
        if (opcode == 2) { // WRQ
            SendTftpError(listener_, clientAddr, 2, "Read-only TFTP server");
            continue;
        }

        if (opcode != 1) { // RRQ
            SendTftpError(listener_, clientAddr, 4, "Illegal TFTP operation: Expected RRQ");
            continue;
        }

        // Parse RRQ: filename\0mode\0[opt1\0val1\0...]
        const char* p = reinterpret_cast<const char*>(packet.data() + 2);
        const char* end = reinterpret_cast<const char*>(packet.data() + recvd);

        const char* null1 = static_cast<const char*>(std::memchr(p, '\0', end - p));
        if (!null1) {
            SendTftpError(listener_, clientAddr, 4, "Malformed RRQ filename");
            continue;
        }
        std::string filename(p, null1 - p);

        p = null1 + 1;
        if (p >= end) {
            SendTftpError(listener_, clientAddr, 4, "Malformed RRQ mode");
            continue;
        }
        const char* null2 = static_cast<const char*>(std::memchr(p, '\0', end - p));
        if (!null2) {
            SendTftpError(listener_, clientAddr, 4, "Malformed RRQ mode");
            continue;
        }
        std::string mode(p, null2 - p);
        p = null2 + 1;

        // Security boundary: filename validation
        std::vector<std::string> segments;
        std::string normalizedName;
        if (!ParseSafeRelativeTftpPath(filename, segments, normalizedName)) {
            SendTftpError(listener_, clientAddr, 2, "Access violation: invalid filename or path traversal");
            continue;
        }

        std::shared_ptr<ITftpFileSource> source;
        TftpRootConfig rootSnapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = publishedFiles_.find(normalizedName);
            if (it != publishedFiles_.end()) {
                source = it->second;
            } else {
                rootSnapshot = rootConfig_;
            }
        }

        if (!source && rootSnapshot.mode != TftpRootMode::None) {
            if (rootSnapshot.mode == TftpRootMode::Local && !rootSnapshot.localDirectory.empty()) {
                std::filesystem::path targetPath(rootSnapshot.localDirectory);
                bool validSegments = true;
                for (const auto& seg : segments) {
                    const auto wseg = MultiByteToWide(
                        reinterpret_cast<const uint8_t*>(seg.data()), seg.size(), CP_UTF8);
                    if (wseg.empty()) {
                        validSegments = false;
                        break;
                    }
                    targetPath /= wseg;
                }
                if (validSegments) {
                    source = std::make_shared<LocalTftpFileSource>(targetPath.native());
                }
            } else if (rootSnapshot.mode == TftpRootMode::Remote && !rootSnapshot.remoteConfig.remotePath.empty()) {
                SshRemoteConfig cfg = rootSnapshot.remoteConfig;
                bool validSegments = true;
                for (const auto& seg : segments) {
                    const auto wseg = MultiByteToWide(
                        reinterpret_cast<const uint8_t*>(seg.data()), seg.size(), CP_UTF8);
                    if (wseg.empty()) {
                        validSegments = false;
                        break;
                    }
                    if (cfg.remotePath != L"/") cfg.remotePath += L"/";
                    cfg.remotePath += wseg;
                }
                if (validSegments) {
                    source = std::make_shared<RemoteSshTftpFileSource>(std::move(cfg), nullptr, true);
                }
            }
        }

        if (!source) {
            SendTftpError(listener_, clientAddr, 1, "File not found: " + normalizedName);
            continue;
        }

        const auto sourceInfo = source->Info();
        if (sourceInfo.status != "Ready" && sourceInfo.status != "Pending") {
            if (sourceInfo.notFound) {
                SendTftpError(listener_, clientAddr, 1, "File not found: " + normalizedName);
            } else {
                SendTftpError(listener_, clientAddr, 2, "Access violation: source unavailable");
            }
            continue;
        }

        // Parse options (RFC 2347, 2348, 2349)
        size_t blksize = 512;
        uint32_t timeoutSec = 2;
        bool tsizeRequested = false;
        std::vector<std::pair<std::string, std::string>> oackOptions;

        while (p < end) {
            const char* optEnd = static_cast<const char*>(std::memchr(p, '\0', end - p));
            if (!optEnd) break;
            std::string optName(p, optEnd - p);
            p = optEnd + 1;
            if (p >= end) break;
            const char* valEnd = static_cast<const char*>(std::memchr(p, '\0', end - p));
            if (!valEnd) break;
            std::string optVal(p, valEnd - p);
            p = valEnd + 1;

            std::string lowerOpt = optName;
            std::transform(lowerOpt.begin(), lowerOpt.end(), lowerOpt.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowerOpt == "blksize") {
                try {
                    int bs = std::stoi(optVal);
                    if (bs >= 512) {
                        blksize = std::clamp(bs, 512, 65464);
                        oackOptions.emplace_back("blksize", std::to_string(blksize));
                    }
                } catch (...) {}
            } else if (lowerOpt == "tsize") {
                tsizeRequested = true;
                if (sourceInfo.sizeKnown) {
                    oackOptions.emplace_back("tsize", std::to_string(sourceInfo.size));
                }
            } else if (lowerOpt == "timeout") {
                try {
                    int to = std::stoi(optVal);
                    if (to >= 1 && to <= 60) {
                        timeoutSec = static_cast<uint32_t>(to);
                        oackOptions.emplace_back("timeout", std::to_string(timeoutSec));
                    }
                } catch (...) {}
            }
        }

        // Enforce maximum concurrent active sessions
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CleanCompletedSessions();
            size_t activeCount = 0;
            for (const auto& s : sessions_) {
                if (s->IsActive()) activeCount++;
            }
            if (activeCount >= kMaxConcurrentTftpSessions) {
                SendTftpError(listener_, clientAddr, 0, "Server busy: too many concurrent transfers");
                continue;
            }
        }

        // Create ephemeral TID socket
        SOCKET transferSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (transferSocket == INVALID_SOCKET) {
            SendTftpError(listener_, clientAddr, 0, "Server failed to allocate transfer socket");
            continue;
        }

        sockaddr_in transferAddr{};
        transferAddr.sin_family = AF_INET;
        InetPtonA(AF_INET, bindIp_.c_str(), &transferAddr.sin_addr);
        transferAddr.sin_port = 0; // ephemeral port

        if (bind(transferSocket, reinterpret_cast<sockaddr*>(&transferAddr), sizeof(transferAddr)) == SOCKET_ERROR) {
            closesocket(transferSocket);
            SendTftpError(listener_, clientAddr, 0, "Server failed to bind transfer socket");
            continue;
        }

        uint64_t transferId = 0;
        std::shared_ptr<TftpTransferSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            CleanCompletedSessions();
            transferId = nextTransferId_++;
            auto sessionSource = source->CreateSessionSource();
            session = std::make_shared<TftpTransferSession>(
                transferId, transferSocket, clientAddr, normalizedName,
                sessionSource ? sessionSource : source, blksize, timeoutSec,
                tsizeRequested, std::move(oackOptions));
            sessions_.push_back(session);
        }

        session->Start();
    }
}

} // namespace serialctl
