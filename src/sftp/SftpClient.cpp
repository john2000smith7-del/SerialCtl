#include "SftpClient.h"
#include "ChildProcess.h"
#include "PuttyHostKey.h"
#include "PasswordPipe.h"
#include "ProcessOutput.h"
#include "SftpModel.h"
#include "Win32Helpers.h"
#include "FilePaths.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace serialctl {
namespace {

std::wstring TrimText(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring DecodeOutput(const std::string& bytes) {
    if (bytes.empty()) return {};
    const int utf8Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    const UINT codePage = utf8Count > 0 ? CP_UTF8 : CP_ACP;
    return MultiByteToWide(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), codePage);
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return L".";
    if (separator == 0) return path.substr(0, 1);
    if (separator == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, separator);
}

bool CreateDownloadTemporaryPath(const std::wstring& localPath,
    std::wstring& temporaryPath, std::wstring& error) {
    const std::wstring directory = DirectoryOf(localPath);
    if (!CreateUniqueTemporaryFile(directory, temporaryPath)) {
        error = L"无法创建下载临时文件：" + Win32ErrorMessage();
        return false;
    }
    return true;
}

bool CreateRemoteUploadTemporaryName(std::wstring& name, std::wstring& error) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        error = L"无法生成上传临时文件名。";
        return false;
    }
    wchar_t text[40]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0) {
        error = L"无法生成上传临时文件名。";
        return false;
    }
    std::wstring token = text;
    token.erase(std::remove(token.begin(), token.end(), L'{'), token.end());
    token.erase(std::remove(token.begin(), token.end(), L'}'), token.end());
    name = L".serialctl-upload-" + token + L".tmp";
    return true;
}

bool LocalFileSize(const std::wstring& path, std::uint64_t& size) {
    HANDLE file = CreateFileW(ExtendedFilePath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER value{};
    const bool ok = GetFileSizeEx(file, &value) != FALSE && value.QuadPart >= 0;
    CloseHandle(file);
    if (ok) size = static_cast<std::uint64_t>(value.QuadPart);
    return ok;
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& text, std::wstring& error) {
    const std::string bytes = WideToMultiByte(text, CP_UTF8);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建 SFTP 临时命令文件：" + Win32ErrorMessage();
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
        written == bytes.size();
    CloseHandle(file);
    if (!ok) error = L"无法写入 SFTP 临时命令文件：" + Win32ErrorMessage();
    return ok;
}

} // namespace

SftpClient::SftpClient(
    std::wstring host, std::uint16_t port, std::wstring username, std::wstring password)
    : host_(std::move(host)), port_(port), username_(std::move(username)), password_(std::move(password)) {}

bool SftpClient::IsAvailable() {
    return !FindPsftp().empty();
}

bool SftpClient::List(const std::wstring& directory, SftpListing& listing,
    std::wstring& error, const std::atomic_bool* cancel) const {
    const std::wstring quotedDirectory = QuotePsftpBatchWord(directory.empty() ? L"." : directory);
    if (quotedDirectory.empty()) {
        error = L"远端目录名称包含不支持的字符。";
        return false;
    }
    std::wstring output;
    if (!RunBatch(L"cd " + quotedDirectory + L"\npwd\nls\nquit\n",
            2 * 60 * 1000, output, error, TransferKind::None, 0, {}, cancel)) return false;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    try {
        SftpModelListing parsed = ParseSftpListingOutput(output,
            directory.empty() ? L"." : directory,
            {static_cast<int>(now.wYear), static_cast<int>(now.wMonth), static_cast<int>(now.wDay)});
        listing.directory = std::move(parsed.directory);
        listing.entries = std::move(parsed.entries);
    } catch (const std::length_error &) {
        listing = {};
        error = L"SFTP 目录超过 16384 条或文本上限；未显示不完整目录，请进入更小的子目录";
        return false;
    }
    return true;
}

