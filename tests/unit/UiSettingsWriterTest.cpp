#include "UiSettingsWriter.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace serialctl;
using namespace std::chrono_literals;

namespace {
void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
UiSettingsWriter::Snapshot State(int value) {
    return {L"unused", {{L"Layout", L"RightPanelCollapsed", std::to_wstring(value)}}};
}
}

int main() {
    try {
        std::promise<void> entered, release;
        auto enteredFuture = entered.get_future();
        auto released = release.get_future().share();
        std::vector<std::wstring> written;
        UiSettingsWriter writer([&](const UiSettingsWriter::Snapshot &state) {
            if (state.entries[0].value == L"1") { entered.set_value(); released.wait(); }
            written.push_back(state.entries[0].value);
        });
        Require(writer.Submit(State(1)), "first settings snapshot was rejected");
        if (enteredFuture.wait_for(2s) != std::future_status::ready) {
            release.set_value();
            throw std::runtime_error("settings writer did not begin the controlled slow write");
        }
        auto submit = std::async(std::launch::async, [&] {
            for (int i = 2; i <= 1001; ++i) if (!writer.Submit(State(i))) return false;
            return true;
        });
        const bool responsive = submit.wait_for(2s) == std::future_status::ready;
        release.set_value();
        Require(submit.get() && responsive, "submission waited for slow settings storage");
        writer.Stop();
        Require(written.size() == 2 && written.front() == L"1" && written.back() == L"1001",
                "shutdown did not flush the most recent coalesced snapshot");
        Require(!writer.Submit(State(1002)), "writer accepted a snapshot after shutdown");

        wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"sct", 0, path),
                "cannot create profile fixture");
        struct RemoveFile { const wchar_t *path; ~RemoveFile() { DeleteFileW(path); } } cleanup{path};
        Require(WritePrivateProfileStringW(L"Unrelated", L"Keep", L"7", path) != FALSE,
                "cannot seed profile fixture");
        {
            UiSettingsWriter realWriter;
            UiSettingsWriter::Snapshot state{path, {{L"Layout", L"RightPanelCollapsed", L"1"},
                                                      {L"Pet", L"Size", L"96"}}};
            Require(realWriter.Submit(std::move(state)), "profile write was rejected");
            realWriter.Stop();
        }
        Require(GetPrivateProfileIntW(L"Layout", L"RightPanelCollapsed", 0, path) == 1 &&
                    GetPrivateProfileIntW(L"Pet", L"Size", 0, path) == 96 &&
                    GetPrivateProfileIntW(L"Unrelated", L"Keep", 0, path) == 7,
                "profile writes changed values or unrelated settings");
        std::cout << "UI settings writes remain responsive and flush the latest snapshot\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
