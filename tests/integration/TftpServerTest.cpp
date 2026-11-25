#include "TftpFileSource.h"
#include "TftpServer.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#define Require(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "REQUIRE FAILED: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        std::exit(1); \
    } \
} while(0)

using namespace serialctl;

namespace {

class TftpClientSimulator {
public:
    TftpClientSimulator() {
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local));

        DWORD timeoutMs = 3000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        int bufSize = 1048576;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    }

    ~TftpClientSimulator() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
        }
    }

    void SendRrq(uint16_t serverPort, const std::string& filename,
                 const std::vector<std::pair<std::string, std::string>>& options = {}) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(serverPort);

        std::vector<uint8_t> pkt;
        pkt.push_back(0); pkt.push_back(1); // RRQ
        for (char c : filename) pkt.push_back(static_cast<uint8_t>(c));
        pkt.push_back(0);
        const std::string mode = "octet";
        for (char c : mode) pkt.push_back(static_cast<uint8_t>(c));
        pkt.push_back(0);

        for (const auto& opt : options) {
            for (char c : opt.first) pkt.push_back(static_cast<uint8_t>(c));
            pkt.push_back(0);
            for (char c : opt.second) pkt.push_back(static_cast<uint8_t>(c));
            pkt.push_back(0);
        }

        sendto(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    int RecvPacket(std::vector<uint8_t>& buf, sockaddr_in& from) {
        buf.resize(65536);
        int fromLen = sizeof(from);
        int recvd = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (recvd > 0) {
            buf.resize(recvd);
        } else {
            buf.clear();
        }
        return recvd;
    }

    void SendAck(const sockaddr_in& to, uint16_t blockNum) {
        uint8_t pkt[4]{};
        pkt[0] = 0; pkt[1] = 4; // ACK
        pkt[2] = static_cast<uint8_t>(blockNum >> 8);
        pkt[3] = static_cast<uint8_t>(blockNum & 0xFF);
        sendto(sock_, reinterpret_cast<const char*>(pkt), 4, 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    bool DownloadFile(uint16_t serverPort, const std::string& filename,
                      size_t blksize, std::vector<uint8_t>& outData,
                      bool useOptions = false, uint64_t* outTsize = nullptr) {
        outData.clear();
        std::vector<std::pair<std::string, std::string>> opts;
        if (useOptions) {
            opts.emplace_back("blksize", std::to_string(blksize));
            opts.emplace_back("tsize", "0");
        }

        SendRrq(serverPort, filename, opts);

        sockaddr_in serverTid{};
        std::vector<uint8_t> pkt;
        int recvd = RecvPacket(pkt, serverTid);
        if (recvd < 4) return false;

        uint16_t opcode = (pkt[0] << 8) | pkt[1];
        if (opcode == 5) return false; // ERROR

        uint32_t expectedBlock = 1;

        if (opcode == 6) { // OACK
            const char* p = reinterpret_cast<const char*>(pkt.data() + 2);
            const char* end = reinterpret_cast<const char*>(pkt.data() + recvd);
            while (p < end) {
                const char* oEnd = static_cast<const char*>(std::memchr(p, '\0', end - p));
                if (!oEnd) break;
                std::string optName(p, oEnd - p);
                p = oEnd + 1;
                if (p >= end) break;
                const char* vEnd = static_cast<const char*>(std::memchr(p, '\0', end - p));
                if (!vEnd) break;
                std::string optVal(p, vEnd - p);
                p = vEnd + 1;

                if (optName == "blksize") blksize = std::stoul(optVal);
                if (optName == "tsize" && outTsize) *outTsize = std::stoull(optVal);
            }

            SendAck(serverTid, 0);

            recvd = RecvPacket(pkt, serverTid);
            if (recvd < 4) return false;
            opcode = (pkt[0] << 8) | pkt[1];
        }

        while (true) {
            if (opcode != 3) return false; // expected DATA
            uint16_t block = (pkt[2] << 8) | pkt[3];
            if (block == static_cast<uint16_t>(expectedBlock & 0xFFFF)) {
                size_t payloadLen = recvd - 4;
                if (payloadLen > 0) {
                    outData.insert(outData.end(), pkt.begin() + 4, pkt.end());
                }
                SendAck(serverTid, block);
                expectedBlock++;

                if (payloadLen < blksize) {
                    return true;
                }
            }

            recvd = RecvPacket(pkt, serverTid);
            if (recvd < 4) return false;
            opcode = (pkt[0] << 8) | pkt[1];
        }
    }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

struct TempDirGuard {
    std::filesystem::path dir;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    Require(out.is_open(), "Open temp test file for writing");
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    Require(out.good(), "Write temp test file contents");
}

bool HasVisibleTftpd32WindowOnInteractiveDesktop() {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return false;

    using PFN_EnumWindows = BOOL(WINAPI*)(WNDENUMPROC, LPARAM);
    using PFN_IsWindowVisible = BOOL(WINAPI*)(HWND);
    using PFN_GetWindowTextW = int(WINAPI*)(HWND, LPWSTR, int);
    using PFN_GetWindowThreadProcessId = DWORD(WINAPI*)(HWND, LPDWORD);

    const auto pEnumWindows = reinterpret_cast<PFN_EnumWindows>(GetProcAddress(user32, "EnumWindows"));
    const auto pIsWindowVisible = reinterpret_cast<PFN_IsWindowVisible>(GetProcAddress(user32, "IsWindowVisible"));
    const auto pGetWindowTextW = reinterpret_cast<PFN_GetWindowTextW>(GetProcAddress(user32, "GetWindowTextW"));
    const auto pGetWindowThreadProcessId =
        reinterpret_cast<PFN_GetWindowThreadProcessId>(GetProcAddress(user32, "GetWindowThreadProcessId"));

    bool found = false;
    if (pEnumWindows && pIsWindowVisible && pGetWindowTextW && pGetWindowThreadProcessId) {
        struct Context {
            PFN_IsWindowVisible isVisible;
            PFN_GetWindowTextW getText;
            PFN_GetWindowThreadProcessId getPid;
            bool* found;
        } ctx{pIsWindowVisible, pGetWindowTextW, pGetWindowThreadProcessId, &found};

        pEnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                const auto* c = reinterpret_cast<const Context*>(lp);
                if (!c->isVisible(hwnd)) return TRUE;
                DWORD pid = 0;
                c->getPid(hwnd, &pid);
                if (pid == 0) return TRUE;
                wchar_t title[256]{};
                if (c->getText(hwnd, title, 256) > 0 && std::wstring(title).find(L"Tftpd32") != std::wstring::npos) {
                    *c->found = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
    }

    FreeLibrary(user32);
    return found;
}

void TestBindAddressValidationAndRestart(uint16_t testPort) {
    std::cout << "[RUN] Testing bind address validation and tftp32 lifecycle..." << std::endl;
    TftpServer server;
    std::wstring error;

    const auto localIps = TftpServer::EnumerateLocalIps();
    const auto defaultIp = TftpServer::DefaultLocalIp();
    std::cout << "[INFO] DefaultLocalIp=" << defaultIp << " (candidates=" << localIps.size() << ")" << std::endl;
    Require(!localIps.empty(), "EnumerateLocalIps must return at least one IPv4 address");
    Require(defaultIp == localIps.front(), "DefaultLocalIp must match top-ranked EnumerateLocalIps entry");
    Require(server.DefaultIp() == defaultIp, "TftpServer::DefaultIp must match DefaultLocalIp");
    for (const auto& ip : localIps) {
        sockaddr_in sa{};
        Require(ip != "0.0.0.0" && InetPtonA(AF_INET, ip.c_str(), &sa.sin_addr) == 1,
                "EnumerateLocalIps must contain only valid non-wildcard IPv4 addresses");
    }

    Require(!server.Start("invalid-bind-address", testPort, error) && !server.IsListening() && !error.empty(),
            "Invalid IPv4 must be rejected without launching tftp32");
    Require(server.State() == "stopped", "State must remain stopped after invalid Start");

    Require(server.Start("127.0.0.1", testPort, error), "Loopback start must succeed");
    Require(server.IsListening() && server.State() == "listening", "Server must report listening");
    Require(server.BindIp() == "127.0.0.1" && server.Port() == testPort, "Server must report configured IP and port");
    Sleep(200);
    Require(!HasVisibleTftpd32WindowOnInteractiveDesktop(),
            "tftp32.exe must run silently without showing any window on the interactive desktop");

    std::wstring duplicateErr;
    Require(!server.Start("127.0.0.1", testPort, duplicateErr), "Second Start while listening must be rejected");

    server.Stop();
    Require(!server.IsListening() && server.State() == "stopped", "Stop must terminate tftp32");

    // Verify the port was cleanly released by starting again on the same port.
    Require(server.Start("127.0.0.1", testPort, error), "Restart on the same port after Stop must succeed");
    server.Stop();
    std::cout << "[PASS] Bind address validation and tftp32 lifecycle verified." << std::endl;
}

void TestLocalDirectoryServing(uint16_t testPort) {
    std::cout << "[RUN] Testing tftp32 local directory serving, options, concurrency, and traversal guard..." << std::endl;
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / (L"serialctl_tftp32_test_" + std::to_wstring(GetCurrentProcessId()));
    const auto root = base / L"root";
    TempDirGuard guard{base};
    std::error_code ec;
    fs::remove_all(base, ec);
    Require(fs::create_directories(root / L"nested", ec), "Create test root & nested dir");

    std::vector<uint8_t> imageData(1234);
    for (size_t i = 0; i < imageData.size(); ++i) {
        imageData[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    }
    std::vector<uint8_t> largeData(8192);
    for (size_t i = 0; i < largeData.size(); ++i) {
        largeData[i] = static_cast<uint8_t>((i * 37 + 19) & 0xFF);
    }
    std::vector<uint8_t> exact1024(1024, 0x77);
    std::vector<uint8_t> secretOutside(32, 0xCC);

    WriteBinaryFile(root / L"Image", imageData);
    WriteBinaryFile(root / L"large.bin", largeData);
    WriteBinaryFile(root / L"empty.bin", {});
    WriteBinaryFile(root / L"exact1024.bin", exact1024);
    WriteBinaryFile(root / L"nested" / L"board.dtb", std::vector<uint8_t>(77, 0x5A));
    WriteBinaryFile(base / L"outside.bin", secretOutside);

    std::vector<TftpDirectoryEntry> entries;
    std::wstring err;
    Require(ListLocalTftpDirectory(root.wstring(), entries, err), "ListLocalTftpDirectory must succeed");
    Require(entries.size() == 4, "Directory listing must exclude nested subdirectories");

    TftpServer server;
    Require(server.SetLocalRoot(root.wstring(), err), "SetLocalRoot must accept valid directory");
    Require(server.Start("127.0.0.1", testPort, err), "tftp32 server must start");

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;

    // 1. Standard 512-byte RRQ
    Require(client.DownloadFile(testPort, "Image", 512, downloaded, false), "Standard 512-byte download must succeed");
    Require(downloaded == imageData, "Downloaded Image must match source bytes");

    // 2. Option negotiation (blksize=1400, tsize)
    uint64_t tsize = 0;
    Require(client.DownloadFile(testPort, "large.bin", 1400, downloaded, true, &tsize),
            "Option-negotiated download must succeed");
    Require(tsize == largeData.size() && downloaded == largeData, "Negotiated tsize and payload must match");

    // 3. 0-byte and exact block multiple
    Require(client.DownloadFile(testPort, "empty.bin", 512, downloaded, false) && downloaded.empty(),
            "0-byte file download must succeed");
    Require(client.DownloadFile(testPort, "exact1024.bin", 512, downloaded, false) && downloaded == exact1024,
            "Exact 1024-byte block-multiple download must succeed");

    // 4. Missing file and path traversal rejection
    client.SendRrq(testPort, "missing.bin");
    sockaddr_in from{};
    std::vector<uint8_t> reply;
    int r = client.RecvPacket(reply, from);
    Require(r >= 4 && ((reply[0] << 8) | reply[1]) == 5, "Missing file must return TFTP ERROR");

    client.SendRrq(testPort, "../outside.bin");
    r = client.RecvPacket(reply, from);
    Require(r >= 4 && ((reply[0] << 8) | reply[1]) == 5, "Path traversal must return TFTP ERROR");

    // 5. Concurrent clients downloading same and different files
    std::vector<uint8_t> c1Data, c2Data;
    bool ok1 = false, ok2 = false;
    std::thread t1([&] {
        TftpClientSimulator c1;
        ok1 = c1.DownloadFile(testPort, "large.bin", 512, c1Data, false);
    });
    std::thread t2([&] {
        TftpClientSimulator c2;
        ok2 = c2.DownloadFile(testPort, "Image", 512, c2Data, false);
    });
    t1.join();
    t2.join();
    Require(ok1 && c1Data == largeData, "Concurrent client 1 download must match");
    Require(ok2 && c2Data == imageData, "Concurrent client 2 download must match");

    server.Stop();
    std::cout << "[PASS] tftp32 directory serving verified." << std::endl;
}

void TestLiveDirectorySwitchAndPublishLocal(uint16_t testPort) {
    std::cout << "[RUN] Testing live directory switch and PublishLocal staging..." << std::endl;
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / (L"serialctl_tftp32_switch_" + std::to_wstring(GetCurrentProcessId()));
    const auto dirA = base / L"dirA";
    const auto dirB = base / L"dirB";
    TempDirGuard guard{base};
    std::error_code ec;
    fs::remove_all(base, ec);
    Require(fs::create_directories(dirA, ec) && fs::create_directories(dirB, ec), "Create switch test dirs");

    const std::vector<uint8_t> alphaData{'A', 'L', 'P', 'H', 'A'};
    const std::vector<uint8_t> betaData{'B', 'E', 'T', 'A', '!'};
    const std::vector<uint8_t> stagedData{'S', 'T', 'A', 'G', 'E', 'D'};
    WriteBinaryFile(dirA / L"alpha.bin", alphaData);
    WriteBinaryFile(dirB / L"beta.bin", betaData);
    WriteBinaryFile(base / L"source.bin", stagedData);

    TftpServer server;
    std::wstring err;
    Require(server.SetLocalRoot(dirA.wstring(), err), "Configure initial root dirA");
    Require(server.Start("127.0.0.1", testPort, err), "Start server on dirA");

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    Require(client.DownloadFile(testPort, "alpha.bin", 512, downloaded, false) && downloaded == alphaData,
            "Download alpha.bin from dirA");

    // Switch directory while server is actively listening
    Require(server.SetLocalRoot(dirB.wstring(), err), "Live switch to dirB must succeed");
    Require(server.IsListening(), "Server must remain listening after live directory switch");
    Require(client.DownloadFile(testPort, "beta.bin", 512, downloaded, false) && downloaded == betaData,
            "Download beta.bin after live switch to dirB");
    Require(!client.DownloadFile(testPort, "alpha.bin", 512, downloaded, false),
            "alpha.bin from old dirA must no longer be served after switch");

    server.Stop();
    server.ClearRoot();

    Require(server.PublishLocal("staged.bin", (base / L"source.bin").wstring(), true, err),
            "PublishLocal single file succeeds");
    Require(server.Start("127.0.0.1", testPort, err), "Start server with PublishLocal fallback root");
    Require(client.DownloadFile(testPort, "staged.bin", 512, downloaded, false) && downloaded == stagedData,
            "Download staged.bin published via PublishLocal");
    server.Stop();
    std::cout << "[PASS] Live directory switch and PublishLocal verified." << std::endl;
}

} // namespace

int main() {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    std::cout << "=== Running tftp32 Integration Tests ===" << std::endl;
    TestBindAddressValidationAndRestart(16981);
    TestLocalDirectoryServing(16982);
    TestLiveDirectorySwitchAndPublishLocal(16983);
    WSACleanup();
    std::cout << "=== tftp32 Integration Tests PASSED ===" << std::endl;
    return 0;
}
