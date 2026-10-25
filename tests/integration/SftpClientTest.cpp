#include "SftpClient.h"
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace serialctl;
void Require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int main() {
    try {
        std::wstring error;
        SftpClient normal(L"pipe-normal", 22, L"fixture", L"fixture-password");
        Require(normal.CreateDirectory(L"/test", error), "SFTP password pipe failed or command line contained -pw");
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
