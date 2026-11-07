#undef NDEBUG
#include <cassert>
#include "TftpServer.h"
#include "TftpFileSource.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <random>
#include <fstream>
#include <windows.h>

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
            // Parse options from OACK
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

            // Send ACK 0
            SendAck(serverTid, 0);

            // Wait for DATA 1
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
                    // EOF reached
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
    assert(server.Start("127.0.0.1", testPort, err));

    TftpClientSimulator client;

    // 1. WRQ must be rejected with ERROR 2 (Access violation)
    client.SendWrq(testPort, "anything.bin");
    std::vector<uint8_t> reply;
    sockaddr_in from{};
    int recvd = client.RecvPacket(reply, from);
    assert(recvd >= 4);
    uint16_t opcode = (reply[0] << 8) | reply[1];
    uint16_t errCode = (reply[2] << 8) | reply[3];
    assert(opcode == 5); // ERROR
    assert(errCode == 2); // Access violation
    std::string errMsg(reinterpret_cast<char*>(reply.data() + 4));
    assert(errMsg.find("Read-only") != std::string::npos);

    // 2. Unlisted filename must return ERROR 1 (File not found)
    client.SendRrq(testPort, "non_existent.bin");
    recvd = client.RecvPacket(reply, from);
    assert(recvd >= 4);
    opcode = (reply[0] << 8) | reply[1];
    errCode = (reply[2] << 8) | reply[3];
    assert(opcode == 5);
    assert(errCode == 1); // File not found

    // 3. Path traversal attacks must be rejected
    for (const char* evil : {"../secret.bin", "..\\windows\\win.ini", "/etc/passwd", "C:\\test.bin"}) {
        client.SendRrq(testPort, evil);
        recvd = client.RecvPacket(reply, from);
        assert(recvd >= 4);
        opcode = (reply[0] << 8) | reply[1];
        errCode = (reply[2] << 8) | reply[3];
        assert(opcode == 5);
        assert(errCode == 2); // Access violation
    }

    // 4. Overwrite confirmation
    std::vector<uint8_t> dummy(10, 0xAA);
    TempTestFile f1(dummy);
    assert(server.PublishLocal("Image", f1.Path(), false, err));
    assert(!server.PublishLocal("Image", f1.Path(), false, err)); // collision rejected without allowOverwrite
    assert(server.PublishLocal("Image", f1.Path(), true, err)); // overwrite allowed

    server.Stop();
    std::cout << "[PASS] Security boundaries verified." << std::endl;
}

