#include "SftpClient.h"
#include "FilePaths.h"
#include "Win32Helpers.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace serialctl;
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
            fs::remove_all(ExtendedFilePath(root.wstring()));
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
        std::cout << "SFTP secure password, output limit and continuous-output cancellation passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
