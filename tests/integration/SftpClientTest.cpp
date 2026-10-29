#include "SftpClient.h"
#include "FilePaths.h"
#include "Win32Helpers.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <stdexcept>
using namespace serialctl;
namespace serialctl {
struct SftpClientTestAccess {
    static bool CleanupWithDeadline(const SftpClient &client, std::wstring &error, bool &launched) {
        std::wstring output;
        return client.RunBatch(L"serialctl-remove-exact \".serialctl-upload-test.tmp\"\nquit\n",
                               1200, output, error, SftpClient::TransferKind::None, 0, {}, nullptr,
                               &launched, GetTickCount64() + 1200);
    }
};
}
void Require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int main(int argc, char**) {
    try {
        std::wstring error;
        SftpClient normal(L"pipe-normal", 22, L"fixture", L"fixture-password");
        Require(normal.CreateDirectory(L"/test", error), "SFTP password pipe failed or command line contained -pw");
        if (argc > 1) return 0;
        {
            namespace fs = std::filesystem;
            auto root = fs::temp_directory_path() / (L"SerialCtl-long-path-" + std::to_wstring(GetCurrentProcessId()));
            std::wstring directory = root.wstring();
            for (int i = 0; i < 5; ++i) directory += L"\\中文 SFTP path " + std::wstring(50, L'x');
            directory = ExtendedFilePath(directory);
            fs::create_directories(directory);
            const auto current = fs::path(ModuleFilePath()).parent_path();
            for (auto name : {L"sftp_client_test.exe", L"plink.exe", L"psftp.exe"})
                Require(CopyFileW((current / name).c_str(), (directory + L"\\" + name).c_str(), FALSE) != FALSE, "copy long-path fixture");
            const auto executable = directory + L"\\sftp_client_test.exe";
            auto command = QuoteCommandLineArgument(executable) + L" --long-path";
            wchar_t oldTemp[32768]{}, oldTmp[32768]{};
            GetEnvironmentVariableW(L"TEMP", oldTemp, 32768); GetEnvironmentVariableW(L"TMP", oldTmp, 32768);
            SetEnvironmentVariableW(L"TEMP", directory.c_str()); SetEnvironmentVariableW(L"TMP", directory.c_str());
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                                nullptr, nullptr, &startup, &process) != FALSE;
            SetEnvironmentVariableW(L"TEMP", oldTemp); SetEnvironmentVariableW(L"TMP", oldTmp);
            Require(created, "launch executable beyond MAX_PATH");
            const auto waited = WaitForSingleObject(process.hProcess, 15000);
            if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
            DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hProcess); CloseHandle(process.hThread);
            // Keep the prefix on the short root too: recursive traversal will
            // create longer child paths from it, beyond the Win32 MAX_PATH cap.
            fs::remove_all(ExtendedFilePath(root.wstring(), true));
            Require(waited == WAIT_OBJECT_0 && code == 0, "SFTP resolves sibling executables and batch TEMP beyond MAX_PATH");
        }
        SftpClient overflow(L"pipe-overflow", 22, L"fixture", L"fixture-password");
        Require(!overflow.CreateDirectory(L"/test", error) && error.find(L"32 MiB") != std::wstring::npos,
                "Unbounded output was accepted as success");
        SftpClient continuous(L"pipe-cancel", 22, L"fixture", L"fixture-password");
        std::atomic_bool cancel{false};
        const auto eventName = L"Local\\SerialCtl.SftpOutput." + std::to_wstring(GetCurrentProcessId());
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        Require(ready != nullptr, "Cannot create output synchronization event");
        SetEnvironmentVariableW(L"SERIALCTL_TEST_OUTPUT_READY", eventName.c_str());
        bool streaming = false;
        const auto start = GetTickCount64();
        std::thread stop([&] {
            streaming = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
            Sleep(50);
            cancel = true;
        });
        const bool completed = continuous.CreateDirectory(L"/test", error, &cancel);
        stop.join();
        SetEnvironmentVariableW(L"SERIALCTL_TEST_OUTPUT_READY", nullptr);
        CloseHandle(ready);
        Require(streaming && !completed && error.find(L"取消") != std::wstring::npos && GetTickCount64() - start < 3000,
                "Continuous output starved cancellation");
        {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / (L"SerialCtl-SftpCancel-" + std::to_wstring(GetCurrentProcessId()));
            fs::create_directories(root);
            struct Remove { fs::path root; ~Remove() { std::error_code ignored; fs::remove_all(root, ignored); } } remove{root};
            const auto file = root / L"本地 文件.txt";
            const auto log = root / L"batch.log";
            DeleteFileW(log.c_str());
            { std::ofstream output(file, std::ios::binary); output << "fixture"; Require(output.good(), "Create upload fixture"); }
            SetEnvironmentVariableW(L"SERIALCTL_TEST_SFTP_BATCH_LOG", log.c_str());
            const auto event = L"Local\\SerialCtl.SftpUpload." + std::to_wstring(GetCurrentProcessId());
            HANDLE uploadReady = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
            Require(uploadReady != nullptr, "Cannot create upload synchronization event");
            SetEnvironmentVariableW(L"SERIALCTL_TEST_OUTPUT_READY", event.c_str());
            std::atomic_bool uploadCancel{false};
            bool uploadStarted = false;
            std::thread cancelUpload([&] {
                uploadStarted = WaitForSingleObject(uploadReady, 5000) == WAIT_OBJECT_0;
                uploadCancel = true;
            });
            SftpClient upload(L"pipe-upload-cancel", 22, L"fixture", L"fixture-password");
            const bool uploaded = upload.Upload(file.wstring(), L"/受控 目录", false, error, {}, &uploadCancel);
            cancelUpload.join();
            SetEnvironmentVariableW(L"SERIALCTL_TEST_OUTPUT_READY", nullptr);
            SetEnvironmentVariableW(L"SERIALCTL_TEST_SFTP_BATCH_LOG", nullptr);
            CloseHandle(uploadReady);
            Require(uploadStarted && !uploaded && error.find(L"取消") != std::wstring::npos,
                    "Controlled upload cancellation did not occur");
            std::ifstream batches(log, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(batches)), std::istreambuf_iterator<char>());
            const auto first = text.find("BATCH-BEGIN\n");
            const auto second = text.find("BATCH-BEGIN\n", first + 1);
            const auto temporary = text.find(".serialctl-upload-", first);
            const auto temporaryEnd = text.find(".tmp", temporary);
            Require(first != std::string::npos && second != std::string::npos && temporary != std::string::npos &&
                    temporaryEnd != std::string::npos && temporaryEnd < second,
                    "Upload and cleanup batches were not both recorded");
            const auto uniqueName = text.substr(temporary, temporaryEnd + 4 - temporary);
            const auto cleanup = text.substr(second);
            const auto removeAt = cleanup.find("serialctl-remove-exact");
            const auto uniqueAt = cleanup.find(uniqueName);
            Require(removeAt != std::string::npos && uniqueAt != std::string::npos &&
                    cleanup.find("serialctl-remove-exact", removeAt + 1) == std::string::npos &&
                    cleanup.find(uniqueName, uniqueAt + 1) == std::string::npos &&
                    cleanup.find("BATCH-BEGIN\n", 1) == std::string::npos && cleanup.find("put ") == std::string::npos &&
                    cleanup.find("serialctl-rename-exact") == std::string::npos &&
                    cleanup.find(WideToMultiByte(file.filename().wstring(), CP_UTF8)) == std::string::npos,
                    "Cancellation cleanup did not target only the upload's unique temporary file");
        }
        {
            const auto event = L"Local\\SerialCtl.SftpProbe." + std::to_wstring(GetCurrentProcessId());
            HANDLE probeReady = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
            Require(probeReady != nullptr, "Cannot create probe synchronization event");
            SetEnvironmentVariableW(L"SERIALCTL_TEST_PROBE_READY", event.c_str());
            bool probeStarted = false;
            std::thread observer([&] { probeStarted = WaitForSingleObject(probeReady, 5000) == WAIT_OBJECT_0; });
            SftpClient probe(L"pipe-probe-hang", 22, L"fixture", L"fixture-password");
            bool launched = false;
            const auto began = GetTickCount64();
            const bool cleaned = SftpClientTestAccess::CleanupWithDeadline(probe, error, launched);
            const auto elapsed = GetTickCount64() - began;
            observer.join();
            SetEnvironmentVariableW(L"SERIALCTL_TEST_PROBE_READY", nullptr);
            CloseHandle(probeReady);
            Require(probeStarted && !cleaned && !launched && elapsed < 2000,
                    "Cleanup deadline did not include a blocked host key probe");
            std::cout << "SFTP cleanup host-key deadline ms=" << elapsed << '\n';
        }
        std::cout << "SFTP secure password, output limits, cancellation and bounded exact cleanup passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
