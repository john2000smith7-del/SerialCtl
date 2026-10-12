#include "UpdateClient.h"
#include "Win32Helpers.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#ifdef SERIALCTL_UPDATE_TESTING
#include <cstdio>
#endif
#include <fstream>
#include <stdexcept>

namespace serialctl::update {
namespace {

#ifdef SERIALCTL_UPDATE_TESTING
std::atomic<unsigned long long> httpStatesCreated{0}, httpStatesDestroyed{0}, httpClosingCallbacks{0};
#endif

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {
        if (!value) throw std::runtime_error("Cannot initialize update HTTP request");
    }
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

struct Url {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

void CheckCancelled(const std::atomic<bool>& cancel) {
    if (cancel.load()) throw std::runtime_error("Update cancelled");
}

Url ParseUrl(const std::wstring& text) {
    if (text.empty() || text.size() > 2048 || std::any_of(text.begin(), text.end(), [](wchar_t c) {
            return c <= 32 || c >= 127 || c == L'\\' || c == L'@' || c == L'#' || c == L'?';
        })) throw std::runtime_error("Update URL must be a static HTTP(S) URL without credentials or query");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        !parts.dwUrlPathLength || parts.lpszUrlPath[parts.dwUrlPathLength - 1] == L'/')
        throw std::runtime_error("Invalid static update manifest URL");
    return {std::wstring(parts.lpszHostName, parts.dwHostNameLength),
            std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength), parts.nPort,
            parts.nScheme == INTERNET_SCHEME_HTTPS};
}

// WinHTTP asynchronous completions let the worker enforce one wall-clock deadline
// even if a server drips headers/body forever. On cancellation the request closes;
// its callback-owned state (including the pending read buffer) survives until the
// final HANDLE_CLOSING notification. No UI thread, detached worker or test hook
// is needed to interrupt native network waits.
class AsyncRequest {
public:
    AsyncRequest(HINTERNET connection, const Url& url)
        : handle_(WinHttpOpenRequest(connection, L"GET", url.path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    url.secure ? WINHTTP_FLAG_SECURE : 0)) {
        if (!handle_) throw std::runtime_error("Cannot create update HTTP request");
        try {
            state_ = new State;
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state_);
            if (!WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
                throw std::runtime_error("Cannot initialize update HTTP callback context");
            constexpr DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
                WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
            if (WinHttpSetStatusCallback(handle_, Callback, notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
                throw std::runtime_error("Cannot initialize update HTTP callbacks");
        } catch (...) {
            WinHttpCloseHandle(handle_);
            delete state_;
            throw;
        }
    }
    ~AsyncRequest() {
        // HANDLE_CLOSING releases the callback reference. It may arrive before or
        // after CloseHandle returns; the owner's reference covers both orders.
        WinHttpCloseHandle(handle_);
        state_->Release();
    }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;
    HINTERNET Get() const { return handle_; }
    void Send(const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        Prepare(cancel, deadline);
        const BOOL started = WinHttpSendRequest(handle_, L"Accept-Encoding: identity\r\nCache-Control: no-cache\r\n", static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(state_));
        CheckStarted(started);
        Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancel, deadline);
    }
    void Receive(const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        Prepare(cancel, deadline);
        CheckStarted(WinHttpReceiveResponse(handle_, nullptr));
        Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancel, deadline);
    }
    DWORD Read(const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        Prepare(cancel, deadline);
        CheckStarted(WinHttpReadData(handle_, state_->buffer.data(), static_cast<DWORD>(state_->buffer.size()), nullptr));
        Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, cancel, deadline);
        return state_->transferred.load();
    }
    const unsigned char* Bytes() const { return state_->buffer.data(); }
private:
    struct State {
        HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<unsigned> references{2};
        std::atomic<DWORD> status{0}, error{0}, transferred{0};
        std::array<unsigned char, 64 * 1024> buffer{};
        State() {
            if (!completed) throw std::runtime_error("Cannot create update HTTP completion event");
#ifdef SERIALCTL_UPDATE_TESTING
            ++httpStatesCreated;
#endif
        }
        ~State() {
            if (completed) {
                const BOOL closed = CloseHandle(completed);
#ifdef SERIALCTL_UPDATE_TESTING
                if (!closed) std::fprintf(stderr, "[FAIL] HTTP state event CloseHandle Windows error %lu\n", GetLastError());
#else
                (void)closed;
#endif
            }
#ifdef SERIALCTL_UPDATE_TESTING
            ++httpStatesDestroyed;
#endif
        }
        void Release() { if (references.fetch_sub(1) == 1) delete this; }
    };
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
        auto* state = reinterpret_cast<State*>(context);
        if (!state) return;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
#ifdef SERIALCTL_UPDATE_TESTING
            ++httpClosingCallbacks;
#endif
            state->Release(); return;
        }
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            const auto* error = static_cast<const WINHTTP_ASYNC_RESULT*>(information);
            state->error = error && length >= sizeof(WINHTTP_ASYNC_RESULT) ? error->dwError : ERROR_GEN_FAILURE;
        } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            state->transferred = length;
        } else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
                   status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
        state->status = status;
        SetEvent(state->completed);
    }
    static void Check(const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        CheckCancelled(cancel);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Update download deadline exceeded");
    }
    void Prepare(const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        Check(cancel, deadline);
        ResetEvent(state_->completed);
        state_->status = state_->error = state_->transferred = 0;
    }
    static void CheckStarted(BOOL started) {
        if (!started && GetLastError() != ERROR_IO_PENDING)
            throw std::runtime_error("Update HTTP operation failed (Windows error " + std::to_string(GetLastError()) + ")");
    }
    void Wait(DWORD expected, const std::atomic<bool>& cancel, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            Check(cancel, deadline);
            const DWORD result = WaitForSingleObject(state_->completed, 25);
            if (result == WAIT_TIMEOUT) continue;
            if (result != WAIT_OBJECT_0) throw std::runtime_error("Cannot wait for update HTTP completion");
            Check(cancel, deadline);
            if (state_->error.load())
                throw std::runtime_error("Update HTTP operation failed (Windows error " + std::to_string(state_->error.load()) + ")");
            if (state_->status.load() != expected) throw std::runtime_error("Unexpected update HTTP completion");
            return;
        }
    }
    HINTERNET handle_ = nullptr;
    State* state_ = nullptr;
};

