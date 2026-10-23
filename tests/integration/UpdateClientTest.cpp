#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include "UpdateClient.h"
#include "UpdateZipFixture.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>

using namespace serialctl::update;
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Json = nlohmann::json;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    std::cout << "[PASS] " << message << '\n';
}
void Reject(const std::function<void()>& action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, message);
}
DWORD ThreadCount() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot count process threads");
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    DWORD count = 0;
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return count;
}
// Diagnostic only: ask the current process for object *types*, never object
// names or other processes' handles. These APIs are loaded dynamically so the
// production/Win7 import contract remains unchanged. Windows versions without
// ProcessHandleInformation simply report this optional snapshot unavailable.
using HandleTypes = std::map<std::string, unsigned long long>;
HandleTypes SnapshotHandleTypes(std::vector<ULONG_PTR>* eventValues = nullptr) {
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto queryProcess = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    const auto queryObject = reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryObject"));
    if (!queryProcess || !queryObject) return {{"diagnostic-unavailable", 1}};
    struct Entry {
        HANDLE value;
        ULONG_PTR handleCount, pointerCount;
        ULONG grantedAccess, typeIndex, attributes, reserved;
    };
    struct Snapshot { ULONG_PTR count, reserved; Entry handles[1]; };
    struct TypeName { USHORT length, maximumLength; PWSTR buffer; };
    std::vector<unsigned char> storage(64 * 1024);
    LONG status = -1;
    ULONG needed = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        status = queryProcess(GetCurrentProcess(), 51 /* ProcessHandleInformation */,
                              storage.data(), static_cast<ULONG>(storage.size()), &needed);
        if (status >= 0) break;
        const size_t next = (std::max)(storage.size() * 2, static_cast<size_t>(needed));
        if (next > 16 * 1024 * 1024) break;
        storage.resize(next);
    }
    if (status < 0) return {{"diagnostic-unavailable", 1}};
    const auto* snapshot = reinterpret_cast<const Snapshot*>(storage.data());
    const size_t headerBytes = offsetof(Snapshot, handles);
    if (snapshot->count > (storage.size() - headerBytes) / sizeof(Entry))
        return {{"diagnostic-invalid-length", 1}};
    std::map<ULONG, std::string> names;
    HandleTypes counts;
    for (ULONG_PTR index = 0; index < snapshot->count; ++index) {
        const auto& entry = snapshot->handles[index];
        auto known = names.find(entry.typeIndex);
        if (known == names.end()) {
            alignas(TypeName) std::array<unsigned char, 2048> typeStorage{};
            ULONG typeBytes = 0;
            if (queryObject(entry.value, 2 /* ObjectTypeInformation */, typeStorage.data(),
                            static_cast<ULONG>(typeStorage.size()), &typeBytes) < 0) continue;
            const auto* type = reinterpret_cast<const TypeName*>(typeStorage.data());
            const auto begin = reinterpret_cast<ULONG_PTR>(typeStorage.data());
            const auto name = reinterpret_cast<ULONG_PTR>(type->buffer);
            if (type->length % sizeof(wchar_t) || name < begin || name > begin + typeStorage.size() ||
                type->length > begin + typeStorage.size() - name) continue;
            std::string label;
            for (size_t character = 0; character < type->length / sizeof(wchar_t); ++character) {
                const wchar_t value = type->buffer[character];
                label += value >= 32 && value < 127 ? static_cast<char>(value) : '?';
            }
            known = names.emplace(entry.typeIndex, std::move(label)).first;
        }
        ++counts[known->second];
        if (eventValues && known->second == "Event")
            eventValues->push_back(reinterpret_cast<ULONG_PTR>(entry.value));
    }
    return counts;
}
// Native handle tracing is only a diagnostic in this test executable. Keep
// records within this process, and print module basenames/offsets, never handle
// names, object names, process paths, user names, or other processes' records.
bool BeginHandleTracing() {
    using Set = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG);
    const auto set = reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
    if (!set) return false;
    struct Enable { ULONG flags, totalSlots; } enable{0, 4096};
    const LONG status = set(GetCurrentProcess(), 32 /* ProcessHandleTracing */, &enable, sizeof(enable));
    std::cout << "[METRIC] native_handle_tracing_status=0x" << std::hex << static_cast<ULONG>(status) << std::dec << '\n';
    return status >= 0;
}
void EndHandleTracing() {
    using Set = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG);
    const auto set = reinterpret_cast<Set>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess"));
    if (set) set(GetCurrentProcess(), 32 /* ProcessHandleTracing */, nullptr, 0);
}
std::string TraceFrame(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) return "unresolved-module";
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return "unresolved-module";
    size_t begin = 0;
    for (size_t index = 0; index < length; ++index)
        if (path[index] == L'\\' || path[index] == L'/') begin = index + 1;
    std::string name;
    for (size_t index = begin; index < length; ++index)
        name += path[index] >= 32 && path[index] < 127 ? static_cast<char>(path[index]) : '?';
    std::ostringstream line;
    line << name << "+0x" << std::hex
         << reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(module);
    return line.str();
}
void PrintNewEventCreationTraces(const std::vector<ULONG_PTR>& before, const std::vector<ULONG_PTR>& after) {
    using Query = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query) return;
    struct Entry { HANDLE value; HANDLE processId, threadId; ULONG type; void* stack[16]; };
    struct Records { HANDLE value; ULONG count; Entry entries[1]; };
    unsigned sampled = 0;
    for (const auto value : after) {
        if (std::find(before.begin(), before.end(), value) != before.end()) continue;
        if (sampled++ >= 4) break;
        std::vector<unsigned char> storage(64 * 1024);
        auto* records = reinterpret_cast<Records*>(storage.data());
        records->value = reinterpret_cast<HANDLE>(value);
        ULONG needed = 0;
        const LONG status = query(GetCurrentProcess(), 32 /* ProcessHandleTracing */, storage.data(),
                                  static_cast<ULONG>(storage.size()), &needed);
        if (status < 0) {
            std::cout << "[METRIC] native_event_trace_sample=" << sampled << " status=0x"
                      << std::hex << static_cast<ULONG>(status) << std::dec << '\n';
            continue;
        }
        const size_t limit = (storage.size() - offsetof(Records, entries)) / sizeof(Entry);
        const auto count = (std::min)(static_cast<size_t>(records->count), limit);
        bool found = false;
        for (size_t entry = 0; entry < count; ++entry) {
            const auto& record = records->entries[entry];
            if (record.value != reinterpret_cast<HANDLE>(value) || record.type != 1 /* OPEN */) continue;
            found = true;
            std::cout << "[METRIC] native_event_trace_sample=" << sampled << " operation=open";
            for (unsigned frame = 0; frame < 16 && record.stack[frame]; ++frame)
                std::cout << " frame" << frame << '=' << TraceFrame(record.stack[frame]);
            std::cout << '\n';
        }
        if (!found) std::cout << "[METRIC] native_event_trace_sample=" << sampled << " open_trace_unavailable=1\n";
    }
}

