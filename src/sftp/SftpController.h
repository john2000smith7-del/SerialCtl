#pragma once
#include "SftpClient.h"
#include "SftpModel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
namespace serialctl {
// UI-thread catalog/queue model with owned operation and transfer workers.
// Workers use immutable requests and never borrow MainWindow/session objects.
// Notifications must be nonblocking; ClearCallbacks is an unregister barrier.
class SftpController {
public:
    enum class Direction { Upload, Download };
    enum class TransferState { Queued, Running, Completed, Failed, Canceled };
    enum class Mutation { CreateDirectory, Rename, Delete, ChangeMode };
    enum class Kind { List, Upload, Download, Mutation };
    struct Session {
        bool sftpFollowTerminal = true;
        std::wstring terminalDirectory;
        bool terminalDirectoryFromOsc = false;
        std::wstring sftpDirectory = L".";
        std::wstring sftpHomeDirectory;
        std::wstring pendingSftpDirectory;
        std::vector<SftpEntry> sftpEntries;
    };
    struct Transfer {
        std::uint64_t id = 0;
        std::uint64_t sessionId = 0;
        Direction direction = Direction::Upload;
        TransferState state = TransferState::Queued;
        std::wstring name;
        std::wstring localPath;
        std::wstring remotePath;
        std::uint64_t transferred = 0;
        std::uint64_t total = 0;
        std::uint64_t bytesPerSecond = 0;
        std::uint32_t remainingSeconds = 0;
        int percent = -1;
        std::wstring error;
        bool replaceExisting = false;
    };

    struct Endpoint { std::wstring host; std::uint16_t port = 22; std::wstring username, password; };
    struct Result {
        std::uint64_t sessionId = 0, transferId = 0;
        Kind kind = Kind::List;
        bool success = false, canceled = false;
        SftpListing listing;
        std::wstring error;
    };
    struct Progress { std::uint64_t transferId = 0; SftpTransferProgress progress; };
    using Complete = std::function<void()>;
    using ProgressSink = std::function<void()>;
    SftpController() = default;
    ~SftpController();
    void SetCallbacks(Complete, ProgressSink);
    void ClearCallbacks();
    std::optional<Result> TakeResult();
    std::optional<Progress> TakeProgress();
    void CancelIo();
    void Join();
    bool HasOperationThread() const { return operation_.joinable(); }
    bool Busy() const { return operationSession_ != 0; }
    std::uint64_t OperationSession() const { return operationSession_; }
    std::uint64_t ActiveTransfer() const { return activeTransfer_; }
    void Select(const Session& state) { directory_ = state.sftpDirectory; entries_ = state.sftpEntries; }
    void ClearView() { directory_ = L"."; entries_.clear(); }
    const std::wstring& Directory() const { return directory_; }
    const std::vector<SftpEntry>& Entries() const { return entries_; }
    SftpSortColumn SortColumn() const { return sortColumn_; }
    bool SortAscending() const { return sortAscending_; }
    void ToggleSort(SftpSortColumn column);
    void Sort(Session* selected);
    bool List(std::uint64_t session, Endpoint, const std::wstring&);
    bool Mutate(std::uint64_t session, Endpoint, Mutation, const std::vector<std::wstring>&,
                const std::wstring&, const std::vector<bool>&);
    std::uint64_t EnqueueUpload(std::uint64_t session, const std::wstring& local, const std::wstring& remote,
                                std::wstring& error);
    std::uint64_t EnqueueDownload(std::uint64_t session, const std::wstring& remote, const std::wstring& local,
                                  std::uint64_t size, bool replace, std::wstring& error);
    const std::vector<Transfer>& Transfers() const { return transfers_; }
    const Transfer* Next() const;
    bool Begin(std::uint64_t transfer, Endpoint);
    void Cancel(std::uint64_t transfer, const std::wstring& reason = L"传输已取消");
    void CancelSession(std::uint64_t session);
    void Retry(std::uint64_t transfer, bool sessionAvailable);
    void ClearFinished();
    bool ApplyProgress(const Progress&);
    bool Finish(Result&, Session*, bool selected); // returns whether this learned the home directory
    std::pair<std::uint64_t, std::wstring> TakeRefresh();
private:
    using Work = std::function<void(Result&, const std::atomic_bool*, const SftpProgressCallback&)>;
    bool StartOperation(std::uint64_t, Kind, Work);
    void Notify(Result);
    void Notify(Progress);
    Transfer* Find(std::uint64_t);
#ifdef SERIALCTL_SFTP_TESTING
    friend struct SftpControllerTestAccess;
    Work controlledWork_;
#endif
    mutable std::mutex notifyMutex_;
    Complete complete_;
    ProgressSink progress_;
    struct CompletionSlot { std::uint64_t order = 0; std::optional<Result> value; };
    CompletionSlot operationResult_, transferResult_;
    std::optional<Progress> latestProgress_;
    std::uint64_t resultOrder_ = 0;
    std::thread operation_, transfer_;
    std::atomic_bool operationCancel_{false}, transferCancel_{false};
    std::uint64_t operationSession_ = 0, activeTransfer_ = 0, nextTransfer_ = 1, refreshSession_ = 0;
    std::wstring refreshDirectory_;
    std::vector<Transfer> transfers_;
    std::wstring directory_ = L".";
    std::vector<SftpEntry> entries_;
    SftpSortColumn sortColumn_ = SftpSortColumn::Name;
    bool sortAscending_ = true;
};
} // namespace serialctl