// No proxy autodiscovery, automatic credentials, cookies, redirects, decompression,
// retry or TLS validation overrides. Caller runs on its own joined worker.
std::vector<unsigned char> Download(const Url& url, std::size_t limit, const std::atomic<bool>& cancel,
                                   const std::chrono::steady_clock::time_point deadline) {
    CheckCancelled(cancel);
    InternetHandle session(WinHttpOpen(L"SerialCtl updater/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000))
        throw std::runtime_error("Cannot set update HTTP timeouts");
    if (url.secure) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
            throw std::runtime_error("TLS 1.2 is unavailable on this Windows installation");
    }
    InternetHandle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
    AsyncRequest request(connection.value, url);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.Get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)))
        throw std::runtime_error("Cannot restrict update HTTP request");
    request.Send(cancel, deadline);
    request.Receive(cancel, deadline);
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("Update server must return HTTP 200 without a redirect");
    // Restrict declared length before allocation, and enforce the actual byte bound
    // even for a chunked body or a dishonest Content-Length.
    DWORD declared = 0;
    size = sizeof(declared);
    bool hasLength = WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                         WINHTTP_HEADER_NAME_BY_INDEX, &declared, &size,
                                         WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (hasLength && declared > limit) throw std::runtime_error("HTTP update body exceeds size limit");
    std::vector<unsigned char> data;
    for (;;) {
        const DWORD count = request.Read(cancel, deadline);
        if (!count) break;
        if (count > limit - data.size()) throw std::runtime_error("HTTP update body exceeds size limit");
        data.insert(data.end(), request.Bytes(), request.Bytes() + count);
    }
    if (hasLength && data.size() != declared) throw std::runtime_error("Update body is truncated");
    return data;
}

void WriteNew(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create staged update file");
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("Cannot save complete update file; check disk space and permissions");
}

void CheckNoReparseDirectory(const std::filesystem::path& path) {
    std::filesystem::path current;
    for (const auto& part : std::filesystem::absolute(path).lexically_normal()) {
        current /= part;
        if (current == current.root_name()) continue;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Installation/update directory cannot contain reparse points");
    }
}

std::filesystem::path NewStage(const std::filesystem::path& install) {
    CheckNoReparseDirectory(install);
    const auto root = install / L".serialctl-update";
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Cannot create update directory; installation must be writable");
    CheckNoReparseDirectory(root);
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        throw std::runtime_error("Cannot initialize update staging randomness");
    std::array<unsigned char, 16> bytes{};
    const bool randomOk = CryptGenRandom(provider, static_cast<DWORD>(bytes.size()), bytes.data()) != FALSE;
    CryptReleaseContext(provider, 0);
    if (!randomOk) throw std::runtime_error("Cannot create random update staging name");
    std::wstring token;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto c : bytes) { token += digits[c >> 4]; token += digits[c & 15]; }
    const auto stage = root / token;
    if (!CreateDirectoryW(stage.c_str(), nullptr)) throw std::runtime_error("Cannot create isolated update staging");
    if (!CreateDirectoryW((stage / L"files").c_str(), nullptr)) {
        RemoveDirectoryW(stage.c_str());
        throw std::runtime_error("Cannot create update payload directory");
    }
    return stage;
}

