#include "TftpFileSource.h"
#include "TftpServer.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

using namespace serialctl;

namespace {

int g_failures = 0;

void Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAILED: " << msg << '\n';
        ++g_failures;
    }
}

std::wstring CreateTempDir() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        std::cerr << "GetTempPathW failed\n";
        std::exit(1);
    }
    const std::wstring dir = std::wstring(tmp, n) + L"serialctl-tftp-unit-" +
                             std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

int main() {
    const std::wstring tempDir = CreateTempDir();
    const std::wstring sampleFile = tempDir + L"\\firmware.bin";
    const std::wstring subDir = tempDir + L"\\nested";
    std::filesystem::create_directories(subDir);
    {
        std::ofstream out(sampleFile, std::ios::binary);
        out << "TFTP_UNIT_PAYLOAD_1234";
        std::ofstream subOut(subDir + L"\\ignored.bin", std::ios::binary);
        subOut << "nested";
    }

    TftpServer server;
    std::wstring err;

    // 1. Path traversal / illegal filename / invalid source rejection on PublishLocal
    Expect(!server.PublishLocal("", sampleFile, true, err), "empty TFTP name rejected");
    Expect(!server.PublishLocal("../secret.bin", sampleFile, true, err), "parent traversal rejected");
    Expect(!server.PublishLocal("dir/uImage", sampleFile, true, err), "forward slash rejected");
    Expect(!server.PublishLocal("dir\\uImage", sampleFile, true, err), "backslash rejected");
    Expect(!server.PublishLocal("C:uImage", sampleFile, true, err), "drive colon rejected");
    Expect(!server.PublishLocal("dirTarget", subDir, true, err), "directory rejected by PublishLocal");
    Expect(!server.PublishLocal("missing", tempDir + L"\\does-not-exist.bin", true, err),
           "non-existent file rejected by PublishLocal");

    // 2. Valid publish / duplicate rejection / overwrite / unpublish
    Expect(server.PublishLocal("firmware.bin", sampleFile, false, err), "PublishLocal valid file succeeds");
    Expect(!server.PublishLocal("firmware.bin", sampleFile, false, err), "duplicate without overwrite rejected");
    Expect(server.PublishLocal("firmware.bin", sampleFile, true, err), "duplicate with overwrite succeeds");
    Expect(server.PublishLocal("uImage", sampleFile, false, err), "second PublishLocal succeeds");

    const auto published = server.ListPublished();
    Expect(published.size() == 2 && server.PublishedCount() == 2, "ListPublished returns 2 items");
    Expect(server.Unpublish("uImage"), "Unpublish existing succeeds");
    Expect(!server.Unpublish("uImage"), "Unpublish missing returns false");
    Expect(server.PublishedCount() == 1, "PublishedCount decrements after Unpublish");

    // 3. Local root directory validation & listing (excluding subdirectories)
    std::wstring normalized;
    Expect(ValidateLocalTftpDirectory(tempDir, normalized, err), "valid local TFTP dir passes");
    Expect(!normalized.empty(), "normalized dir populated");
    Expect(!ValidateLocalTftpDirectory(L"relative/path", normalized, err), "relative local TFTP dir rejected");
    Expect(!ValidateLocalTftpDirectory(tempDir + L"\\..\\nope", normalized, err), "traversal local dir rejected");

    std::vector<TftpDirectoryEntry> entries;
    Expect(ListLocalTftpDirectory(tempDir, entries, err), "ListLocalTftpDirectory succeeds");
    Expect(entries.size() == 1 && entries[0].name == "firmware.bin" && entries[0].sizeKnown &&
               entries[0].size == 22,
           "ListLocalTftpDirectory finds only top-level regular files with exact size");

    Expect(server.SetLocalRoot(tempDir, err), "SetLocalRoot succeeds");
    Expect(server.RootConfig().mode == TftpRootMode::Local, "RootConfig mode is Local");
    server.ClearRoot();
    Expect(server.RootConfig().mode == TftpRootMode::None, "ClearRoot resets RootConfig");

    // 4. Local IP enumeration returns at least one IPv4 string
    const auto ips = TftpServer::EnumerateLocalIps();
    Expect(!ips.empty(), "EnumerateLocalIps returns at least one IPv4 address");

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    return g_failures == 0 ? 0 : 1;
}
