#include "TftpFileSource.h"
#include "TftpServer.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
    {
        std::ofstream out(sampleFile, std::ios::binary);
        out << "TFTP_UNIT_PAYLOAD_1234";
    }

    TftpServer server;
    std::wstring err;

    // 1. Path traversal / illegal filename rejection on PublishLocal & PublishCustom
    auto mock = std::make_shared<MockStreamTftpFileSource>(64);
    Expect(!server.PublishCustom("", mock, true, err), "empty TFTP name rejected");
    Expect(!server.PublishCustom("../secret.bin", mock, true, err), "parent traversal rejected");
    Expect(!server.PublishCustom("dir/uImage", mock, true, err), "forward slash rejected");
    Expect(!server.PublishCustom("dir\\uImage", mock, true, err), "backslash rejected");
    Expect(!server.PublishCustom("C:uImage", mock, true, err), "drive colon rejected");
    Expect(!server.PublishLocal("../etc/passwd", sampleFile, true, err), "PublishLocal traversal rejected");

    // 2. Valid publish / duplicate rejection / overwrite / unpublish
    Expect(server.PublishCustom("uImage", mock, false, err), "valid PublishCustom succeeds");
    Expect(!server.PublishCustom("uImage", mock, false, err), "duplicate without overwrite rejected");
    Expect(server.PublishCustom("uImage", mock, true, err), "duplicate with overwrite succeeds");
    Expect(server.PublishLocal("firmware.bin", sampleFile, false, err), "PublishLocal valid file succeeds");

    const auto published = server.ListPublished();
    Expect(published.size() == 2, "ListPublished returns 2 items");
    Expect(server.Unpublish("uImage"), "Unpublish existing succeeds");
    Expect(!server.Unpublish("uImage"), "Unpublish missing returns false");

    // 3. Local root directory validation & listing
    std::wstring normalized;
    Expect(ValidateLocalTftpDirectory(tempDir, normalized, err), "valid local TFTP dir passes");
    Expect(!normalized.empty(), "normalized dir populated");
    Expect(!ValidateLocalTftpDirectory(L"relative/path", normalized, err), "relative local TFTP dir rejected");
    Expect(!ValidateLocalTftpDirectory(tempDir + L"\\..\\nope", normalized, err), "traversal local dir rejected");

    std::vector<TftpDirectoryEntry> entries;
    Expect(ListLocalTftpDirectory(tempDir, entries, err), "ListLocalTftpDirectory succeeds");
    Expect(entries.size() == 1 && entries[0].name == "firmware.bin" && entries[0].sizeKnown &&
               entries[0].size == 22,
           "ListLocalTftpDirectory finds sample file with exact size");

    // 4. Remote SSH directory config validation
    SshRemoteConfig remoteCfg;
    remoteCfg.host = L"192.0.2.10";
    remoteCfg.username = L"tester";
    remoteCfg.port = 22;
    remoteCfg.remotePath = L"  /srv/tftp/  ";
    Expect(!ValidateRemoteTftpDirectoryConfig(remoteCfg, err), "missing hostKey rejected");

    remoteCfg.hostKey = L"ssh-ed25519 255 00:11:22:33";
    Expect(ValidateRemoteTftpDirectoryConfig(remoteCfg, err), "valid remote config normalized");
    Expect(remoteCfg.remotePath == L"/srv/tftp", "whitespace and trailing slash trimmed");

    remoteCfg.host = L"-oProxyCommand=bad";
    Expect(!ValidateRemoteTftpDirectoryConfig(remoteCfg, err), "leading dash host rejected");

    remoteCfg.host = L"192.0.2.10";
    remoteCfg.remotePath = L"../escape";
    Expect(!ValidateRemoteTftpDirectoryConfig(remoteCfg, err), "non-absolute remote path rejected");

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    return g_failures == 0 ? 0 : 1;
}
