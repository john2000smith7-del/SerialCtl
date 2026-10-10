#include "CmdConnection.h"
#include "QueuedConnection.h"
#include <condition_variable>
#include <iostream>
#include <mutex>
using namespace serialctl;
int main()
{
    QueuedConnection cmd(std::make_unique<CmdConnection>());
    std::string output;
    std::mutex mutex;
    std::condition_variable ready;
    std::wstring error;
    if (!cmd.Start(
            [&](const Bytes &bytes) {
                std::lock_guard<std::mutex> lock(mutex);
                output.append(bytes.begin(), bytes.end());
                ready.notify_all();
            },
            [](const auto &, bool) {}, error))
    {
        std::wcerr << error;
        return 1;
    }
    std::string command =
        "set SERIALCTL_TEST=retained\r\necho SERIALCTL_BEGIN-%SERIALCTL_TEST%\r\n";
    if (!cmd.Send(Bytes(command.begin(), command.end()), error))
        return 2;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4), [&] {
                return output.find("SERIALCTL_BEGIN-retained") != std::string::npos;
            }))
            return 3;
    }
    command = "echo SERIALCTL_SECOND-%SERIALCTL_TEST%\r\n";
    cmd.Send(Bytes(command.begin(), command.end()), error);
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4), [&] {
                return output.find("SERIALCTL_SECOND-retained") != std::string::npos;
            }))
            return 4;
    }
    cmd.Stop();
    if (cmd.IsConnected())
        return 5;
    std::cout << "CMD session retains state and shuts down\n";
    return 0;
}