UpdateResult Run(const std::filesystem::path& exePath, const std::string& currentVersion,
                 const std::atomic<bool>& cancel, const std::vector<unsigned char>* key) {
    UpdateResult result;
    std::filesystem::path stage;
    try {
        CheckCancelled(cancel);
        const auto install = std::filesystem::absolute(exePath).parent_path().lexically_normal();
        const auto config = install / L"update.ini";
        std::array<wchar_t, 2050> setting{};
        const DWORD length = GetPrivateProfileStringW(L"update", L"url", L"", setting.data(),
                                                      static_cast<DWORD>(setting.size()), config.c_str());
        if (!length) {
            result.status = UpdateStatus::Disabled;
            result.message = L"自动更新未配置";
            return result;
        }
        if (length >= setting.size() - 1) throw std::runtime_error("Update URL exceeds length limit");
        CompareVersions(currentVersion, currentVersion);
        const auto manifestUrl = ParseUrl(std::wstring(setting.data(), length));
        auto signatureUrl = manifestUrl;
        signatureUrl.path += L".sig";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        const auto metadataDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        const auto manifestBytes = Download(manifestUrl, kMaxManifestBytes, cancel, metadataDeadline);
        const auto signatureBytes = Download(signatureUrl, 256, cancel, metadataDeadline);
        Manifest manifest;
#ifdef SERIALCTL_UPDATE_TESTING
        if (key) manifest = testing::VerifySignedManifest(manifestBytes, signatureBytes, *key);
        else
#else
        (void)key;
#endif
            manifest = VerifySignedManifest(manifestBytes, signatureBytes);
        result.version = manifest.version;
        if (CompareVersions(manifest.version, currentVersion) <= 0) {
            result.status = UpdateStatus::NoUpdate;
            result.message = L"当前已是最新版本";
            return result;
        }
        // An update whose startup failed is quarantined until an operator removes
        // this local marker or publishes a different version. Never retry it in a loop.
        std::ifstream failed(install / L".serialctl-update" / L"failed-version.txt", std::ios::binary);
        std::array<char, 64> failedText{};
        if (failed) {
            failed.read(failedText.data(), static_cast<std::streamsize>(failedText.size() - 1));
            std::string failedVersion(failedText.data(), static_cast<size_t>(failed.gcount()));
            while (!failedVersion.empty() && (failedVersion.back() == '\r' || failedVersion.back() == '\n'))
                failedVersion.pop_back();
            if (failedVersion == manifest.version) {
                result.status = UpdateStatus::NoUpdate;
                result.message = L"此版本此前启动失败，已暂停自动重试；请联系管理员";
                return result;
            }
        }
        CheckCancelled(cancel);
        stage = NewStage(install);
        WriteNew(stage / L"update.json", manifestBytes);
        WriteNew(stage / L"update.json.sig", signatureBytes);
        for (const auto& file : manifest.files) {
            CheckCancelled(cancel);
            auto url = manifestUrl;
            url.path.erase(url.path.rfind(L'/') + 1);
            url.path.append(file.url.begin(), file.url.end());
            auto bytes = Download(url, static_cast<size_t>(file.size), cancel, deadline);
            if (bytes.size() != file.size) throw std::runtime_error("Downloaded file length differs from signed manifest");
            const auto destination = stage / L"files" / std::filesystem::u8path(file.name);
            WriteNew(destination, bytes);
            if (Sha256File(destination, &cancel) != file.sha256)
                throw std::runtime_error("Downloaded file SHA-256 differs from signed manifest");
        }
        CheckCancelled(cancel);
#ifdef SERIALCTL_UPDATE_TESTING
        if (key) testing::VerifyStagedManifest(stage, currentVersion, *key, &cancel);
        else
#endif
            VerifyStagedManifest(stage, currentVersion, &cancel);
        CheckCancelled(cancel);
        result.status = UpdateStatus::Ready;
        result.message = L"更新下载并校验完成，等待安全安装";
        result.stagingDirectory = stage;
    } catch (const std::exception& error) {
        if (!stage.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(stage, ignored);
        }
        result.status = cancel.load() ? UpdateStatus::Cancelled : UpdateStatus::Failed;
        const std::string errorText = error.what();
        result.message = cancel.load() ? L"自动更新已取消" :
            L"自动更新失败，继续使用当前版本：" +
            MultiByteToWide(reinterpret_cast<const std::uint8_t*>(errorText.data()), errorText.size(), CP_UTF8);
    }
    return result;
}

} // namespace

UpdateResult CheckAndStage(const std::filesystem::path& exePath, const std::string& currentVersion,
                           const std::atomic<bool>& cancel) {
    return Run(exePath, currentVersion, cancel, nullptr);
}

#ifdef SERIALCTL_UPDATE_TESTING
namespace testing {
HttpLifetimeCounts GetHttpLifetimeCounts() {
    return {httpStatesCreated.load(), httpStatesDestroyed.load(), httpClosingCallbacks.load()};
}
UpdateResult CheckAndStage(const std::filesystem::path& exePath, const std::string& currentVersion,
                          const std::atomic<bool>& cancel, const std::vector<unsigned char>& key) {
    return Run(exePath, currentVersion, cancel, &key);
}
}
#endif

} // namespace serialctl::update