void PrintHandleTypeDelta(const char* scenario, const HandleTypes& before, const HandleTypes& after) {
    HandleTypes combined = before;
    for (const auto& entry : after) combined.emplace(entry.first, 0);
    for (const auto& entry : combined) {
        const auto first = before.find(entry.first), last = after.find(entry.first);
        const auto start = first == before.end() ? 0 : first->second;
        const auto end = last == after.end() ? 0 : last->second;
        std::cout << "[METRIC] handle_type_scenario=" << scenario << " type=" << entry.first
                  << " before=" << start << " after=" << end
                  << " delta=" << static_cast<long long>(end) - static_cast<long long>(start) << '\n';
    }
}
Bytes Data(const std::string& value) { return Bytes(value.begin(), value.end()); }
void Write(const fs::path& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("Cannot write test fixture");
}

struct SigningKey {
    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    Bytes publicKey;
    SigningKey() {
        if (!CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
            !CryptGenKey(provider, AT_SIGNATURE, (2048u << 16) | CRYPT_EXPORTABLE, &key))
            throw std::runtime_error("Cannot generate isolated test signing key");
        DWORD size = 0;
        if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, nullptr, &size)) throw std::runtime_error("Export size failed");
        publicKey.resize(size);
        if (!CryptExportKey(key, 0, PUBLICKEYBLOB, 0, publicKey.data(), &size)) throw std::runtime_error("Export failed");
    }
    ~SigningKey() { if (key) CryptDestroyKey(key); if (provider) CryptReleaseContext(provider, 0); }
    Bytes Sign(const Bytes& data) const {
        HCRYPTHASH hash = 0;
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) throw std::runtime_error("Hash failed");
        DWORD size = 256;
        Bytes bytes(size);
        const bool ok = CryptHashData(hash, data.data(), static_cast<DWORD>(data.size()), 0) &&
            CryptSignHashW(hash, AT_SIGNATURE, nullptr, 0, bytes.data(), &size);
        CryptDestroyHash(hash);
        if (!ok || size != 256) throw std::runtime_error("Test signature failed");
        std::reverse(bytes.begin(), bytes.end());
        return bytes;
    }
};

struct Response {
    Bytes body;
    int status = 200;
    int declaredExtra = 0;
    int delayMs = 0;
    bool chunked = false;
    int headerByteDelayMs = 0;
    int bodyPauseMs = 0;
    // Keep Content-Length correct for the signed file while deliberately closing
    // the socket before the advertised body is delivered.
    size_t omitTailBytes = 0;
};

class HttpFixture {
public:
    HttpFixture() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("Cannot create HTTP fixture");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 16))
            throw std::runtime_error("Cannot bind HTTP fixture");
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }
    ~HttpFixture() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        closesocket(listener_);
    }
    std::string Url(const std::string& path = "/update.json") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    void Set(const std::string& path, Response response) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[path] = std::move(response);
    }
    unsigned Requests() const { return requests_.load(); }
    unsigned HeadersSent() const { return headersSent_.load(); }
private:
    static bool SendAll(SOCKET socket, const unsigned char* bytes, size_t size) {
        while (size) {
            const int count = send(socket, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0);
            if (count <= 0) return false;
            bytes += count;
            size -= static_cast<size_t>(count);
        }
        return true;
    }
    void Serve() {
        while (!stop_) {
            fd_set readSet;
            FD_ZERO(&readSet); FD_SET(listener_, &readSet);
            timeval timeout{0, 50000};
            if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD ioTimeout = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ioTimeout), sizeof(ioTimeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&ioTimeout), sizeof(ioTimeout));
            std::string request;
            char buffer[2048];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
                int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, static_cast<size_t>(count));
            }
            std::string path;
            const size_t first = request.find(' '), second = request.find(' ', first + 1);
            if (first != std::string::npos && second != std::string::npos)
                path = request.substr(first + 1, second - first - 1);
            Response response{Data("missing"), 404};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = responses_.find(path);
                if (found != responses_.end()) response = found->second;
            }
            ++requests_;
            for (int waited = 0; waited < response.delayMs && !stop_; waited += 10) Sleep(10);
            std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " Fixture\r\nConnection: close\r\n";
            if (response.status == 302) headers += "Location: /redirected.json\r\n";
            if (response.chunked) headers += "Transfer-Encoding: chunked\r\n\r\n";
            else headers += "Content-Length: " + std::to_string(response.body.size() + response.declaredExtra) + "\r\n\r\n";
            bool sent = true;
            if (response.headerByteDelayMs) {
                for (const unsigned char byte : headers) {
                    if (stop_ || !SendAll(client, &byte, 1)) { sent = false; break; }
                    for (int waited = 0; waited < response.headerByteDelayMs && !stop_; waited += 10) Sleep(10);
                }
            } else sent = SendAll(client, reinterpret_cast<const unsigned char*>(headers.data()), headers.size());
            ++headersSent_;
            for (int waited = 0; waited < response.bodyPauseMs && !stop_; waited += 10) Sleep(10);
            if (sent && response.chunked) {
                char chunkHeader[32];
                const int count = sprintf_s(chunkHeader, "%zx\r\n", response.body.size());
                SendAll(client, reinterpret_cast<unsigned char*>(chunkHeader), static_cast<size_t>(count));
            }
            if (sent) SendAll(client, response.body.data(),
                              response.body.size() - (std::min)(response.body.size(), response.omitTailBytes));
            if (sent && response.chunked) {
                const std::string tail = "\r\n0\r\n\r\n";
                SendAll(client, reinterpret_cast<const unsigned char*>(tail.data()), tail.size());
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> requests_{0}, headersSent_{0};
    std::thread thread_;
    std::mutex mutex_;
    std::map<std::string, Response> responses_;
};

