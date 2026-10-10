#include "CmdConnection.h"
#include "CmdLineEditor.h"
#include "QueuedConnection.h"
#include "SessionService.h"
#include "Win32Helpers.h"
#include <condition_variable>
#include <iostream>
#include <mutex>
using namespace serialctl;
int main() {
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
            [](const auto &, bool) {}, error)) {
        std::wcerr << error;
        return 1;
    }
    std::string command = "set SERIALCTL_TEST=retained\r\necho SERIALCTL_BEGIN-%SERIALCTL_TEST%\r\n";
    if (!cmd.Send(Bytes(command.begin(), command.end()), error))
        return 2;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4),
                            [&] { return output.find("SERIALCTL_BEGIN-retained") != std::string::npos; }))
            return 3;
    }
    CmdLineEditor editor;
    for (wchar_t ch : std::wstring(L"echX SERIALCTL_KEY_EDITED"))
        editor.Insert(std::wstring(1, ch));
    using Key = CmdLineEditor::Key;
    editor.Edit(Key::Home);
    editor.Edit(Key::Right);
    editor.Edit(Key::Right);
    editor.Edit(Key::Right);
    editor.Edit(Key::Delete);
    editor.Insert(L"o");
    editor.Edit(Key::End);
    editor.Insert(L"X");
    editor.Edit(Key::Backspace);
    auto edited = editor.Text() + L"\r\n";
    auto encoded = WideToMultiByte(edited, GetOEMCP());
    if (!cmd.Send(SessionService::CmdBatch(Bytes(encoded.begin(), encoded.end())), error))
        return 6;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4),
                            [&] { return output.find("SERIALCTL_KEY_EDITED") != std::string::npos; }))
            return 7;
    }
    for (auto invalid : {Bytes{'n', 'o'}, Bytes{'e', 'c', 'h', 'o', 0, 13, 10}, Bytes{3, 13, 10}, Bytes(8200, 'x')}) {
        bool rejected = false;
        try {
            SessionService::CmdBatch(invalid);
        } catch (...) {
            rejected = true;
        }
        if (!rejected)
            return 8;
    }
    command = "echo SERIALCTL_SECOND-%SERIALCTL_TEST%\r\n";
    cmd.Send(Bytes(command.begin(), command.end()), error);
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4),
                            [&] { return output.find("SERIALCTL_SECOND-retained") != std::string::npos; }))
            return 4;
    }
    // Cancellation of a pipe writer must terminate the real CMD process tree.
    std::string blocked = "echo SERIALCTL_BLOCK_BEGIN\r\nping -n 100 127.0.0.1 >nul\r\n";
    cmd.Send(Bytes(blocked.begin(), blocked.end()), error);
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(4),
                            [&] { return output.find("SERIALCTL_BLOCK_BEGIN") != std::string::npos; }))
            return 10;
    }
    Bytes queued;
    auto line = "rem " + std::string(500, 'x') + "\r\n";
    while (queued.size() + line.size() <= 65536)
        queued.insert(queued.end(), line.begin(), line.end());
    bool bounded = false;
    for (int i = 0; i < 200; ++i) {
        if (!cmd.Send(queued, error)) {
            bounded = true;
            break;
        }
    }
    if (!bounded)
        return 11;
    auto stopStarted = GetTickCount64();
    cmd.Stop();
    if (GetTickCount64() - stopStarted > 5000)
        return 9;
    if (cmd.IsConnected())
        return 5;
    std::cout << "CMD session retains state and shuts down\n";
    return 0;
}