bool SftpClient::Upload(
    const std::wstring& localPath, const std::wstring& remoteDirectory, bool replaceExisting,
    std::wstring& error, const SftpProgressCallback& progress,
    const std::atomic_bool* cancel) const {
    const std::wstring local = QuotePsftpBatchWord(ExtendedFilePath(localPath));
    const std::wstring remote = QuotePsftpBatchWord(remoteDirectory.empty() ? L"." : remoteDirectory);
    const std::wstring name = QuotePsftpBatchWord(FileNameOf(localPath));
    std::wstring temporaryName;
    if (!CreateRemoteUploadTemporaryName(temporaryName, error)) return false;
    const std::wstring temporary = QuotePsftpBatchWord(temporaryName);
    const std::wstring temporaryExact = QuotePsftpBatchWord(temporaryName);
    if (local.empty() || remote.empty() || name.empty() ||
        temporary.empty() || temporaryExact.empty()) {
        error = L"上传路径包含不支持的字符。";
        return false;
    }
    std::uint64_t totalBytes = 0;
    LocalFileSize(localPath, totalBytes);
    std::wstring output;
    const std::wstring renameCommand = replaceExisting ?
        L"serialctl-replace-exact " : L"serialctl-rename-exact ";
    const bool success = RunBatch(L"cd " + remote + L"\nput " + local + L" " + temporary +
        L"\n" + renameCommand + temporary + L" " + name + L"\nquit\n",
        10 * 60 * 1000, output, error, TransferKind::Upload, totalBytes, progress, cancel);
    if (!success) {
        // Cancellation used to return here and leave the remote .tmp behind. The
        // cleanup is bounded (10 s), only removes the unique name generated for this
        // upload, and deliberately ignores the already-set cancel flag.
        std::wstring cleanupOutput;
        std::wstring cleanupError;
        RunBatch(L"cd " + remote + L"\nserialctl-remove-exact " + temporaryExact + L"\nquit\n",
            10000, cleanupOutput, cleanupError, TransferKind::None, 0, {}, nullptr);
    }
    return success;
}

bool SftpClient::Download(
    const std::wstring& remotePath, const std::wstring& localPath, std::uint64_t expectedSize,
    bool replaceExisting, std::wstring& error, const SftpProgressCallback& progress,
    const std::atomic_bool* cancel) const {
    const std::wstring remote = QuotePsftpBatchWord(remotePath);
    std::wstring partialPath;
    if (!CreateDownloadTemporaryPath(localPath, partialPath, error)) return false;
    const std::wstring local = QuotePsftpBatchWord(partialPath);
    if (remote.empty() || local.empty()) {
        error = L"下载路径包含不支持的字符。";
        DeleteFileW(partialPath.c_str());
        return false;
    }
    std::wstring output;
    if (!RunBatch(L"get -- " + remote + L" " + local + L"\nquit\n",
            10 * 60 * 1000, output, error, TransferKind::Download,
            expectedSize, progress, cancel)) {
        DeleteFileW(partialPath.c_str());
        return false;
    }
    DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
    if (replaceExisting) moveFlags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(partialPath.c_str(), ExtendedFilePath(localPath).c_str(), moveFlags)) {
        const DWORD moveError = GetLastError();
        error = (moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS) ?
            L"本地目标在下载期间已出现，未覆盖该文件。" :
            L"无法保存下载文件：" + Win32ErrorMessage(moveError);
        DeleteFileW(partialPath.c_str());
        return false;
    }
    return true;
}

bool SftpClient::CreateDirectory(const std::wstring& path, std::wstring& error,
    const std::atomic_bool* cancel) const {
    const std::wstring quoted = QuotePsftpBatchWord(path);
    if (quoted.empty()) {
        error = L"目录名称包含不支持的字符。";
        return false;
    }
    std::wstring output;
    return RunBatch(L"mkdir " + quoted + L"\nquit\n", 60000, output, error,
        TransferKind::None, 0, {}, cancel);
}

bool SftpClient::Rename(
    const std::wstring& source, const std::wstring& destination, std::wstring& error,
    const std::atomic_bool* cancel) const {
    const std::wstring quotedSource = QuotePsftpBatchWord(source);
    const std::wstring quotedDestination = QuotePsftpBatchWord(destination);
    if (quotedSource.empty() || quotedDestination.empty()) {
        error = L"远端路径包含不支持的字符。";
        return false;
    }
    std::wstring output;
    return RunBatch(L"serialctl-rename-exact " + quotedSource + L" " +
        quotedDestination + L"\nquit\n",
        60000, output, error, TransferKind::None, 0, {}, cancel);
}

bool SftpClient::Delete(
    const std::vector<SftpDeleteTarget>& targets, std::wstring& error,
    const std::atomic_bool* cancel) const {
    if (targets.empty()) return true;
    std::wstring commands;
    for (const SftpDeleteTarget& target : targets) {
        const std::wstring quoted = QuotePsftpBatchWord(target.path);
        if (quoted.empty()) {
            error = L"远端路径包含不支持的字符。";
            return false;
        }
        commands += target.directory ? L"serialctl-rmdir-exact " :
            L"serialctl-remove-exact ";
        commands += quoted + L"\n";
    }
    commands += L"quit\n";
    std::wstring output;
    return RunBatch(commands, 2 * 60 * 1000, output, error,
        TransferKind::None, 0, {}, cancel);
}