struct Fixture {
    fs::path root;
    SigningKey key;
    HttpFixture server;
    Json manifest;
    std::map<std::string, Bytes> payload;
    std::atomic<bool> cancel{false};
    Fixture() {
        static std::atomic<unsigned long> nextFixture{0};
        root = fs::temp_directory_path() /
            (L"serialctl-update-client-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(nextFixture.fetch_add(1)));
        fs::create_directories(root);
        payload = {{"serialctl.exe", Data("native runtime test bytes")}, {"plink.exe", Data("pinned plink fixture")},
                   {"psftp.exe", Data("pinned psftp fixture")},
                   {"VERSION", Data("PROJECT_VERSION=V1.0.8\r\nCPP_VERSION=V1.0.8\r\n")}};
        manifest = {{"schema", 1}, {"product", "SerialCtl"}, {"architecture", "x64"}, {"version", "V1.0.8"},
                    {"files", Json::array()}};
        for (const auto& entry : payload) {
            const auto file = root / fs::u8path(entry.first);
            Write(file, entry.second);
            manifest["files"].push_back({{"name", entry.first}, {"size", entry.second.size()},
                                        {"sha256", Sha256File(file)}, {"url", "files/" + entry.first}});
            server.Set("/files/" + entry.first, {entry.second});
        }
        Publish(manifest);
        Configure(server.Url());
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void Publish(const Json& value, const std::string& path = "/update.json") {
        Bytes bytes = Data(value.dump());
        server.Set(path, {bytes});
        server.Set(path + ".sig", {key.Sign(bytes)});
    }
    void Configure(const std::string& url) { Write(root / L"update.ini", Data("[update]\r\nurl=" + url + "\r\n")); }
    UpdateResult Run(const std::string &version = "V1.0.7", const UpdateCallbacks &callbacks = {}) {
        return testing::CheckAndStage(root / L"serialctl.exe", version, cancel, key.publicKey, callbacks);
    }
    void Cleanup(const UpdateResult& result) {
        if (!result.stagingDirectory.empty()) fs::remove_all(result.stagingDirectory);
    }
};

Bytes Envelope(const Json &payload, const SigningKey &key) {
    const auto raw = Data(payload.dump());
    std::string signature;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : key.Sign(raw)) {
        signature += hex[byte >> 4];
        signature += hex[byte & 15];
    }
    return Data(Json{{"schema", 2}, {"payload", payload.dump()}, {"signature", signature}}.dump());
}
void VerifyRealArchive(const fs::path &metadata, const fs::path &archive, const fs::path &publicKey) {
    auto read = [](const fs::path &path) {
        std::ifstream file(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto manifest = testing::VerifyEmbeddedManifest(read(metadata), read(publicKey));
    const auto root = fs::temp_directory_path() / (L"serialctl-real-archive-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directory(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    std::atomic<bool> cancel{false};
    // Match the client: the archive is a regular file in its local staging
    // directory, even when this test receives it through a mapped host drive.
    const auto localArchive = root / L"package.zip";
    fs::copy_file(archive, localArchive);
    ExtractUpdateArchive(localArchive, manifest, root, cancel);
    Require(!fs::exists(root / L"drivers") && !fs::exists(root / L"update.ini"),
            "real full release ZIP extracts only signed runtime without drivers or configuration");
    std::cout << "Real release archive round trip passed: " << manifest.version << " / " << manifest.files.size()
              << " runtime files\n";
}

void RunArchiveTests() {
    Fixture fixture;
    auto payload = fixture.manifest;
    payload["schema"] = 2;
    auto members = fixture.payload;
    members["update.ini"] = Data("[update]\nurl=must-not-replace\n");
    members["drivers/vendor/setup.exe"] = Data("must not extract or execute");
    members["unexpected.exe"] = Data("must not install");
    auto publish = [&](const Bytes &archive) {
        const auto path = fixture.root / L"candidate.zip";
        Write(path, archive);
        payload["archive"] = {
            {"url", "SerialCtl-V1.0.8-Win7-x64.zip"}, {"size", archive.size()}, {"sha256", Sha256File(path)}};
        fixture.server.Set("/update.json", {Envelope(payload, fixture.key)});
        fixture.server.Set("/SerialCtl-V1.0.8-Win7-x64.zip", {archive});
    };
    const auto installed = Sha256File(fixture.root / L"serialctl.exe");
    for (bool deflated : {false, true}) {
        publish(testing::BuildTestZip(members, deflated));
        unsigned confirmations = 0;
        const auto beforeApproval = fixture.server.Requests();
        UpdateCallbacks decline;
        decline.confirm = [&](const std::string &version) {
            ++confirmations;
            Require(version == "V1.0.8", "confirmation receives verified newer version");
            Require(fixture.server.Requests() == beforeApproval + 1, "no ZIP request before confirmation");
            Require(!fs::exists(fixture.root / L".serialctl-update") ||
                        fs::is_empty(fixture.root / L".serialctl-update"),
                    "no staging before confirmation");
            return false;
        };
        Require(fixture.Run("V1.0.7", decline).status == UpdateStatus::Cancelled && confirmations == 1 &&
                    fixture.server.Requests() == beforeApproval + 1,
                "declining verified offer never downloads archive");
        bool sawDownload = false, sawVerify = false;
        UpdateCallbacks accept;
        accept.confirm = [](const std::string &) { return true; };
        accept.progress = [&](std::uint64_t received, std::uint64_t total, UpdatePhase phase) {
            Require(received <= total && total > 0, "download progress is bounded signed bytes");
            if (phase == UpdatePhase::Download)
                sawDownload = true;
            else
                sawVerify = true;
        };
        auto accepted = fixture.Run("V1.0.7", accept);
        Require(accepted.status == UpdateStatus::Ready && sawDownload && sawVerify,
                "approved offer downloads and reports real verification phase");
        fixture.Cleanup(accepted);
        const auto count = fixture.server.Requests();
        auto result = fixture.Run();
        Require(result.status == UpdateStatus::Ready && fixture.server.Requests() == count + 2,
                "two-file update requests only signed JSON and full ZIP");
        Require(!fs::exists(result.stagingDirectory / L"update.json.sig") &&
                    !fs::exists(result.stagingDirectory / L"files" / L"update.ini") &&
                    !fs::exists(result.stagingDirectory / L"files" / L"drivers") &&
                    !fs::exists(result.stagingDirectory / L"files" / L"unexpected.exe"),
                "ZIP staging excludes configuration, drivers and unlisted members");
        Require(testing::VerifyStagedManifest(result.stagingDirectory, "V1.0.7", fixture.key.publicKey).archive.size >
                    0,
                "installer re-verifies embedded signature and signed staged runtime");
        Require(!fs::exists(result.stagingDirectory / L"package.zip"),
                "full ZIP is discarded after verified extraction");
        Write(result.stagingDirectory / L"files" / L"serialctl.exe", Data("changed after staging"));
        Reject([&] { testing::VerifyStagedManifest(result.stagingDirectory, "V1.0.7", fixture.key.publicKey); },
               "extracted runtime tampering before installation is rejected");
        fixture.Cleanup(result);
    }
    members["serialctl.exe"] = Data(std::string(100000, 'X'));
    const auto path = fixture.root / L"huffman.bin";
    Write(path, members["serialctl.exe"]);
    for (auto &record : payload["files"])
        if (record["name"] == "serialctl.exe") {
            record["size"] = members["serialctl.exe"].size();
            record["sha256"] = Sha256File(path);
        }
    publish(testing::BuildTestZip(members, true));
    auto result = fixture.Run();
    Require(result.status == UpdateStatus::Ready &&
                Sha256File(result.stagingDirectory / L"files" / L"serialctl.exe") == Sha256File(path),
            "dynamic-Huffman DEFLATE preserves the signed output exactly");
    fixture.Cleanup(result);
    publish(testing::BuildTestZip(members, true, true));
    Require(fixture.Run().status == UpdateStatus::Failed, "validly signed ZIP with wrong CRC is rejected");
    members["../escape.exe"] = Data("never written");
    publish(testing::BuildTestZip(members, false));
    Require(fixture.Run().status == UpdateStatus::Failed && !fs::exists(fixture.root.parent_path() / L"escape.exe"),
            "signed ZIP path traversal is rejected even for an unselected member");
    members.erase("../escape.exe");
    publish(testing::BuildTestZip(members, true));
    auto envelope = Json::parse(Envelope(payload, fixture.key));
    envelope["payload"] = envelope["payload"].get<std::string>() + " ";
    fixture.server.Set("/update.json", {Data(envelope.dump())});
    auto count = fixture.server.Requests();
    Require(fixture.Run().status == UpdateStatus::Failed && fixture.server.Requests() == count + 1,
            "tampered embedded payload is rejected before any archive request");
    publish(testing::BuildTestZip(members, true));
    fixture.server.Set("/SerialCtl-V1.0.8-Win7-x64.zip", {Data("wrong archive")});
    Require(fixture.Run().status == UpdateStatus::Failed && fs::is_empty(fixture.root / L".serialctl-update"),
            "wrong or truncated ZIP is rejected and staging cleaned");
    publish(testing::BuildTestZip(members, true));
    count = fixture.server.Requests();
    Require(fixture.Run("V1.0.8").status == UpdateStatus::NoUpdate && fixture.server.Requests() == count + 1,
            "same-version two-file update never downloads ZIP");
    Require(Sha256File(fixture.root / L"serialctl.exe") == installed,
            "archive checks never modify the installed runtime");
}

// Split session/option/request construction from HTTP I/O so a native event
// lifetime issue cannot be mistaken for the callback state's completion event.
void ProbeNativeHttpLifetime(const char* scenario, unsigned constructionLevel) {
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    const auto typesBefore = SnapshotHandleTypes();
    bool operationsOk = true;
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        HINTERNET session = WinHttpOpen(L"SerialCtl updater lifetime diagnostic", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        if (!session) { operationsOk = false; continue; }
        if (constructionLevel >= 1 && !WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000)) operationsOk = false;
        if (constructionLevel >= 2) {
            HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", 9, 0);
            if (!connection) operationsOk = false;
            else {
                HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/", nullptr,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (!request) operationsOk = false;
                else if (!WinHttpCloseHandle(request)) operationsOk = false;
                if (!WinHttpCloseHandle(connection)) operationsOk = false;
            }
        }
        if (!WinHttpCloseHandle(session)) operationsOk = false;
    }
    Sleep(500);
    GetProcessHandleCount(GetCurrentProcess(), &after);
    PrintHandleTypeDelta(scenario, typesBefore, SnapshotHandleTypes());
    std::cout << "[METRIC] native_lifetime_scenario=" << scenario << " cycles=64 handles_before=" << before
              << " handles_after=" << after << " operations_ok=" << operationsOk << '\n';
    Require(operationsOk, "native session/request lifetime diagnostic API calls succeed");
}

void RunTests() {
    Fixture fixture;
    const auto installedDigest = Sha256File(fixture.root / L"serialctl.exe");
    const Bytes bytes = Data(fixture.manifest.dump());
    const auto signature = fixture.key.Sign(bytes);
    Require(testing::VerifySignedManifest(bytes, signature, fixture.key.publicKey).version == "V1.0.8",
            "RSA-2048 SHA-256 signed manifest accepted with explicit test key");
    Reject([&] { VerifySignedManifest(bytes, signature); }, "production trust rejects ephemeral test key");
    Bytes changed = bytes; changed.back() ^= 1;
    Reject([&] { testing::VerifySignedManifest(changed, signature, fixture.key.publicKey); }, "tampered manifest rejected before JSON parsing");
    auto badSignature = signature; badSignature[0] ^= 1;
    Reject([&] { testing::VerifySignedManifest(bytes, badSignature, fixture.key.publicKey); }, "tampered signature rejected");
    Reject([&] { testing::VerifySignedManifest(bytes, Bytes(257), fixture.key.publicKey); }, "signature length strictly limited");
    Reject([&] { testing::VerifySignedManifest(bytes, Bytes(), fixture.key.publicKey); }, "missing signature rejected");
    const auto duplicateKeys = Data("{\"schema\":1,\"schema\":1}");
    Reject([&] { testing::VerifySignedManifest(duplicateKeys, fixture.key.Sign(duplicateKeys), fixture.key.publicKey); },
           "even signed JSON duplicate keys rejected");
    const auto nested = Data("{\"schema\":[[[[[[[[[[1]]]]]]]]]]}");
    Reject([&] { testing::VerifySignedManifest(nested, fixture.key.Sign(nested), fixture.key.publicKey); },
           "even signed excessive JSON nesting rejected");
    Require(CompareVersions("V1.0.10", "V1.0.9") > 0 && CompareVersions("V1.0.8", "V1.0.8") == 0,
            "versions compare numerically");
    for (const std::string invalid : {"1.0.8", "V01.0.8", "V1.0.8-beta", "V65536.0.0", "V1.0.-1"})
        Reject([&] { CompareVersions(invalid, "V1.0.8"); }, "noncanonical version rejected");

    auto result = fixture.Run();
    Require(result.status == UpdateStatus::Ready, "HTTP valid signed files download and verify as Ready");
    Require(result.stagingDirectory.parent_path() == fixture.root / L".serialctl-update" &&
            result.stagingDirectory.filename().wstring().size() == 32, "staging is isolated in install directory");
    Require(testing::VerifyStagedManifest(result.stagingDirectory, "V1.0.7", fixture.key.publicKey).files.size() == 4,
            "worker re-verification covers all signed files and VERSION");
    auto tamperedFile = fixture.payload["serialctl.exe"]; tamperedFile[0] ^= 1;
    Write(result.stagingDirectory / L"files" / L"serialctl.exe", tamperedFile);
    Reject([&] { testing::VerifyStagedManifest(result.stagingDirectory, "V1.0.7", fixture.key.publicKey); },
           "same-size post-download tampering rejected by installer SHA-256 verification");
    fixture.Cleanup(result);
    result = fixture.Run();
    Write(result.stagingDirectory / L"files" / L"unlisted.dll", Data("must not execute"));
    Reject([&] { testing::VerifyStagedManifest(result.stagingDirectory, "V1.0.7", fixture.key.publicKey); },
           "unlisted staged payload rejected by final verification");
    fixture.Cleanup(result);

    unsigned before = fixture.server.Requests();
    fs::remove(fixture.root / L"update.ini");
    Require(fixture.Run().status == UpdateStatus::Disabled && fixture.server.Requests() == before,
            "missing config disables update without contacting network");
    fixture.Configure("");
    Require(fixture.Run().status == UpdateStatus::Disabled && fixture.server.Requests() == before,
            "blank config disables update without contacting network");
    fixture.Configure(fixture.server.Url());
    Require(fixture.Run("V1.0.8").status == UpdateStatus::NoUpdate, "equal version downloads no payload");
    Require(fixture.Run("V1.1.0").status == UpdateStatus::NoUpdate, "older signed version cannot downgrade");
    Require(fixture.server.Requests() == before + 4, "same/older version only requests signed metadata");
    Write(fixture.root / L".serialctl-update" / L"failed-version.txt", Data("V1.0.8\r\n"));
    Require(fixture.Run().status == UpdateStatus::NoUpdate, "previously failed version is quarantined");
    fs::remove(fixture.root / L".serialctl-update" / L"failed-version.txt");

    fixture.Publish(fixture.manifest, "/relocated.json");
    fixture.Configure(fixture.server.Url("/relocated.json"));
    result = fixture.Run();
    Require(result.status == UpdateStatus::Ready, "editing update.ini URL takes effect on next check");
    fixture.Cleanup(result);
    fixture.Configure(fixture.server.Url());

    auto signedFailure = [&](Json invalid, const char* message) {
        fixture.Publish(invalid);
        const unsigned start = fixture.server.Requests();
        Require(fixture.Run().status == UpdateStatus::Failed && fixture.server.Requests() == start + 2, message);
    };
    Json invalid = fixture.manifest;
    invalid["files"][0]["name"] = "../serialctl.exe";
    signedFailure(invalid, "signed path traversal rejected before payload downloads");
    for (const char* forbidden : {"update.ini", "evil.dll", "drivers.exe", "commands.txt", "serialctl.EXE"}) {
        invalid = fixture.manifest; invalid["files"][0]["name"] = forbidden;
        signedFailure(invalid, "non-allowlisted destination rejected before payload downloads");
    }
    for (const char* unsafe : {"http://attacker/a", "//attacker/a", "files/../a", "files/%2e%2e/a", "files/a?x=1"}) {
        invalid = fixture.manifest; invalid["files"][0]["url"] = unsafe;
        signedFailure(invalid, "cross-origin or ambiguous relative URL rejected");
    }
    invalid = fixture.manifest; invalid["files"][0]["size"] = kMaxUpdateFileBytes + 1;
    signedFailure(invalid, "signed oversized file rejected before downloads");
    invalid = fixture.manifest; for (auto& file : invalid["files"]) file["size"] = kMaxUpdateFileBytes;
    signedFailure(invalid, "signed total size limit enforced");
    invalid = fixture.manifest; invalid["files"].push_back(invalid["files"][0]);
    signedFailure(invalid, "duplicate destinations rejected");
    invalid = fixture.manifest; invalid["architecture"] = "x86";
    signedFailure(invalid, "wrong architecture rejected");
    invalid = fixture.manifest; invalid["files"].erase(0);
    signedFailure(invalid, "missing required runtime file rejected");
    invalid = fixture.manifest; invalid["product"] = "AnotherProduct";
    signedFailure(invalid, "wrong product rejected");
    invalid = fixture.manifest; invalid["schema"] = 2;
    signedFailure(invalid, "unsupported schema rejected");
    invalid = fixture.manifest; invalid["extra"] = true;
    signedFailure(invalid, "unknown root field rejected");
    invalid = fixture.manifest; invalid["files"][0]["extra"] = true;
    signedFailure(invalid, "unknown file field rejected");
    invalid = fixture.manifest; invalid["files"][0]["size"] = -1;
    signedFailure(invalid, "negative payload length rejected");
    invalid = fixture.manifest; invalid["files"][0]["size"] = 1.5;
    signedFailure(invalid, "fractional payload length rejected");
    invalid = fixture.manifest; invalid["files"][0]["sha256"] = std::string(64, 'G');
    signedFailure(invalid, "nonhex SHA-256 rejected");
    invalid = fixture.manifest; invalid["files"][0]["size"] = 0;
    signedFailure(invalid, "empty runtime file rejected");
    fixture.Publish(fixture.manifest);

    fixture.server.Set("/update.json.sig", {badSignature});
    Require(fixture.Run().status == UpdateStatus::Failed, "HTTP tampered signature fails closed");
    fixture.Publish(fixture.manifest);
    fixture.server.Set("/files/serialctl.exe", {tamperedFile});
    Require(fixture.Run().status == UpdateStatus::Failed, "HTTP payload corruption fails closed");
    Require(fs::is_empty(fixture.root / L".serialctl-update"), "failed staging is removed without touching installed files");
    fixture.server.Set("/files/serialctl.exe", {fixture.payload["serialctl.exe"], 200, 20});
    Require(fixture.Run().status == UpdateStatus::Failed, "declared payload size exceeding signed size is rejected");
    fixture.server.Set("/files/serialctl.exe", {fixture.payload["serialctl.exe"], 200, 0, 0, false, 0, 0, 1});
    Require(fixture.Run().status == UpdateStatus::Failed,
            "early socket close with correct signed Content-Length rejects a truncated HTTP body");
    fixture.server.Set("/files/serialctl.exe", {fixture.payload["serialctl.exe"]});
    fixture.server.Set("/update.json", {Bytes(kMaxManifestBytes + 1, 'x')});
    Require(fixture.Run().status == UpdateStatus::Failed, "declared oversized metadata rejected");
    fixture.server.Set("/update.json", {Bytes(kMaxManifestBytes + 1, 'x'), 200, 0, 0, true});
    Require(fixture.Run().status == UpdateStatus::Failed, "chunked oversized metadata rejected by actual-byte cap");
    fixture.server.Set("/update.json", {Data("redirect"), 302});
    before = fixture.server.Requests();
    Require(fixture.Run().status == UpdateStatus::Failed && fixture.server.Requests() == before + 1,
            "HTTP redirect rejected without following it");
    fixture.server.Set("/update.json", {Data("<html>please log in</html>")});
    Require(fixture.Run().status == UpdateStatus::Failed, "HTTP login HTML cannot become trusted metadata");
    fixture.server.Set("/update.json", {Data("login"), 401});
    before = fixture.server.Requests();
    Require(fixture.Run().status == UpdateStatus::Failed && fixture.server.Requests() == before + 1,
            "HTTP authentication challenge rejected without credential retry");
    fixture.server.Set("/update.json", {Data("missing"), 404});
    Require(fixture.Run().status == UpdateStatus::Failed, "HTTP missing manifest reports failure");
    fixture.Publish(fixture.manifest);

    // Exercise multi-buffer download/hash with every byte value, not just text.
    Bytes large(4 * 1024 * 1024);
    for (size_t index = 0; index < large.size(); ++index) large[index] = static_cast<unsigned char>(index);
    const auto largePath = fixture.root / L"large-fixture.bin";
    Write(largePath, large);
    Json largeManifest = fixture.manifest;
    for (auto& file : largeManifest["files"]) if (file["name"] == "serialctl.exe") {
        file["size"] = large.size(); file["sha256"] = Sha256File(largePath);
    }
    fixture.server.Set("/files/serialctl.exe", {large});
    fixture.Publish(largeManifest);
    result = fixture.Run();
    Require(result.status == UpdateStatus::Ready &&
            Sha256File(result.stagingDirectory / L"files" / L"serialctl.exe") == Sha256File(largePath),
            "multi-megabyte binary payload preserves all byte values");
    fixture.Cleanup(result);
    fixture.server.Set("/files/serialctl.exe", {fixture.payload["serialctl.exe"]});
    fixture.Publish(fixture.manifest);
    fs::remove(largePath);

    Json wrongVersion = fixture.manifest;
    const Bytes wrongVersionBytes = Data("PROJECT_VERSION=V1.0.9\r\nCPP_VERSION=V1.0.9\r\n");
    const auto wrongVersionPath = fixture.root / L"wrong-version-fixture.bin";
    Write(wrongVersionPath, wrongVersionBytes);
    for (auto& file : wrongVersion["files"]) if (file["name"] == "VERSION") {
        file["size"] = wrongVersionBytes.size(); file["sha256"] = Sha256File(wrongVersionPath);
    }
    fixture.Publish(wrongVersion);
    fixture.server.Set("/files/VERSION", {wrongVersionBytes});
    Require(fixture.Run().status == UpdateStatus::Failed, "validly signed VERSION content must match signed product version");
    fixture.server.Set("/files/VERSION", {fixture.payload["VERSION"]});
    fixture.Publish(fixture.manifest);
    fs::remove(wrongVersionPath);

    fixture.Configure("http://127.0.0.1:1/update.json");
    Require(fixture.Run().status == UpdateStatus::Failed, "unreachable HTTP server preserves current program");
    fixture.Configure("file:///C:/update.json");
    Require(fixture.Run().status == UpdateStatus::Failed, "unsupported configured URL scheme rejected");
    for (const char* invalidUrl : {"http://user:password@127.0.0.1/update.json", "http://127.0.0.1/update.json?token=a",
                                   "http://127.0.0.1/update.json#fragment", "http://127.0.0.1/"}) {
        fixture.Configure(invalidUrl);
        Require(fixture.Run().status == UpdateStatus::Failed, "unsafe configured manifest URL rejected");
    }
    fixture.Configure(fixture.server.Url());
    fixture.cancel = true;
    before = fixture.server.Requests();
    Require(fixture.Run().status == UpdateStatus::Cancelled && fixture.server.Requests() == before,
            "pre-cancelled work makes no HTTP request");
    fixture.cancel = false;
    fixture.server.Set("/update.json", {bytes, 200, 0, 3000});
    before = fixture.server.Requests();
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&] { result = fixture.Run(); });
    for (unsigned i = 0; i < 200 && fixture.server.Requests() == before; ++i) Sleep(10);
    fixture.cancel = true;
    worker.join();
    Require(result.status == UpdateStatus::Cancelled && std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "cancel during stalled HTTP returns within one second");
    Require(Sha256File(fixture.root / L"serialctl.exe") == installedDigest,
            "original runtime SHA-256 is unchanged after all downloads and failures");
    {
        Fixture body;
        body.server.Set("/update.json", {Data(body.manifest.dump()), 200, 0, 0, false, 0, 3000});
        UpdateResult pendingResult;
        const auto headers = body.server.HeadersSent();
        std::thread pending([&] { pendingResult = body.Run(); });
        for (unsigned i = 0; i < 1000 && body.server.HeadersSent() == headers; ++i) Sleep(2);
        Sleep(50); // Headers are delivered; body remains pending in WinHttpReadData.
        const auto cancelStart = std::chrono::steady_clock::now();
        body.cancel = true;
        pending.join();
        Require(pendingResult.status == UpdateStatus::Cancelled &&
                std::chrono::steady_clock::now() - cancelStart < std::chrono::seconds(1),
                "cancel interrupts a pending body read without freeing its callback buffer early");
    }
    {
        Fixture cancelledCheck;
        Fixture successfulCheck;
        cancelledCheck.server.Set("/update.json",
            {Data(cancelledCheck.manifest.dump()), 200, 0, 0, false, 0, 3000});
        successfulCheck.server.Set("/files/serialctl.exe",
            {successfulCheck.payload.at("serialctl.exe"), 200, 0, 0, false, 0, 500});
        const auto expectedDigest = Sha256File(successfulCheck.root / L"serialctl.exe");
        UpdateResult firstResult, secondResult;
        std::atomic<bool> secondFinished{false};
        std::thread first, second;
        // Also covers failure to create the second thread or a later assertion:
        // every live HTTP worker is cancelled and joined before either fixture dies.
        struct JoinWorkers {
            Fixture& firstFixture; Fixture& secondFixture;
            std::thread& first; std::thread& second;
            ~JoinWorkers() {
                firstFixture.cancel = true;
                secondFixture.cancel = true;
                if (first.joinable()) first.join();
                if (second.joinable()) second.join();
            }
        } joined{cancelledCheck, successfulCheck, first, second};
        first = std::thread([&] { firstResult = cancelledCheck.Run(); });
        for (unsigned wait = 0; wait < 1000 && !cancelledCheck.server.HeadersSent(); ++wait) Sleep(2);
        const bool firstHadHeaders = cancelledCheck.server.HeadersSent() != 0;
        Sleep(50); // The first request is now waiting for its deliberately stalled body.
        second = std::thread([&] {
            secondResult = successfulCheck.Run();
            secondFinished = true;
        });
        // Two metadata requests plus the four signed files; the last serialctl
        // body is deliberately held so cancellation overlaps actual payload I/O.
        for (unsigned wait = 0; wait < 1000 && successfulCheck.server.Requests() < 6 &&
             !secondFinished.load(); ++wait) Sleep(2);
        const bool overlappedPayload = successfulCheck.server.Requests() >= 6 && !secondFinished.load();
        cancelledCheck.cancel = true;
        first.join();
        second.join();
        Require(firstHadHeaders && overlappedPayload,
                "two independent HTTP fixtures overlap a stalled body and signed payload download");
        Require(firstResult.status == UpdateStatus::Cancelled && secondResult.status == UpdateStatus::Ready,
                "cancelling one shared-session request does not cancel another update check");
        Require(Sha256File(secondResult.stagingDirectory / L"files" / L"serialctl.exe") == expectedDigest &&
                Sha256File(successfulCheck.root / L"serialctl.exe") == expectedDigest,
                "concurrent successful update preserves staged bytes and the installed runtime");
        successfulCheck.Cleanup(secondResult);
    }
    {
        Fixture slow;
        slow.server.Set("/update.json", {Data(slow.manifest.dump()), 200, 0, 0, false, 400});
        const auto slowStart = std::chrono::steady_clock::now();
        const auto slowResult = slow.Run();
        const auto duration = std::chrono::steady_clock::now() - slowStart;
        Require(slowResult.status == UpdateStatus::Failed &&
                duration < std::chrono::seconds(18),
                "continuous slow header bytes remain bounded by the metadata deadline or stricter native timeout");
        std::cout << "[METRIC] slow_header_ms=" <<
            std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << '\n';
    }
    {
        ProbeNativeHttpLifetime("native-open-close-only", 0);
        ProbeNativeHttpLifetime("native-open-timeouts-close", 1);
        ProbeNativeHttpLifetime("native-open-timeouts-request-close", 2);
        Fixture repeated;
        repeated.Configure(repeated.server.Url("/missing.json"));
        DWORD completedBefore = 0, completedAfter = 0;
        const bool handleTracing = BeginHandleTracing();
        GetProcessHandleCount(GetCurrentProcess(), &completedBefore);
        std::vector<ULONG_PTR> eventsBefore, eventsAfter;
        const auto completedTypesBefore = SnapshotHandleTypes(&eventsBefore);
        const auto completedLifetimeBefore = testing::GetHttpLifetimeCounts();
        bool all404Failed = true;
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            const auto completedResult = repeated.Run();
            all404Failed = all404Failed && completedResult.status == UpdateStatus::Failed;
        }
        Sleep(500);
        GetProcessHandleCount(GetCurrentProcess(), &completedAfter);
        const auto completedTypesAfter = SnapshotHandleTypes(&eventsAfter);
        PrintHandleTypeDelta("completed-404-requests", completedTypesBefore, completedTypesAfter);
        if (handleTracing) {
            PrintNewEventCreationTraces(eventsBefore, eventsAfter);
            EndHandleTracing();
        }
        const auto completedLifetimeAfter = testing::GetHttpLifetimeCounts();
        std::cout << "[METRIC] completed_404_cycles=64 handles_before=" << completedBefore
                  << " handles_after=" << completedAfter
                  << " states_created_before=" << completedLifetimeBefore.created
                  << " states_created_after=" << completedLifetimeAfter.created
                  << " states_destroyed_before=" << completedLifetimeBefore.destroyed
                  << " states_destroyed_after=" << completedLifetimeAfter.destroyed
                  << " closing_before=" << completedLifetimeBefore.closingCallbacks
                  << " closing_after=" << completedLifetimeAfter.closingCallbacks << '\n';
        Require(all404Failed, "64 completed HTTP 404 requests are rejected");
        repeated.Configure(repeated.server.Url());
        repeated.server.Set("/update.json", {Data(repeated.manifest.dump()), 200, 0, 20});
        DWORD handlesBefore = 0, handlesAfter = 0;
        Sleep(500); // Let preceding asynchronous HANDLE_CLOSING callbacks settle.
        if (!GetProcessHandleCount(GetCurrentProcess(), &handlesBefore)) throw std::runtime_error("Cannot count handles");
        const DWORD threadsBefore = ThreadCount();
        const auto lifetimeBefore = testing::GetHttpLifetimeCounts();
        const auto handleTypesBefore = SnapshotHandleTypes();
        bool allCancelled = true;
        long long maximumCancelMs = 0;
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            repeated.cancel = false;
            const auto requests = repeated.server.Requests();
            UpdateResult cancelled;
            std::thread pending([&] { cancelled = repeated.Run(); });
            for (unsigned wait = 0; wait < 1000 && repeated.server.Requests() == requests; ++wait) Sleep(2);
            const auto cancelStart = std::chrono::steady_clock::now();
            repeated.cancel = true;
            pending.join();
            maximumCancelMs = (std::max)(maximumCancelMs,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancelStart).count());
            allCancelled = allCancelled && cancelled.status == UpdateStatus::Cancelled;
        }
        Sleep(500);
        if (!GetProcessHandleCount(GetCurrentProcess(), &handlesAfter)) throw std::runtime_error("Cannot count handles");
        const DWORD threadsAfter = ThreadCount();
        const auto lifetimeAfter = testing::GetHttpLifetimeCounts();
        const auto handleTypesAfter = SnapshotHandleTypes();
        PrintHandleTypeDelta("new-worker-per-cycle", handleTypesBefore, handleTypesAfter);
        std::cout << "[METRIC] cancellation_cycles=64 handles_before=" << handlesBefore << " handles_after=" << handlesAfter
                  << " threads_before=" << threadsBefore << " threads_after=" << threadsAfter
                  << " maximum_cancel_ms=" << maximumCancelMs << '\n';
        std::cout << "[METRIC] http_states_created_before=" << lifetimeBefore.created
                  << " http_states_created_after=" << lifetimeAfter.created
                  << " http_states_destroyed_before=" << lifetimeBefore.destroyed
                  << " http_states_destroyed_after=" << lifetimeAfter.destroyed
                  << " http_closing_callbacks_before=" << lifetimeBefore.closingCallbacks
                  << " http_closing_callbacks_after=" << lifetimeAfter.closingCallbacks << '\n';
        // Keep the failing original checks below this control, so one CI log
        // distinguishes resources tied to each WinHTTP session from resources
        // tied to the lifetime of a newly created calling thread.
        std::atomic<unsigned> command{0}, completed{0};
        std::atomic<bool> workerReady{false}, releaseWorker{false};
        UpdateResult sameWorkerResult;
        std::thread longLived([&] {
            workerReady = true;
            for (unsigned cycle = 1; cycle <= 64; ++cycle) {
                while (command.load() < cycle) Sleep(1);
                sameWorkerResult = repeated.Run();
                completed = cycle;
            }
            while (!releaseWorker.load()) Sleep(1);
        });
        while (!workerReady.load()) Sleep(1);
        DWORD sameHandlesBefore = 0, sameHandlesAfter = 0, sameHandlesAfterJoin = 0;
        GetProcessHandleCount(GetCurrentProcess(), &sameHandlesBefore);
        const DWORD sameThreadsBefore = ThreadCount();
        const auto sameTypesBefore = SnapshotHandleTypes();
        const auto sameLifetimeBefore = testing::GetHttpLifetimeCounts();
        bool sameAllCancelled = true;
        long long sameMaximumCancelMs = 0;
        for (unsigned cycle = 1; cycle <= 64; ++cycle) {
            repeated.cancel = false;
            const auto requests = repeated.server.Requests();
            command = cycle;
            for (unsigned wait = 0; wait < 1000 && repeated.server.Requests() == requests; ++wait) Sleep(2);
            const auto cancelStart = std::chrono::steady_clock::now();
            repeated.cancel = true;
            while (completed.load() < cycle) Sleep(1);
            sameMaximumCancelMs = (std::max)(sameMaximumCancelMs,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancelStart).count());
            sameAllCancelled = sameAllCancelled && sameWorkerResult.status == UpdateStatus::Cancelled;
        }
        Sleep(500);
        GetProcessHandleCount(GetCurrentProcess(), &sameHandlesAfter);
        const DWORD sameThreadsAfter = ThreadCount();
        const auto sameTypesAfter = SnapshotHandleTypes();
        const auto sameLifetimeAfter = testing::GetHttpLifetimeCounts();
        releaseWorker = true;
        longLived.join();
        Sleep(500);
        GetProcessHandleCount(GetCurrentProcess(), &sameHandlesAfterJoin);
        const auto sameTypesAfterJoin = SnapshotHandleTypes();
        PrintHandleTypeDelta("same-worker-64-cycles", sameTypesBefore, sameTypesAfter);
        PrintHandleTypeDelta("same-worker-after-join", sameTypesBefore, sameTypesAfterJoin);
        std::cout << "[METRIC] same_worker_cancellation_cycles=64 handles_before=" << sameHandlesBefore
                  << " handles_after=" << sameHandlesAfter << " handles_after_join=" << sameHandlesAfterJoin
                  << " threads_before=" << sameThreadsBefore << " threads_after=" << sameThreadsAfter
                  << " maximum_cancel_ms=" << sameMaximumCancelMs
                  << " states_created_before=" << sameLifetimeBefore.created
                  << " states_created_after=" << sameLifetimeAfter.created
                  << " states_destroyed_before=" << sameLifetimeBefore.destroyed
                  << " states_destroyed_after=" << sameLifetimeAfter.destroyed
                  << " closing_before=" << sameLifetimeBefore.closingCallbacks
                  << " closing_after=" << sameLifetimeAfter.closingCallbacks << '\n';
        Require(sameLifetimeAfter.created == sameLifetimeAfter.destroyed &&
                sameLifetimeAfter.closingCallbacks == sameLifetimeAfter.created,
                "same worker releases every HTTP callback state and event");
        Require(sameAllCancelled && sameMaximumCancelMs < 1000,
                "64 cancellations on the same worker finish promptly");
        Require(sameHandlesAfter <= sameHandlesBefore + 12 && sameThreadsAfter <= sameThreadsBefore + 4,
                "same-worker cancellation cycles show no unbounded handle/thread growth");
        Require(lifetimeAfter.created == lifetimeAfter.destroyed &&
                lifetimeAfter.closingCallbacks == lifetimeAfter.created,
                "all HTTP callback states and buffers are released after HANDLE_CLOSING");
        Require(allCancelled && maximumCancelMs < 1000, "64 active HTTP cancellations finish promptly");
        Require(handlesAfter <= handlesBefore + 12 && threadsAfter <= threadsBefore + 4,
                "64 cancellation cycles show no unbounded handle/thread growth (allows native pool variation)");
    }
}
} // namespace

int main(int argc, char **argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return 1;
    int result = 0;
    try {
        if (argc == 5 && std::string(argv[1]) == "--verify-archive")
            VerifyRealArchive(fs::path(argv[2]), fs::path(argv[3]), fs::path(argv[4]));
        else {
            RunArchiveTests();
            if (argc < 2 || std::string(argv[1]) != "--archive-only")
                RunTests();
        }
    } catch (const std::exception &error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        result = 1;
    }
    WSACleanup();
    return result;
}