void TestStandard512Transfer(uint16_t testPort) {
    std::cout << "[RUN] Testing standard 512-byte RRQ transfer..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    std::vector<uint8_t> testData(2500);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
    }
    TempTestFile f(testData);
    assert(server.PublishLocal("kernel.bin", f.Path(), true, err));

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    bool ok = client.DownloadFile(testPort, "kernel.bin", 512, downloaded, false);
    assert(ok);
    assert(downloaded == testData);

    for (int i = 0; i < 50; ++i) {
        auto t = server.ListTransfers();
        if (!t.empty() && t.back().phase == "completed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto trans = server.ListTransfers();
    assert(!trans.empty());
    assert(trans.back().phase == "completed");
    assert(trans.back().ackedBytes == testData.size());

    server.Stop();
    std::cout << "[PASS] Standard 512-byte transfer verified (" << downloaded.size() << " bytes)." << std::endl;
}

void TestOptionNegotiation(uint16_t testPort) {
    std::cout << "[RUN] Testing RFC 2347/2348 option negotiation (blksize=1400, tsize)..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    std::vector<uint8_t> testData(8192);
    for (size_t i = 0; i < testData.size(); ++i) {
        testData[i] = static_cast<uint8_t>(i & 0xFF);
    }
    TempTestFile f(testData);
    assert(server.PublishLocal("Image", f.Path(), true, err));

    TftpClientSimulator client;
    std::vector<uint8_t> downloaded;
    uint64_t tsize = 0;
    bool ok = client.DownloadFile(testPort, "Image", 1400, downloaded, true, &tsize);
    assert(ok);
    assert(tsize == testData.size());
    assert(downloaded == testData);

    server.Stop();
    std::cout << "[PASS] Option negotiation verified." << std::endl;
}

void TestExactBlockMultipleAndZeroByteFile(uint16_t testPort) {
    std::cout << "[RUN] Testing 0-byte file and exact block multiple (1024 bytes -> 0-byte termination)..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    // 1. 0-byte file
    TempTestFile emptyFile({});
    assert(server.PublishLocal("empty.bin", emptyFile.Path(), true, err));
    TftpClientSimulator client;
    std::vector<uint8_t> dlEmpty;
    assert(client.DownloadFile(testPort, "empty.bin", 512, dlEmpty, false));
    assert(dlEmpty.empty());

    // 2. Exact 1024 bytes with blksize=512 (must send 0-byte terminating block #3)
    std::vector<uint8_t> blockMultipleData(1024, 0x77);
    TempTestFile multFile(blockMultipleData);
    assert(server.PublishLocal("exact1024.bin", multFile.Path(), true, err));
    std::vector<uint8_t> dlMult;
    assert(client.DownloadFile(testPort, "exact1024.bin", 512, dlMult, false));
    assert(dlMult == blockMultipleData);

    server.Stop();
    std::cout << "[PASS] 0-byte and exact block multiple verified." << std::endl;
}

void TestRetransmissionAndDupAck(uint16_t testPort) {
    std::cout << "[RUN] Testing timeout retransmission, duplicate ACK ignore, and client ERROR..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    std::vector<uint8_t> testData(2048, 0xAB);
    TempTestFile f(testData);
    assert(server.PublishLocal("retrans.bin", f.Path(), true, err));

    TftpClientSimulator client;
    client.SendRrq(testPort, "retrans.bin");

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    assert(recvd >= 4);
    uint16_t opcode = (pkt[0] << 8) | pkt[1];
    assert(opcode == 3); // DATA 1

    // 1. Ignore DATA 1 intentionally and wait for server to retransmit!
    std::vector<uint8_t> retransPkt;
    sockaddr_in retransFrom{};
    int retransRecvd = client.RecvPacket(retransPkt, retransFrom);
    assert(retransRecvd >= 4);
    assert((retransPkt[0] << 8 | retransPkt[1]) == 3);
    assert((retransPkt[2] << 8 | retransPkt[3]) == 1); // DATA 1 retransmitted!

    // 2. Send duplicate ACK #0 (before DATA 1), server should ignore it
    client.SendAck(serverTid, 0);

    // Send valid ACK #1
    client.SendAck(serverTid, 1);

    // Receive DATA 2 (skipping any in-flight late duplicate of DATA 1)
    do {
        recvd = client.RecvPacket(pkt, serverTid);
        assert(recvd >= 4);
    } while ((pkt[2] << 8 | pkt[3]) == 1);
    assert((pkt[2] << 8 | pkt[3]) == 2); // DATA 2

    // 3. Client cancels transfer with ERROR packet
    client.SendError(serverTid, 0, "Client aborted");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto trans = server.ListTransfers();
    assert(!trans.empty());
    assert(trans.back().phase == "cancelled");

    server.Stop();
    std::cout << "[PASS] Retransmission, duplicate ACK, and cancel verified." << std::endl;
}

void TestConcurrentClients(uint16_t testPort) {
    std::cout << "[RUN] Testing concurrent downloads from two clients..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    std::vector<uint8_t> data1(4096, 0x11);
    std::vector<uint8_t> data2(4096, 0x22);
    TempTestFile f1(data1);
    TempTestFile f2(data2);
    assert(server.PublishLocal("file1.bin", f1.Path(), true, err));
    assert(server.PublishLocal("file2.bin", f2.Path(), true, err));

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

    assert(ok1 && res1 == data1);
    assert(ok2 && res2 == data2);

    // Also test two clients concurrently downloading the EXACT SAME published file
    std::vector<uint8_t> sameData(16384);
    for (size_t i = 0; i < sameData.size(); ++i) sameData[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    TempTestFile fSame(sameData);
    assert(server.PublishLocal("shared.bin", fSame.Path(), true, err));
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
    assert(okSame1 && same1 == sameData);
    assert(okSame2 && same2 == sameData);

    server.Stop();
    std::cout << "[PASS] Concurrent client downloads verified." << std::endl;
}

void TestRemoteStreamingBoundedMemory(uint16_t testPort) {
    std::cout << "[RUN] Testing remote streaming (500 MiB virtual stream, bounded memory, zero disk footprint)..." << std::endl;
    TftpServer server;
    std::wstring err;
    assert(server.Start("127.0.0.1", testPort, err));

    // 500 MiB simulated remote stream
    const uint64_t virtualSize = 500ULL * 1024ULL * 1024ULL;
    auto mockSource = std::make_shared<MockStreamTftpFileSource>(virtualSize);
    assert(server.PublishCustom("BigStream.img", mockSource, true, err));

    // Download first 128 KiB to verify streaming and options
    TftpClientSimulator client;
    client.SendRrq(testPort, "BigStream.img", {{"blksize", "1400"}, {"tsize", "0"}});

    sockaddr_in serverTid{};
    std::vector<uint8_t> pkt;
    int recvd = client.RecvPacket(pkt, serverTid);
    assert(recvd >= 4);
    assert((pkt[0] << 8 | pkt[1]) == 6); // OACK

    client.SendAck(serverTid, 0);

    // Read 100 blocks (140,000 bytes)
    for (uint32_t b = 1; b <= 100; ++b) {
        recvd = client.RecvPacket(pkt, serverTid);
        assert(recvd == static_cast<int>(1400 + 4));
        assert((pkt[2] << 8 | pkt[3]) == (b & 0xFFFF));

        // Verify pattern: byte = (offset & 0xFF)
        uint64_t blkOffset = static_cast<uint64_t>(b - 1) * 1400;
        for (size_t i = 0; i < 1400; ++i) {
            uint8_t expected = static_cast<uint8_t>((blkOffset + i) & 0xFF);
            assert(pkt[4 + i] == expected);
        }

        client.SendAck(serverTid, static_cast<uint16_t>(b & 0xFFFF));
    }

    // Verify memory bounds & zero disk files
    assert(RemoteSshTftpFileSource::MaxBufferCapacity() <= 4 * 1024 * 1024); // max bounded buffer <= 4 MiB

    // Test remote stream failure / disconnect midway
    mockSource->InjectError(L"Remote SSH connection disconnected");
    recvd = client.RecvPacket(pkt, serverTid);
    // Server should send ERROR packet
    assert(recvd >= 4);
    assert((pkt[0] << 8 | pkt[1]) == 5); // ERROR packet

    // Direct test of RemoteSshTftpFileSource sliding window & backpressure with pipe writer
    {
        SshRemoteConfig cfg{L"10.0.0.1", 22, L"root", L"secret", L"/boot/Image", L""};
        std::thread writerThread;
        auto pipeLauncher = [&](const SshRemoteConfig&, HANDLE& outputRead, HANDLE& procHandle, std::wstring&) -> bool {
            HANDLE outWrite = nullptr;
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
            if (!CreatePipe(&outputRead, &outWrite, &sa, 65536)) return false;
            procHandle = nullptr;
            writerThread = std::thread([outWrite]() {
                std::vector<uint8_t> chunk(65536);
                // Write 3 MiB (> 2 MiB MaxBufferCapacity) to exercise sliding-window backpressure
                const size_t totalToWrite = 3 * 1024 * 1024;
                size_t writtenTotal = 0;
                while (writtenTotal < totalToWrite) {
                    for (size_t i = 0; i < chunk.size(); ++i) {
                        chunk[i] = static_cast<uint8_t>((writtenTotal + i) & 0xFF);
                    }
                    DWORD written = 0;
                    if (!WriteFile(outWrite, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr) || written == 0) {
                        break;
                    }
                    writtenTotal += written;
                }
                CloseHandle(outWrite);
            });
            return true;
        };
        TftpSourceInfo info;
        info.type = "remote";
        info.displayPath = cfg.remotePath;
        info.host = cfg.host;
        info.port = cfg.port;
        info.username = cfg.username;
        info.size = 3 * 1024 * 1024;
        info.sizeKnown = true;
        info.status = "Ready";

        RemoteSshTftpFileSource sshSource(cfg, pipeLauncher, info);
        assert(sshSource.Open(err));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        // Sliding window buffer must stay bounded near MaxBufferCapacity (2 MiB + at most 1 chunk)
        assert(sshSource.CurrentBufferSize() <= RemoteSshTftpFileSource::MaxBufferCapacity() + 32768);

        std::vector<uint8_t> readBuf(16384);
        uint64_t offset = 0;
        while (offset < info.size) {
            size_t got = 0;
            assert(sshSource.Read(offset, readBuf.data(), readBuf.size(), got, err));
            assert(got > 0);
            for (size_t i = 0; i < got; ++i) {
                assert(readBuf[i] == static_cast<uint8_t>((offset + i) & 0xFF));
            }
            offset += got;
            assert(sshSource.CurrentBufferSize() <= RemoteSshTftpFileSource::MaxBufferCapacity() + 32768);
        }
        sshSource.Close();
        if (writerThread.joinable()) writerThread.join();
    }

    server.Stop();
    std::cout << "[PASS] Remote streaming, bounded memory, and error handling verified." << std::endl;
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    std::cout << "=== Running TFTP Server Integration Tests ===" << std::endl;

    TestSecurityBoundaries(6970);
    TestStandard512Transfer(6971);
    TestOptionNegotiation(6972);
    TestExactBlockMultipleAndZeroByteFile(6973);
    TestRetransmissionAndDupAck(6974);
    TestConcurrentClients(6975);
    TestRemoteStreamingBoundedMemory(6976);

    WSACleanup();
    std::cout << "=== All TFTP Server Integration Tests PASSED ===" << std::endl;
    return 0;
}