bool SftpClient::ChangeMode(const std::vector<std::wstring>& paths,
    const std::wstring& mode, std::wstring& error, const std::atomic_bool* cancel) const {
    if (paths.empty()) return true;
    if ((mode.size() != 3 && mode.size() != 4) ||
        std::any_of(mode.begin(), mode.end(), [](wchar_t value) { return value < L'0' || value > L'7'; })) {
        error = L"权限必须是三位或四位八进制数。";
        return false;
    }
    std::wstring commands;
    for (const std::wstring& path : paths) {
        const std::wstring quoted = QuotePsftpWildcardLiteral(path);
        if (quoted.empty()) {
            error = L"远端路径包含不支持的字符。";
            return false;
        }
        commands += L"chmod " + mode + L" " + quoted + L"\n";
    }
    commands += L"quit\n";
    std::wstring output;
    return RunBatch(commands, 2 * 60 * 1000, output, error,
        TransferKind::None, 0, {}, cancel);
}

bool SftpClient::RunBatch(const std::wstring& commands, DWORD timeoutMilliseconds,
    std::wstring& output, std::wstring& error, TransferKind transferKind,
    std::uint64_t totalBytes, const SftpProgressCallback& progress,
    const std::atomic_bool* cancel) const {
    const std::wstring psftp = FindPsftp();
    if (psftp.empty()) {
        error = L"未找到 psftp.exe。请保留它与 serialctl.exe 位于同一目录。";
        return false;
    }
    const std::wstring plink = FindPlink();
    std::wstring hostKey;
    if (plink.empty() || !ResolvePuttyHostKey(plink, host_, port_, username_, password_,
            hostKey, error, cancel)) {
        if (plink.empty()) error = L"未找到 plink.exe，无法验证 SFTP 主机密钥。";
        return false;
    }
    PasswordPipe password;
    if (!password_.empty() && !password.Open(password_, error)) return false;

    std::vector<wchar_t> tempDirectory(32768);
    std::wstring batchPath;
    const auto tempLength = GetTempPathW(static_cast<DWORD>(tempDirectory.size()), tempDirectory.data());
    if (!tempLength || tempLength >= tempDirectory.size() ||
        !CreateUniqueTemporaryFile(tempDirectory.data(), batchPath)) {
        error = L"无法创建 SFTP 临时文件：" + Win32ErrorMessage();
        return false;
    }
    if (!WriteUtf8File(batchPath.c_str(), commands, error)) {
        DeleteFileW(batchPath.c_str());
        return false;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &security, 65536)) {
        DeleteFileW(batchPath.c_str());
        error = L"创建 SFTP 输出管道失败：" + Win32ErrorMessage();
        return false;
    }
    if (!SetHandleInheritable(outputRead, false,
            L"设置 SFTP 输出管道读取端继承属性失败", error)) {
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        DeleteFileW(batchPath.c_str());
        return false;
    }
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        error = L"打开 SFTP 空输入失败：" + Win32ErrorMessage();
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        DeleteFileW(batchPath.c_str());
        return false;
    }

    std::wstring command = QuoteCommandLineArgument(psftp) +
        L" -serialctl-machine-list -batch -P " + std::to_wstring(port_) +
        L" -l " + QuoteCommandLineArgument(username_);
    if (!password_.empty()) command += L" -pwfile " + QuoteCommandLineArgument(password.Name());
    if (!hostKey.empty()) command += L" -hostkey " + QuoteCommandLineArgument(hostKey);
    command += L" -b " + QuoteCommandLineArgument(batchPath.c_str()) +
        L" " + QuoteCommandLineArgument(host_);
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');

    RestrictedProcessStartup startup;
    if (!startup.Initialize(nullInput, outputWrite, outputWrite, {}, error)) {
        CloseHandle(nullInput);
        CloseHandle(outputWrite);
        CloseHandle(outputRead);
        DeleteFileW(batchPath.c_str());
        return false;
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(psftp.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        startup.StartupInfo(), &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(nullInput);
    CloseHandle(outputWrite);
    if (!created) {
        CloseHandle(outputRead);
        DeleteFileW(batchPath.c_str());
        error = L"启动 psftp.exe 失败：" + Win32ErrorMessage(createError);
        return false;
    }

    std::string bytes;
    const DWORD started = GetTickCount();
    DWORD lastActivityTick = started;
    DWORD lastProgressTick = started;
    std::uint64_t lastProgressBytes = 0;
    IO_COUNTERS baselineIo{};
    bool progressStarted = transferKind == TransferKind::None;
    int nativePercent = -1;
    bool timedOut = false;
    bool cancelled = false;
    bool tooLarge = false;
    constexpr size_t MaximumOutput = 32 * 1024 * 1024;
    for (;;) {
        password.Poll();
        const auto previous = bytes.size();
        const auto read = ReadProcessOutput(outputRead, bytes, MaximumOutput, cancel);
        tooLarge = read == PipeRead::Limit;
        cancelled = read == PipeRead::Cancelled;
        if (tooLarge || cancelled || password.Failed()) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
        const bool receivedOutput = bytes.size() != previous;
        if (receivedOutput && transferKind != TransferKind::None) {
            lastActivityTick = GetTickCount();
            constexpr size_t ProgressTailBytes = 2048;
            const size_t offset = bytes.size() > ProgressTailBytes ?
                bytes.size() - ProgressTailBytes : 0;
            const int parsed = ParsePsftpProgressPercent(
                DecodeOutput(bytes.substr(offset)));
            if (parsed >= 0) nativePercent = std::max(nativePercent, parsed);
        }
        if (!progressStarted && transferKind != TransferKind::None) {
            const char* marker = transferKind == TransferKind::Upload ? "local:" : "remote:";
            if (bytes.find(marker) != std::string::npos || nativePercent >= 0) {
                GetProcessIoCounters(process.hProcess, &baselineIo);
                progressStarted = true;
                lastProgressTick = GetTickCount();
                if (progress) progress({0, totalBytes, 0, 0, totalBytes == 0 ? -1 : 0});
            }
        }
        const DWORD now = GetTickCount();
        if (progress && progressStarted && transferKind != TransferKind::None &&
            now - lastProgressTick >= 100) {
            IO_COUNTERS currentIo{};
            if (GetProcessIoCounters(process.hProcess, &currentIo)) {
                const std::uint64_t current = transferKind == TransferKind::Upload
                    ? currentIo.ReadTransferCount : currentIo.WriteTransferCount;
                const std::uint64_t baseline = transferKind == TransferKind::Upload
                    ? baselineIo.ReadTransferCount : baselineIo.WriteTransferCount;
                const std::uint64_t raw = current >= baseline ? current - baseline : 0;
                const std::uint64_t transferred = nativePercent >= 0 && totalBytes > 0
                    ? totalBytes * static_cast<std::uint64_t>(nativePercent) / 100ULL
                    : (totalBytes == 0 ? raw : std::min(totalBytes, raw));
                const DWORD elapsed = std::max<DWORD>(1, now - lastProgressTick);
                const std::uint64_t speed = transferred >= lastProgressBytes
                    ? (transferred - lastProgressBytes) * 1000ULL / elapsed : 0;
                SftpTransferProgress value;
                value.transferred = transferred;
                value.total = totalBytes;
                value.bytesPerSecond = speed;
                value.percent = nativePercent >= 0 ? nativePercent :
                    (totalBytes == 0 ? -1 : static_cast<int>(transferred * 100ULL / totalBytes));
                value.remainingSeconds = speed > 0 && totalBytes > transferred
                    ? static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFULL,
                        (totalBytes - transferred + speed - 1) / speed)) : 0;
                progress(value);
                if (transferred > lastProgressBytes) lastActivityTick = now;
                lastProgressBytes = transferred;
                lastProgressTick = now;
            }
        }
        if (WaitForSingleObject(process.hProcess, read == PipeRead::More ? 0 : 20) == WAIT_OBJECT_0) break;
        if (cancel && cancel->load()) {
            cancelled = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
        const DWORD timeoutStart = transferKind == TransferKind::None ? started : lastActivityTick;
        if (now - timeoutStart >= timeoutMilliseconds) {
            timedOut = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }
    if (!tooLarge && !cancelled && !timedOut) {
        PipeRead read;
        do { read = ReadProcessOutput(outputRead, bytes, MaximumOutput, cancel); }
        while (read == PipeRead::More);
        tooLarge = read == PipeRead::Limit;
        cancelled = read == PipeRead::Cancelled;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(outputRead);
    DeleteFileW(batchPath.c_str());

    output = DecodeOutput(bytes);
    if (tooLarge || password.Failed()) {
        output.clear();
        error = tooLarge ? L"SFTP 输出超过 32 MiB，已停止；目录或操作结果不完整" : L"SFTP 安全密码通道写入失败";
        return false;
    }
    if (cancelled) {
        error = L"传输已取消。";
        return false;
    }
    if (timedOut) {
        error = transferKind == TransferKind::None ? L"SFTP 操作超时。" :
            L"SFTP 传输长时间无进展，已停止。";
        return false;
    }
    if (exitCode != 0) {
        error = TrimText(output);
        if (error.empty()) error = L"SFTP 操作失败，退出码 " + std::to_wstring(exitCode);
        if (error.size() > 600) error = error.substr(error.size() - 600);
        return false;
    }
    if (progress && transferKind != TransferKind::None)
        progress({totalBytes, totalBytes, 0, 0, 100});
    return true;
}

std::wstring SftpClient::FindPsftp() {
    std::wstring path = ModuleFilePath();
    if (path.empty()) return {};
    const size_t slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"psftp.exe";
    path = ExtendedFilePath(path);
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : std::wstring{};
}

std::wstring SftpClient::FindPlink() {
    std::wstring path = FindPsftp();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    path += L"plink.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : std::wstring{};
}

} // namespace serialctl
