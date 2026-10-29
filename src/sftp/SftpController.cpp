#include "SftpController.h"
#include "FilePaths.h"
#include <algorithm>
namespace serialctl {
namespace {
std::wstring LocalFileNameOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}
bool ExistingLocalPathsReferToSameFile(
    const std::wstring& left, const std::wstring& right) {
    if (CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL)
        return true;
    HANDLE leftFile = CreateFileW(ExtendedFilePath(left).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (leftFile == INVALID_HANDLE_VALUE) return false;
    HANDLE rightFile = CreateFileW(ExtendedFilePath(right).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (rightFile == INVALID_HANDLE_VALUE) {
        CloseHandle(leftFile);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION leftInfo{};
    BY_HANDLE_FILE_INFORMATION rightInfo{};
    const bool same = GetFileInformationByHandle(leftFile, &leftInfo) &&
        GetFileInformationByHandle(rightFile, &rightInfo) &&
        leftInfo.dwVolumeSerialNumber == rightInfo.dwVolumeSerialNumber &&
        leftInfo.nFileIndexHigh == rightInfo.nFileIndexHigh &&
        leftInfo.nFileIndexLow == rightInfo.nFileIndexLow;
    CloseHandle(rightFile);
    CloseHandle(leftFile);
    return same;
}
std::uint64_t LocalFileSize(const std::wstring& path) {
    HANDLE file = CreateFileW(ExtendedFilePath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    const bool success = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    CloseHandle(file);
    return success ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}
}
SftpController::~SftpController() { ClearCallbacks(); CancelIo(); Join(); }
void SftpController::SetCallbacks(Complete complete, ProgressSink progress) {
    std::lock_guard<std::mutex> lock(notifyMutex_);
    complete_ = std::move(complete); progress_ = std::move(progress);
}
void SftpController::ClearCallbacks() {
    std::lock_guard<std::mutex> lock(notifyMutex_); complete_ = {}; progress_ = {};
}
void SftpController::CancelIo() { operationCancel_ = true; transferCancel_ = true; }
void SftpController::Join() {
    if (operation_.joinable()) operation_.join();
    if (transfer_.joinable()) transfer_.join();
}
void SftpController::Notify(Result result) {
    std::lock_guard<std::mutex> lock(notifyMutex_);
    auto& slot = result.kind == Kind::Upload || result.kind == Kind::Download ? transferResult_ : operationResult_;
    slot.order = ++resultOrder_; slot.value = std::move(result);
    // Results remain owned by this controller if a sink has been unregistered
    // or rejects notification; no posted heap pointer can outlive its window.
    try { if (complete_) complete_(); } catch (...) {}
}
void SftpController::Notify(Progress progress) {
    std::lock_guard<std::mutex> lock(notifyMutex_);
    const bool notify = !latestProgress_;
    latestProgress_ = progress;
    try { if (notify && progress_) progress_(); } catch (...) {}
}
std::optional<SftpController::Result> SftpController::TakeResult() {
    std::lock_guard<std::mutex> lock(notifyMutex_);
    auto* slot = !operationResult_.value ? &transferResult_ : !transferResult_.value ? &operationResult_ :
        operationResult_.order < transferResult_.order ? &operationResult_ : &transferResult_;
    auto result = std::move(slot->value); slot->value.reset(); return result;
}
std::optional<SftpController::Progress> SftpController::TakeProgress() {
    std::lock_guard<std::mutex> lock(notifyMutex_);
    auto result = std::move(latestProgress_); latestProgress_.reset(); return result;
}
void SftpController::ToggleSort(SftpSortColumn column) {
    if (sortColumn_ == column) sortAscending_ = !sortAscending_;
    else { sortColumn_ = column; sortAscending_ = true; }
}
void SftpController::Sort(Session* selected) {
    SortSftpEntries(entries_, sortColumn_, sortAscending_);
    if (selected) selected->sftpEntries = entries_;
}
bool SftpController::StartOperation(std::uint64_t session, Kind kind, Work work) {
    if (!session || Busy()) return false;
    if (operation_.joinable()) operation_.join();
#ifdef SERIALCTL_SFTP_TESTING
    if (controlledWork_) work = controlledWork_;
#endif
    operationCancel_ = false; operationSession_ = session;
    try {
        operation_ = std::thread([this, session, kind, work = std::move(work)] {
            Result result; result.sessionId = session; result.kind = kind;
            try { work(result, &operationCancel_, {}); }
            catch (...) { result.error = kind == Kind::List ? L"SFTP 目录读取发生内部错误。" : L"SFTP 文件操作发生内部错误。"; }
            result.canceled = operationCancel_.load();
            Notify(std::move(result));
        });
        return true;
    } catch (...) { operationSession_ = 0; return false; }
}
bool SftpController::List(std::uint64_t session, Endpoint endpoint, const std::wstring& directory) {
    return StartOperation(session, Kind::List, [endpoint = std::move(endpoint), directory](Result& result,
        const std::atomic_bool* cancel, const SftpProgressCallback&) {
        SftpClient client(endpoint.host, endpoint.port, endpoint.username, endpoint.password);
        result.success = client.List(directory, result.listing, result.error, cancel);
    });
}
bool SftpController::Mutate(std::uint64_t session, Endpoint endpoint, Mutation kind,
    const std::vector<std::wstring>& paths, const std::wstring& value, const std::vector<bool>& directories) {
    if (paths.empty()) return false;
    return StartOperation(session, Kind::Mutation, [endpoint = std::move(endpoint), kind, paths, value, directories](Result& result,
        const std::atomic_bool* cancel, const SftpProgressCallback&) {
        SftpClient client(endpoint.host, endpoint.port, endpoint.username, endpoint.password);
        if (kind == Mutation::CreateDirectory)
            result.success = client.CreateDirectory(paths.front(), result.error, cancel);
        else if (kind == Mutation::Rename)
            result.success = paths.size() >= 2 && client.Rename(paths[0], paths[1], result.error, cancel);
        else if (kind == Mutation::Delete) {
            std::vector<SftpDeleteTarget> targets;
            for (size_t i = 0; i < paths.size(); ++i) targets.push_back({paths[i], i < directories.size() && directories[i]});
            result.success = client.Delete(targets, result.error, cancel);
        } else result.success = client.ChangeMode(paths, value, result.error, cancel);
    });
}
std::uint64_t SftpController::EnqueueUpload(std::uint64_t session, const std::wstring& local,
                                           const std::wstring& remote, std::wstring& error) {
    Transfer item; item.id = nextTransfer_++; item.sessionId = session; item.direction = Direction::Upload;
    item.name = LocalFileNameOf(local); item.localPath = local; item.remotePath = remote.empty() ? directory_ : remote;
    if (item.remotePath == directory_) {
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.name == item.name; });
        if (found != entries_.end()) {
            if (found->directory) { error = L"远端已有同名文件夹，无法上传该文件。"; return 0; }
            item.replaceExisting = true;
        }
    }
    item.total = LocalFileSize(local); const auto id = item.id; transfers_.push_back(std::move(item)); return id;
}
std::uint64_t SftpController::EnqueueDownload(std::uint64_t session, const std::wstring& remote,
    const std::wstring& local, std::uint64_t size, bool replace, std::wstring& error) {
    const bool duplicate = std::any_of(transfers_.begin(), transfers_.end(), [&](const auto& item) {
        return item.direction == Direction::Download && (item.state == TransferState::Queued || item.state == TransferState::Running) &&
               ExistingLocalPathsReferToSameFile(item.localPath, local);
    });
    if (duplicate) { error = L"已有任务正在写入同一本地路径，已跳过重复下载。"; return 0; }
    Transfer item; item.id = nextTransfer_++; item.sessionId = session; item.direction = Direction::Download;
    item.name = BaseNameSftpRemotePath(remote); item.localPath = local; item.remotePath = remote;
    item.total = size; item.replaceExisting = replace;
    const auto id = item.id; transfers_.push_back(std::move(item)); return id;
}
SftpController::Transfer* SftpController::Find(std::uint64_t id) {
    const auto item = std::find_if(transfers_.begin(), transfers_.end(), [id](const auto& value) { return value.id == id; });
    return item == transfers_.end() ? nullptr : &*item;
}
const SftpController::Transfer* SftpController::Next() const {
    const auto item = std::find_if(transfers_.begin(), transfers_.end(), [](const auto& value) { return value.state == TransferState::Queued; });
    return item == transfers_.end() ? nullptr : &*item;
}
bool SftpController::Begin(std::uint64_t id, Endpoint endpoint) {
    if (activeTransfer_) return false;
    auto* item = Find(id); if (!item || item->state != TransferState::Queued) return false;
    if (transfer_.joinable()) transfer_.join();
    item->state = TransferState::Running; item->error.clear(); item->percent = item->total == 0 ? -1 : 0;
    activeTransfer_ = id; transferCancel_ = false;
    const Transfer task = *item;
    Work work = [endpoint = std::move(endpoint), task](Result& result, const std::atomic_bool* cancel, const SftpProgressCallback& progress) {
        SftpClient client(endpoint.host, endpoint.port, endpoint.username, endpoint.password);
        if (task.direction == Direction::Upload)
            result.success = client.Upload(task.localPath, task.remotePath, task.replaceExisting, result.error, progress, cancel);
        else result.success = client.Download(task.remotePath, task.localPath, task.total, task.replaceExisting, result.error, progress, cancel);
    };
#ifdef SERIALCTL_SFTP_TESTING
    if (controlledWork_) work = controlledWork_;
#endif
    try {
        transfer_ = std::thread([this, task, work = std::move(work)] {
            Result result; result.sessionId = task.sessionId; result.transferId = task.id;
            result.kind = task.direction == Direction::Upload ? Kind::Upload : Kind::Download;
            try { work(result, &transferCancel_, [this, id = task.id](const auto& value) { Notify(Progress{id, value}); }); }
            catch (...) { result.error = L"SFTP 传输发生内部错误。"; }
            result.canceled = transferCancel_.load(); Notify(std::move(result));
        });
        return true;
    } catch (...) { item->state = TransferState::Failed; item->error = L"无法启动传输任务"; activeTransfer_ = 0; return false; }
}
void SftpController::Cancel(std::uint64_t id, const std::wstring& reason) {
    auto* item = Find(id); if (!item) return;
    if (item->state == TransferState::Queued) { item->state = TransferState::Canceled; item->error = reason; }
    else if (item->state == TransferState::Running) { transferCancel_ = true; item->error = L"正在取消…"; }
}
void SftpController::CancelSession(std::uint64_t session) {
    if (operationSession_ == session) operationCancel_ = true;
    for (auto& item : transfers_) if (item.sessionId == session) {
        if (item.state == TransferState::Queued) { item.state = TransferState::Canceled; item.error = L"连接已断开"; }
        else if (item.state == TransferState::Running) transferCancel_ = true;
    }
}
void SftpController::Retry(std::uint64_t id, bool available) {
    auto* item = Find(id);
    if (!item || (item->state != TransferState::Failed && item->state != TransferState::Canceled)) return;
    if (!available) { item->error = L"原连接已断开，无法重试"; item->state = TransferState::Failed; return; }
    item->state = TransferState::Queued; item->error.clear(); item->transferred = 0;
    item->bytesPerSecond = 0; item->remainingSeconds = 0; item->percent = item->total == 0 ? -1 : 0;
}
void SftpController::ClearFinished() {
    transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(), [](const auto& item) {
        return item.state == TransferState::Completed || item.state == TransferState::Failed || item.state == TransferState::Canceled;
    }), transfers_.end());
}
bool SftpController::ApplyProgress(const Progress& progress) {
    auto* item = Find(progress.transferId);
    if (!item || item->state != TransferState::Running) return false;
    item->transferred = progress.progress.transferred; item->total = progress.progress.total;
    item->bytesPerSecond = progress.progress.bytesPerSecond; item->remainingSeconds = progress.progress.remainingSeconds;
    item->percent = progress.progress.percent; return true;
}
bool SftpController::Finish(Result& result, Session* session, bool selected) {
    if (result.kind == Kind::Upload || result.kind == Kind::Download) {
        if (result.transferId != activeTransfer_) return false;
        if (transfer_.joinable()) transfer_.join();
        if (auto* item = Find(result.transferId)) {
            item->state = result.success ? TransferState::Completed : (result.canceled ? TransferState::Canceled : TransferState::Failed);
            item->error = result.error;
            if (result.success) {
                item->percent = 100; item->transferred = item->total;
                if (item->direction == Direction::Upload) { refreshSession_ = item->sessionId; refreshDirectory_ = item->remotePath; }
            }
        }
        activeTransfer_ = 0; return false;
    }
    if (result.sessionId != operationSession_) return false;
    if (operation_.joinable()) operation_.join();
    operationSession_ = 0;
    if (!session || !result.success || result.kind != Kind::List) return false;
    const bool learned = session->sftpHomeDirectory.empty();
    if (learned) session->sftpHomeDirectory = result.listing.directory;
    session->sftpDirectory = result.listing.directory; session->sftpEntries = std::move(result.listing.entries);
    if (selected) Select(*session);
    return learned;
}
std::pair<std::uint64_t, std::wstring> SftpController::TakeRefresh() {
    auto result = std::make_pair(refreshSession_, std::move(refreshDirectory_)); refreshSession_ = 0; refreshDirectory_.clear(); return result;
}
} // namespace serialctl
