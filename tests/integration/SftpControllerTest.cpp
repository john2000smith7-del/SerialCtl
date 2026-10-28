#include "SftpController.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace serialctl {
struct SftpControllerTestAccess {
    using Work = std::function<void(SftpController::Result&, const std::atomic_bool*, const SftpProgressCallback&)>;
    static void Control(SftpController& controller, Work work) { controller.controlledWork_ = std::move(work); }
};
}
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Gate {
    std::mutex mutex; std::condition_variable wake; bool entered = false, released = false;
    void Run(const std::atomic_bool* cancel) {
        std::unique_lock<std::mutex> lock(mutex); entered = true; wake.notify_all();
        while (!released && !cancel->load()) wake.wait_for(lock, std::chrono::milliseconds(10));
    }
    void Entered() {
        std::unique_lock<std::mutex> lock(mutex);
        Check(wake.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }), "controlled worker did not enter");
    }
};
}
int main() {
    try {
        SftpController controller;
        std::atomic_uint completions{0}, progressWakes{0};
        controller.SetCallbacks([&] { ++completions; }, [&] { ++progressWakes; });
        SftpController::Session session;
        SftpControllerTestAccess::Control(controller, [](auto& result, const auto*, const auto&) {
            result.success = true; result.listing.directory = L"/controlled";
            SftpEntry item; item.name = L"file"; item.size = 12; result.listing.entries.push_back(item);
        });
        Check(controller.List(1, {}, L"."), "start owned directory worker");
        controller.Join();
        auto result = controller.TakeResult();
        Check(result && completions == 1 && controller.Busy(), "completion remains controller-owned until UI consumption");
        Check(controller.Finish(*result, &session, true) && !controller.Busy() &&
              session.sftpHomeDirectory == L"/controlled" && controller.Directory() == L"/controlled" &&
              controller.Entries().size() == 1, "directory completion updates model and first home identity");
        std::wstring error;
        const auto download = controller.EnqueueDownload(1, L"/controlled/file", L"controller-destination.tmp", 100, false, error);
        Check(download && !controller.EnqueueDownload(1, L"/other", L"CONTROLLER-DESTINATION.TMP", 100, false, error),
              "queued writes to same local destination are rejected");
        SftpControllerTestAccess::Control(controller, [](auto& done, const auto*, const auto& progress) {
            for (int percent = 0; percent <= 100; ++percent) {
                SftpTransferProgress value{}; value.total = 100; value.transferred = percent; value.percent = percent;
                progress(value);
            }
            done.success = true;
        });
        Check(controller.Begin(download, {}), "begin immutable transfer request");
        controller.Join();
        auto progress = controller.TakeProgress();
        Check(progress && progressWakes == 1 && progress->progress.percent == 100 && controller.ApplyProgress(*progress),
              "progress handoff coalesces to latest bounded snapshot");
        result = controller.TakeResult(); Check(result && result->success, "owned transfer result delivered");
        controller.Finish(*result, &session, true);
        Check(controller.Transfers()[0].state == SftpController::TransferState::Completed &&
              !controller.ActiveTransfer(), "completed transfer leaves no active worker state");
        auto gate = std::make_shared<Gate>();
        SftpControllerTestAccess::Control(controller, [gate](auto&, const auto* cancel, const auto&) { gate->Run(cancel); });
        const auto canceled = controller.EnqueueUpload(1, L"controller-input.tmp", L"/controlled", error);
        Check(controller.Begin(canceled, {}), "begin cancellation fixture"); gate->Entered();
        const auto start = std::chrono::steady_clock::now(); controller.Cancel(canceled);
        const auto cancelUsec = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        Check(cancelUsec < 100000, "cancel acceptance blocks the UI caller");
        controller.Join(); result = controller.TakeResult(); Check(result && result->canceled, "worker observes persistent cancellation");
        controller.Finish(*result, &session, true);
        Check(controller.Transfers()[1].state == SftpController::TransferState::Canceled, "cancellation remains canceled at completion");
        controller.Retry(canceled, false);
        Check(controller.Transfers()[1].state == SftpController::TransferState::Failed, "retry cannot silently bind to another session");
        controller.Retry(canceled, true);
        Check(controller.Next() && controller.Next()->id == canceled && controller.Next()->transferred == 0,
              "retry is explicit and resets submission progress");
        controller.CancelSession(1);
        const auto other = controller.EnqueueUpload(2, L"another.tmp", L"/other", error);
        Check(controller.Next() && controller.Next()->id == other, "session cancellation does not cancel another session's queue");
        controller.Cancel(other);
        controller.ClearFinished(); Check(controller.Transfers().empty(), "finished history can be removed without active worker loss");
        // Unregister before destruction: a completed job retains its owned result
        // without calling a retired window sink.
        DWORD warmHandles = 0; nlohmann::json trace = nlohmann::json::array();
        for (int cycle = -1; cycle < 16; ++cycle) {
            gate = std::make_shared<Gate>();
            SftpControllerTestAccess::Control(controller, [gate](auto&, const auto* cancel, const auto&) { gate->Run(cancel); });
            controller.SetCallbacks([&] { ++completions; }, {});
            const auto before = completions.load();
            Check(controller.List(1, {}, L"."), "repeated operation launch"); gate->Entered();
            controller.ClearCallbacks(); controller.CancelIo(); controller.Join();
            Check(completions == before, "unregistered sink was called after its lifetime ended");
            result = controller.TakeResult(); Check(result && result->canceled, "retired sink cannot orphan completion storage");
            controller.Finish(*result, &session, true);
            DWORD handles = 0; Check(GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE, "read owned-worker resource count");
            if (cycle == -1) warmHandles = handles;
            trace.push_back({{"cycle", cycle}, {"handles", handles}});
            Check(handles <= warmHandles + 3, "owned SFTP workers accumulate handles across completed lifetimes");
        }
        controller.SetCallbacks([] { throw std::runtime_error("controlled notification failure"); }, {});
        SftpControllerTestAccess::Control(controller, [](auto&, const auto*, const auto&) { throw std::runtime_error("controlled backend failure"); });
        Check(controller.Mutate(1, {}, SftpController::Mutation::Delete, {L"/controlled"}, L"", {}), "exception fixture launch");
        controller.Join(); result = controller.TakeResult();
        Check(result && !result->success && !result->error.empty(), "backend and notification exceptions preserve owned failure result");
        controller.Finish(*result, &session, true); Check(!controller.Busy(), "failed operation releases busy state after consumption");
        const nlohmann::json evidence{{"scope", "owned SFTP controller workers with controlled backend; no network or hardware"},
            {"cycles", 16}, {"cancelAcceptanceUsec", cancelUsec}, {"warmHandles", warmHandles}, {"resourceTrace", trace}};
        std::cout << evidence.dump() << '\n';
        wchar_t evidenceDirectory[32768]{};
        if (GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", evidenceDirectory, 32768))
            std::ofstream(std::filesystem::path(evidenceDirectory) / L"sftp-controller.json") << evidence.dump(2);
        std::cout << "SFTP owned workers, queue/session state, coalesced progress, cancellation, unregister and exceptions passed; controlled backend, no network/hardware\n";
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
    return 0;
}
