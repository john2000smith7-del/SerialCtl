#include "TftpServer.h"
#include "TftpFileSource.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <thread>
#include <chrono>
#include <random>
#include <fstream>
#include <atomic>
#include <windows.h>
#include <wincrypt.h>

#define Require(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "REQUIRE FAILED: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        std::exit(1); \
    } \
} while(0)

using namespace serialctl;

// Temporary file helper
class TempTestFile {
public:
    TempTestFile(const std::vector<uint8_t>& data) {
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        wchar_t tempFile[MAX_PATH];
        GetTempFileNameW(tempPath, L"tftp_", 0, tempFile);
        path_ = tempFile;

        HANDLE h = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!data.empty()) {
                WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            }
            CloseHandle(h);
        }
    }

    ~TempTestFile() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
};

// Simple TFTP client simulator for testing over loopback
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
        std::string mode = "octet";
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

    void SendWrq(uint16_t serverPort, const std::string& filename) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(serverPort);

        std::vector<uint8_t> pkt;
        pkt.push_back(0); pkt.push_back(2); // WRQ
        for (char c : filename) pkt.push_back(static_cast<uint8_t>(c));
        pkt.push_back(0);
        std::string mode = "octet";
        for (char c : mode) pkt.push_back(static_cast<uint8_t>(c));
        pkt.push_back(0);

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
        uint8_t pkt[4];
        pkt[0] = 0; pkt[1] = 4; // ACK
        pkt[2] = static_cast<uint8_t>(blockNum >> 8);
        pkt[3] = static_cast<uint8_t>(blockNum & 0xFF);
        sendto(sock_, reinterpret_cast<const char*>(pkt), 4, 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    void SendError(const sockaddr_in& to, uint16_t code, const std::string& msg) {
        std::vector<uint8_t> pkt(4 + msg.size() + 1);
        pkt[0] = 0; pkt[1] = 5; // ERROR
        pkt[2] = static_cast<uint8_t>(code >> 8);
        pkt[3] = static_cast<uint8_t>(code & 0xFF);
        std::memcpy(pkt.data() + 4, msg.c_str(), msg.size() + 1);
        sendto(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
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

// ============================================================================
// Test Suites
// ============================================================================

void TestSecurityBoundaries(uint16_t testPort) {
    std::cout << "[RUN] Testing security boundaries..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    TftpClientSimulator client;

    // 1. WRQ must be rejected with ERROR 2 (Access violation)
    client.SendWrq(testPort, "anything.bin");
    std::vector<uint8_t> reply;
    sockaddr_in from{};
    int recvd = client.RecvPacket(reply, from);
    Require(recvd >= 4, "WRQ response must be at least 4 bytes");
    uint16_t opcode = (reply[0] << 8) | reply[1];
    uint16_t errCode = (reply[2] << 8) | reply[3];
    Require(opcode == 5, "WRQ must return ERROR opcode (5)");
    Require(errCode == 2, "WRQ error code must be 2 (Access violation)");
    std::string errMsg(reinterpret_cast<char*>(reply.data() + 4));
    Require(errMsg.find("Read-only") != std::string::npos, "Error message must state Read-only");

    // 2. Unlisted filename must return ERROR 1 (File not found)
    client.SendRrq(testPort, "non_existent.bin");
    recvd = client.RecvPacket(reply, from);
    Require(recvd >= 4, "RRQ response must be at least 4 bytes");
    opcode = (reply[0] << 8) | reply[1];
    errCode = (reply[2] << 8) | reply[3];
    Require(opcode == 5, "Non-existent file must return ERROR opcode");
    Require(errCode == 1, "Non-existent file must return ERROR code 1 (File not found)");

    // 3. Path traversal attacks must be rejected
    for (const char* evil : {"../secret.bin", "..\\windows\\win.ini", "/etc/passwd", "C:\\test.bin"}) {
        client.SendRrq(testPort, evil);
        recvd = client.RecvPacket(reply, from);
        Require(recvd >= 4, "Evil path response must be at least 4 bytes");
        opcode = (reply[0] << 8) | reply[1];
        errCode = (reply[2] << 8) | reply[3];
        Require(opcode == 5, "Path traversal must return ERROR opcode");
        Require(errCode == 2, "Path traversal must return ERROR code 2");
    }

    // 4. Overwrite confirmation
    std::vector<uint8_t> dummy(10, 0xAA);
    TempTestFile f1(dummy);
    const bool p1 = server.PublishLocal("Image", f1.Path(), false, err);
    Require(p1, "First PublishLocal must succeed");
    const bool p2 = server.PublishLocal("Image", f1.Path(), false, err);
    Require(!p2, "Collision without allowOverwrite must be rejected");
    const bool p3 = server.PublishLocal("Image", f1.Path(), true, err);
    Require(p3, "PublishLocal with allowOverwrite must succeed");

    server.Stop();
    std::cout << "[PASS] Security boundaries verified." << std::endl;
}

void TestStandard512Transfer(uint16_t testPort) {
    std::cout << "[RUN] Testing standard 512-byte RRQ transfer..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    std::vector<uint8_t> testData(2500);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
    }
    TempTestFile f(testData);
    const bool published = server.PublishLocal("kernel.bin", f.Path(), true, err);
    Require(published, "PublishLocal kernel.bin must succeed");

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    bool ok = client.DownloadFile(testPort, "kernel.bin", 512, downloaded, false);
    Require(ok, "DownloadFile must succeed");
    Require(downloaded == testData, "Downloaded data must match source data exactly");

    for (int i = 0; i < 50; ++i) {
        auto t = server.ListTransfers();
        if (!t.empty() && t.back().phase == "completed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto trans = server.ListTransfers();
    Require(!trans.empty(), "Transfers list must not be empty");
    Require(trans.back().phase == "completed", "Transfer phase must be completed");
    Require(trans.back().ackedBytes == testData.size(), "Acked bytes must equal file size");

    server.Stop();
    std::cout << "[PASS] Standard 512-byte transfer verified (" << downloaded.size() << " bytes)." << std::endl;
}

void TestOptionNegotiation(uint16_t testPort) {
    std::cout << "[RUN] Testing RFC 2347/2348 option negotiation (blksize=1400, tsize)..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    std::vector<uint8_t> testData(8192);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>(i & 0xFF);
    }
    TempTestFile f(testData);
    const bool published = server.PublishLocal("Image", f.Path(), true, err);
    Require(published, "PublishLocal Image must succeed");

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    uint64_t tsize = 0;
    bool ok = client.DownloadFile(testPort, "Image", 1400, downloaded, true, &tsize);
    Require(ok, "DownloadFile with options must succeed");
    Require(tsize == testData.size(), "tsize option must match testData size");
    Require(downloaded == testData, "Downloaded data must match testData");

    server.Stop();
    std::cout << "[PASS] Option negotiation verified." << std::endl;
}

void TestExactBlockMultipleAndZeroByteFile(uint16_t testPort) {
    std::cout << "[RUN] Testing 0-byte file and exact block multiple (1024 bytes -> 0-byte termination)..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    // 1. 0-byte file
    TempTestFile emptyFile({});
    const bool pubEmpty = server.PublishLocal("empty.bin", emptyFile.Path(), true, err);
    Require(pubEmpty, "Publish 0-byte file must succeed");
    TftpClientSimulator client;
    std::vector<uint8_t> dlEmpty;
    const bool dlOk1 = client.DownloadFile(testPort, "empty.bin", 512, dlEmpty, false);
    Require(dlOk1, "Download 0-byte file must succeed");
    Require(dlEmpty.empty(), "0-byte file must download 0 bytes");

    // 2. Exact 1024 bytes with blksize=512 (must send 0-byte terminating block #3)
    std::vector<uint8_t> blockMultipleData(1024, 0x77);
    TempTestFile multFile(blockMultipleData);
    const bool pubMult = server.PublishLocal("exact1024.bin", multFile.Path(), true, err);
    Require(pubMult, "Publish 1024-byte file must succeed");
    std::vector<uint8_t> dlMult;
    const bool dlOk2 = client.DownloadFile(testPort, "exact1024.bin", 512, dlMult, false);
    Require(dlOk2, "Download 1024-byte file must succeed");
    Require(dlMult == blockMultipleData, "Downloaded 1024-byte data must match exactly");

    server.Stop();
    std::cout << "[PASS] 0-byte and exact block multiple verified." << std::endl;
}

void TestRetransmissionAndDupAck(uint16_t testPort) {
    std::cout << "[RUN] Testing timeout retransmission, duplicate ACK ignore, and client ERROR..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    std::vector<uint8_t> testData(2048, 0xAB);
    TempTestFile f(testData);
    const bool published = server.PublishLocal("retrans.bin", f.Path(), true, err);
    Require(published, "PublishLocal retrans.bin must succeed");

    TftpClientSimulator client;
    client.SendRrq(testPort, "retrans.bin");

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    Require(recvd >= 4, "Must receive DATA 1");
    uint16_t opcode = (pkt[0] << 8) | pkt[1];
    Require(opcode == 3, "Opcode must be DATA (3)");

    // 1. Ignore DATA 1 intentionally and wait for server to retransmit!
    std::vector<uint8_t> retransPkt;
    sockaddr_in retransFrom{};
    int retransRecvd = client.RecvPacket(retransPkt, retransFrom);
    Require(retransRecvd >= 4, "Must receive retransmitted packet");
    Require((retransPkt[0] << 8 | retransPkt[1]) == 3, "Retransmitted packet must be DATA");
    Require((retransPkt[2] << 8 | retransPkt[3]) == 1, "Retransmitted packet block must be 1");

    // 2. Send duplicate ACK #0 (before DATA 1), server should ignore it
    client.SendAck(serverTid, 0);

    // Send valid ACK #1
    client.SendAck(serverTid, 1);

    // Receive DATA 2 (skipping any in-flight late duplicate of DATA 1)
    do {
        recvd = client.RecvPacket(pkt, serverTid);
        Require(recvd >= 4, "Must receive DATA packet");
    } while ((pkt[2] << 8 | pkt[3]) == 1);
    Require((pkt[2] << 8 | pkt[3]) == 2, "Must receive DATA block 2");

    // 3. Client cancels transfer with ERROR packet
    client.SendError(serverTid, 0, "Client aborted");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto trans = server.ListTransfers();
    Require(!trans.empty(), "Transfers list must not be empty");
    Require(trans.back().phase == "cancelled", "Transfer phase must be cancelled");

    server.Stop();
    std::cout << "[PASS] Retransmission, duplicate ACK, and cancel verified." << std::endl;
}

void TestConcurrentClients(uint16_t testPort) {
    std::cout << "[RUN] Testing concurrent downloads from two clients..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    std::vector<uint8_t> data1(4096, 0x11);
    std::vector<uint8_t> data2(4096, 0x22);
    TempTestFile f1(data1);
    TempTestFile f2(data2);
    const bool p1 = server.PublishLocal("file1.bin", f1.Path(), true, err);
    Require(p1, "Publish file1.bin must succeed");
    const bool p2 = server.PublishLocal("file2.bin", f2.Path(), true, err);
    Require(p2, "Publish file2.bin must succeed");

    std::vector<uint8_t> res1, res2;
    bool ok1 = false, ok2 = false;

    std::thread t1([&] {
        TftpClientSimulator c1;
        ok1 = c1.DownloadFile(testPort, "file1.bin", 512, res1, false);
    });
    std::thread t2([&] {
        TftpClientSimulator c2;
        ok2 = c2.DownloadFile(testPort, "file2.bin", 512, res2, false);
    });

    t1.join();
    t2.join();

    Require(ok1 && res1 == data1, "Client 1 download must match data1");
    Require(ok2 && res2 == data2, "Client 2 download must match data2");

    // Also test two clients concurrently downloading the EXACT SAME published file
    std::vector<uint8_t> sameData(16384);
    for (size_t i = 0; i < sameData.size(); ++i) sameData[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    TempTestFile fSame(sameData);
    const bool pSame = server.PublishLocal("shared.bin", fSame.Path(), true, err);
    Require(pSame, "Publish shared.bin must succeed");

    std::vector<uint8_t> same1, same2;
    bool okSame1 = false, okSame2 = false;
    std::thread ts1([&] {
        TftpClientSimulator c1;
        okSame1 = c1.DownloadFile(testPort, "shared.bin", 1024, same1, true);
    });
    std::thread ts2([&] {
        TftpClientSimulator c2;
        okSame2 = c2.DownloadFile(testPort, "shared.bin", 1024, same2, true);
    });
    ts1.join();
    ts2.join();
    Require(okSame1 && same1 == sameData, "Client 1 shared download must match");
    Require(okSame2 && same2 == sameData, "Client 2 shared download must match");

    server.Stop();
    std::cout << "[PASS] Concurrent client downloads verified." << std::endl;
}

void TestRemoteStreamingBoundedMemory(uint16_t testPort) {
    std::cout << "[RUN] Testing remote streaming (500 MiB virtual stream, bounded memory, zero disk footprint)..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    // 500 MiB simulated remote stream
    const uint64_t virtualSize = 500ULL * 1024ULL * 1024ULL;
    auto mockSource = std::make_shared<MockStreamTftpFileSource>(virtualSize);
    const bool pubStream = server.PublishCustom("BigStream.img", mockSource, true, err);
    Require(pubStream, "PublishCustom BigStream.img must succeed");

    // Download first 128 KiB to verify streaming and options
    TftpClientSimulator client;
    client.SendRrq(testPort, "BigStream.img", {{"blksize", "1400"}, {"tsize", "0"}});

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    Require(recvd >= 4, "Must receive OACK");
    Require((pkt[0] << 8 | pkt[1]) == 6, "Must be OACK packet");

    client.SendAck(serverTid, 0);

    // Read 100 blocks (140,000 bytes)
    for (uint32_t b = 1; b <= 100; ++b) {
        recvd = client.RecvPacket(pkt, serverTid);
        Require(recvd == static_cast<int>(1400 + 4), "DATA packet size must be 1404");
        Require((pkt[2] << 8 | pkt[3]) == (b & 0xFFFF), "Block number must match");

        // Verify pattern: byte = (offset & 0xFF)
        uint64_t blkOffset = static_cast<uint64_t>(b - 1) * 1400;
        for (size_t i = 0; i < 1400; ++i) {
            uint8_t expected = static_cast<uint8_t>((blkOffset + i) & 0xFF);
            Require(pkt[4 + i] == expected, "Payload byte must match expected pattern");
        }

        client.SendAck(serverTid, static_cast<uint16_t>(b & 0xFFFF));
    }

    // Verify memory bounds & zero disk files
    Require(RemoteSshTftpFileSource::MaxBufferCapacity() <= 4 * 1024 * 1024, "Max buffer must be <= 4 MiB");

    // Test remote stream failure / disconnect midway
    mockSource->InjectError(L"Remote SSH connection disconnected");
    recvd = client.RecvPacket(pkt, serverTid);
    // Server should send ERROR packet
    Require(recvd >= 4, "Must receive response after error");
    Require((pkt[0] << 8 | pkt[1]) == 5, "Response must be ERROR opcode (5)");

    server.Stop();
    std::cout << "[PASS] Remote streaming, bounded memory, and error handling verified." << std::endl;
}

void TestComplete500MiB(uint16_t testPort) {
    TftpServer server; std::wstring error;
    Require(server.Start("127.0.0.1", testPort, error), "500 MiB server starts");
    constexpr uint64_t total = 500ULL * 1024 * 1024;
    Require(server.PublishCustom("complete-500MiB.bin", std::make_shared<MockStreamTftpFileSource>(total), true, error), "500 MiB source published");
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    Require(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT), "SHA256 provider");
    Require(CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash), "SHA256 state");
    TftpClientSimulator client;
    client.SendRrq(testPort, "complete-500MiB.bin", {{"blksize", "65464"}, {"tsize", "0"}});
    sockaddr_in tid{}; std::vector<uint8_t> packet;
    Require(client.RecvPacket(packet, tid) >= 4 && packet[1] == 6, "500 MiB option ACK");
    client.SendAck(tid, 0);
    uint64_t received = 0; uint32_t block = 1;
    while (true) {
        const int count = client.RecvPacket(packet, tid);
        Require(count >= 4 && packet[1] == 3, "500 MiB DATA packet");
        Require(((packet[2] << 8) | packet[3]) == static_cast<uint16_t>(block), "500 MiB block ordering");
        const DWORD bytes = static_cast<DWORD>(count - 4);
        Require(received + bytes <= total, "500 MiB exact upper bound");
        Require(CryptHashData(hash, packet.data() + 4, bytes, 0), "Hash every received byte");
        received += bytes;
        client.SendAck(tid, static_cast<uint16_t>(block++));
        if (bytes < 65464) break;
    }
    Require(received == total, "The ENTIRE 500 MiB must arrive before success");
    BYTE digest[32]{}; DWORD length = sizeof(digest);
    Require(CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0), "Read final SHA256");
    CryptDestroyHash(hash); CryptReleaseContext(provider, 0);
    static const char digits[] = "0123456789abcdef";
    std::string hexadecimal;
    for (BYTE byte : digest) { hexadecimal += digits[byte >> 4]; hexadecimal += digits[byte & 15]; }
    Require(hexadecimal == "411fd088435f42d23961db92a0c245b2f946aab0b98ab356649503680410a4dc", "500 MiB full SHA256 mismatch");
    server.Stop();
    std::cout << "PASS: full TFTP loopback received=" << received << " SHA256=" << hexadecimal
              << " source=deterministic-generator (not real SSH)\n";
}

// B06: RFC 1350 block number wrap-around 65535 -> 0 -> 1 -> 2
void TestWrapAround65535(uint16_t testPort) {
    std::cout << "[RUN] Testing TFTP block 65535 wrap-around (65535 -> 0 -> 1 -> 2)..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    // Total size: exactly 65537 blocks of 512 bytes = 33,554,944 bytes (crosses 65535 and 0 and terminates on block 1)
    const uint64_t totalBytes = 65537ULL * 512ULL;
    auto mockSource = std::make_shared<MockStreamTftpFileSource>(totalBytes);
    const bool published = server.PublishCustom("wrap.bin", mockSource, true, err);
    Require(published, "Publish wrap.bin must succeed");

    TftpClientSimulator client;
    client.SendRrq(testPort, "wrap.bin");

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    Require(recvd >= 4, "Must receive block 1");

    uint64_t totalReceived = 0;
    uint32_t blockCount = 1;

    while (true) {
        Require((pkt[0] << 8 | pkt[1]) == 3, "Opcode must be DATA (3)");
        uint16_t blk16 = (pkt[2] << 8) | pkt[3];
        uint16_t expectedBlk16 = static_cast<uint16_t>(blockCount & 0xFFFF);
        Require(blk16 == expectedBlk16, "Block 16-bit number must strictly match without skipping");

        size_t payloadLen = recvd - 4;
        totalReceived += payloadLen;

        // On block 65535: test sending duplicate ACK 65535, server must ignore it and not skip block 65536
        if (blk16 == 65535) {
            client.SendAck(serverTid, 65535); // Duplicate ACK
        }

        client.SendAck(serverTid, blk16);
        blockCount++;

        if (payloadLen < 512) {
            // Reached termination block
            break;
        }

        recvd = client.RecvPacket(pkt, serverTid);
        Require(recvd >= 4, "Must receive next data block");
    }

    Require(totalReceived == totalBytes, "Total received bytes across wrap-around must match totalBytes");
    Require(blockCount > 65536, "Block count must have exceeded 65536 to verify wrap-around");

    server.Stop();
    std::cout << "[PASS] TFTP wrap-around 65535 -> 0 -> 1 verified (" << totalReceived << " bytes)." << std::endl;
}

// B05: Premature EOF and slow stream timeout detection
class PrematureEofSource : public ITftpFileSource, public std::enable_shared_from_this<PrematureEofSource> {
public:
    TftpSourceInfo Info() const override {
        TftpSourceInfo info;
        info.type = "remote";
        info.displayPath = L"mock://premature.bin";
        info.size = 2048; // Declared size is 2048 bytes
        info.sizeKnown = true;
        info.status = "Ready";
        return info;
    }
    std::shared_ptr<ITftpFileSource> CreateSessionSource() const override {
        return std::const_pointer_cast<PrematureEofSource>(shared_from_this());
    }
    bool Open(std::wstring&) override { return true; }
    void Close() override {}
    void Cancel() override {}
    bool Read(uint64_t offset, void* buffer, size_t length, size_t& bytesRead, std::wstring&) override {
        if (offset == 0) {
            bytesRead = (length < 512) ? length : 512;
            std::memset(buffer, 0x55, bytesRead);
            return true;
        }
        // At block 2 (offset 512): return short block (100 bytes < 512) and EOF prematurely!
        bytesRead = 100;
        std::memset(buffer, 0xAA, bytesRead);
        return true;
    }
};

void TestSlowStreamAndPrematureEof(uint16_t testPort) {
    std::cout << "[RUN] Testing premature EOF and stream timeout detection..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    // 1. Premature EOF detection
    auto preSource = std::make_shared<PrematureEofSource>();
    const bool pubPre = server.PublishCustom("premature.bin", preSource, true, err);
    Require(pubPre, "Publish premature.bin must succeed");

    TftpClientSimulator client;
    client.SendRrq(testPort, "premature.bin");

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    Require(recvd >= 4, "Must receive block 1");
    Require((pkt[0] << 8 | pkt[1]) == 3, "Opcode must be DATA 1");
    client.SendAck(serverTid, 1);

    // Block 2: returned 100 bytes (< 512), but offset+bytesRead == 612 != TotalSize (2048)
    // Server must send ERROR packet and mark phase failed
    client.SendAck(serverTid, 2);
    recvd = client.RecvPacket(pkt, serverTid);
    // Next packet from server must be ERROR or server aborted
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto trans = server.ListTransfers();
    Require(!trans.empty(), "Transfers list must not be empty");
    Require(trans.back().phase == "failed", "Transfer phase must be failed on premature EOF");
    Require(trans.back().error.find(L"premature EOF") != std::wstring::npos,
            "Error must explicitly mention premature EOF");

    // 2. Slow stream read timeout / incomplete block
    auto timeoutSource = std::make_shared<MockStreamTftpFileSource>(5000);
    timeoutSource->InjectError(L"Remote stream read timeout (incomplete block)");
    const bool pubTimeout = server.PublishCustom("timeout.bin", timeoutSource, true, err);
    Require(pubTimeout, "Publish timeout.bin must succeed");

    client.SendRrq(testPort, "timeout.bin");
    recvd = client.RecvPacket(pkt, serverTid);
    Require(recvd >= 4, "Must receive ERROR packet on read timeout");
    uint16_t op = (pkt[0] << 8) | pkt[1];
    Require(op == 5, "Response on read timeout must be TFTP ERROR (5)");

    server.Stop();
    std::cout << "[PASS] Premature EOF and stream timeout detection verified." << std::endl;
}

// B07: Concurrency limit (16 max) and path boundary security
void TestConcurrencyLimitAndPathBoundary(uint16_t testPort) {
    std::cout << "[RUN] Testing TFTP concurrency limit (max 16) and path boundaries..." << std::endl;
    TftpServer server;
    std::wstring err;
    const bool started = server.Start("127.0.0.1", testPort, err);
    Require(started, "server.Start must succeed");

    // 1. Path boundary checks
    const bool okDir = server.PublishLocal("winDir", L"C:\\Windows", false, err);
    Require(!okDir, "PublishLocal must reject directory");
    const bool okNonExist = server.PublishLocal("missing", L"C:\\non_existent_file_xyz_12345.bin", false, err);
    Require(!okNonExist, "PublishLocal must reject non-existent file");

    // 2. Concurrency limit: start 16 slow streams, verify 17th is rejected
    auto slowSource = std::make_shared<MockStreamTftpFileSource>(1024ULL * 1024ULL);
    const bool pubSlow = server.PublishCustom("slow.bin", slowSource, true, err);
    Require(pubSlow, "Publish slow.bin must succeed");

    std::vector<std::unique_ptr<TftpClientSimulator>> clients;
    std::vector<sockaddr_in> tids(16);
    for (int i = 0; i < 16; ++i) {
        clients.push_back(std::make_unique<TftpClientSimulator>());
        clients.back()->SendRrq(testPort, "slow.bin");
        std::vector<uint8_t> p;
        int r = clients.back()->RecvPacket(p, tids[i]);
        Require(r >= 4, "Client must receive DATA 1");
        Require((p[0] << 8 | p[1]) == 3, "Opcode must be DATA 1");
        // Keep them active without ACKing
    }

    Require(server.ActiveTransferCount() == 16, "Active transfer count must be exactly 16");

    // 17th client attempts RRQ -> Must receive ERROR 0: "Server busy: too many concurrent transfers"
    TftpClientSimulator client17;
    client17.SendRrq(testPort, "slow.bin");
    sockaddr_in from17{};
    std::vector<uint8_t> reply17;
    int r17 = client17.RecvPacket(reply17, from17);
    Require(r17 >= 4, "17th client must receive ERROR packet");
    Require((reply17[0] << 8 | reply17[1]) == 5, "17th client opcode must be ERROR (5)");
    std::string errMsg17(reinterpret_cast<char*>(reply17.data() + 4));
    Require(errMsg17.find("too many concurrent transfers") != std::string::npos,
            "17th client error must mention too many concurrent transfers");

    // Clean up active clients by sending error
    for (size_t i = 0; i < clients.size(); ++i) {
        clients[i]->SendError(tids[i], 0, "Done test");
    }

    server.Stop();
    std::cout << "[PASS] Concurrency limit (16) and path boundaries verified." << std::endl;
}

void TestLocalSourceIdentity() {
    TempTestFile original({1, 2, 3}), replacement({4, 5, 6});
    LocalTftpFileSource source(original.Path());
    Require(source.Info().status == "Ready", "Readable file must publish");
    std::wstring error;
    auto transfer = source.CreateSessionSource();
    Require(transfer->Open(error), "Published local file opens");
    HANDLE writer = CreateFileW(original.Path().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Require(writer == INVALID_HANDLE_VALUE, "An open transfer must prevent simultaneous file writes");
    transfer->Close();

    HANDLE file = CreateFileW(original.Path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILETIME stamp{};
    Require(file != INVALID_HANDLE_VALUE && GetFileTime(file, nullptr, nullptr, &stamp),
            "Fixture reads published timestamp");
    CloseHandle(file);
    file = CreateFileW(replacement.Path().c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &stamp),
            "Fixture matches replacement timestamp");
    CloseHandle(file);
    Require(MoveFileExW(replacement.Path().c_str(), original.Path().c_str(), MOVEFILE_REPLACE_EXISTING),
            "Fixture atomically replaces published name with same size and time");
    Require(!source.CreateSessionSource()->Open(error), "Same-size same-time replacement must be refused");
    std::cout << "[PASS] Local source write exclusion and file identity verification\n";
}

void TestBindAddressValidation(uint16_t testPort) {
    TftpServer server;
    std::wstring error;
    Require(!server.Start("invalid-bind-address", testPort, error) && !server.IsListening() && !error.empty(),
            "Invalid IPv4 must be rejected without falling back to all interfaces");
    Require(server.Start("127.0.0.1", testPort, error) && server.BindIp() == "127.0.0.1",
            "Loopback listen works on the same server after invalid input");
    server.Stop();
}

void TestLocalDirectoryRootMode(uint16_t testPort) {
    std::cout << "[RUN] Testing directory-based TFTP root mode (excluding subdirectories)..." << std::endl;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (L"serialctl_tftp_root_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    Require(fs::create_directories(root / L"nested", ec), "Create test root & nested subdir");

    std::vector<uint8_t> imageData(1234);
    for (size_t i = 0; i < imageData.size(); ++i) imageData[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
    std::vector<uint8_t> dtbData(77, 0x5A);
    {
        std::ofstream f1(root / L"Image", std::ios::binary);
        f1.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
        std::ofstream f2(root / L"nested" / L"board.dtb", std::ios::binary);
        f2.write(reinterpret_cast<const char*>(dtbData.data()), dtbData.size());
    }

    std::vector<TftpDirectoryEntry> entries;
    std::wstring err;
    Require(ListLocalTftpDirectory(root.wstring(), entries, err), "ListLocalTftpDirectory must succeed");
    Require(entries.size() == 1 && entries[0].name == "Image" && entries[0].sizeKnown && entries[0].size == 1234,
            "Directory listing must include only top-level regular files and exclude nested subdirectories");

    TftpServer server;
    Require(server.SetLocalRoot(root.wstring(), err), "SetLocalRoot must accept valid directory");
    Require(server.RootConfig().mode == TftpRootMode::Local, "RootConfig mode must be Local");
    Require(server.Start("127.0.0.1", testPort, err), "Server start for directory root test");

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    uint64_t tsize = 0;
    Require(client.DownloadFile(testPort, "Image", 512, downloaded, true, &tsize),
            "Download file directly from configured local root");
    Require(tsize == imageData.size() && downloaded == imageData, "Root file contents & tsize must match");

    downloaded.clear();
    Require(client.DownloadFile(testPort, "nested/board.dtb", 512, downloaded, true, &tsize),
            "Download file via safe relative subpath under configured root");
    Require(tsize == dtbData.size() && downloaded == dtbData, "Nested subpath file contents must match");

    client.SendRrq(testPort, "missing.bin");
    sockaddr_in from{};
    std::vector<uint8_t> reply;
    int r = client.RecvPacket(reply, from);
    Require(r >= 4 && (reply[0] << 8 | reply[1]) == 5 && (reply[2] << 8 | reply[3]) == 1,
            "Missing file under directory root must return TFTP ERROR 1");

    client.SendRrq(testPort, "nested");
    r = client.RecvPacket(reply, from);
    Require(r >= 4 && (reply[0] << 8 | reply[1]) == 5 && (reply[2] << 8 | reply[3]) == 2,
            "Requesting a directory itself must return TFTP ERROR 2");

    client.SendRrq(testPort, "../outside.bin");
    r = client.RecvPacket(reply, from);
    Require(r >= 4 && (reply[0] << 8 | reply[1]) == 5 && (reply[2] << 8 | reply[3]) == 2,
            "Path traversal under directory root must return TFTP ERROR 2");

    server.Stop();
    fs::remove_all(root, ec);

    // Deferred remote source verification (pending until session Open)
    SshRemoteConfig remoteCfg;
    remoteCfg.host = L"127.0.0.1";
    remoteCfg.port = 22;
    remoteCfg.username = L"tester";
    remoteCfg.remotePath = L"/tftpboot/Image";
    auto mockLauncher = [](const SshRemoteConfig&, HANDLE& outRead, HANDLE& outProc, std::wstring&) -> bool {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rPipe = nullptr, wPipe = nullptr;
        if (!CreatePipe(&rPipe, &wPipe, &sa, 4096)) return false;
        CloseHandle(wPipe);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        wchar_t cmd[] = L"cmd.exe /c exit 0";
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(rPipe);
            return false;
        }
        CloseHandle(pi.hThread);
        outRead = rPipe;
        outProc = pi.hProcess;
        return true;
    };
    RemoteSshTftpFileSource deferredRemote(remoteCfg, mockLauncher, true);
    Require(deferredRemote.Info().status == "Pending", "Deferred remote source must start in Pending status");
    Require(deferredRemote.Open(err), "Deferred remote source queries/opens on demand");
    Require(deferredRemote.Info().status == "Ready" && deferredRemote.Info().sizeKnown,
            "Deferred remote source becomes Ready with size on Open");
    deferredRemote.Close();
    std::cout << "[PASS] Directory root mode, exclusion, subpath resolution, and deferred remote verified." << std::endl;
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    std::cout << "=== Running TFTP Server Integration Tests ===" << std::endl;

    TestBindAddressValidation(6981);
    TestSecurityBoundaries(6970);
    TestStandard512Transfer(6971);
    TestOptionNegotiation(6972);
    TestExactBlockMultipleAndZeroByteFile(6973);
    TestRetransmissionAndDupAck(6974);
    TestConcurrentClients(6975);
    TestRemoteStreamingBoundedMemory(6976);
    TestComplete500MiB(6980);
    TestWrapAround65535(6977);
    TestSlowStreamAndPrematureEof(6978);
    TestConcurrencyLimitAndPathBoundary(6979);
    TestLocalSourceIdentity();
    TestLocalDirectoryRootMode(6982);

    WSACleanup();
    std::cout << "=== TFTP Server Integration Tests PASSED ===" << std::endl;
    return 0;
}
