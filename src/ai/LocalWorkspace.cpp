#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "LocalWorkspace.h"
#include "Workspace.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <deque>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace serialctl::ai {
namespace {

constexpr size_t kMaxReadFileBytes = 64 * 1024;
constexpr size_t kMaxWriteFileBytes = 512 * 1024;
constexpr size_t kMaxAgentsMdBytes = 64 * 1024;
constexpr size_t kMaxSearchFileBytes = 1024 * 1024;
constexpr size_t kMaxListEntries = 500;
constexpr size_t kMaxSearchMatches = 200;
constexpr size_t kMaxExecOutputBytes = 64 * 1024;

bool Utf8ToWideStrict(const std::string &utf8, std::wstring &out) {
    out.clear();
    if (utf8.empty())
        return true;
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return false;
    out.resize(static_cast<size_t>(needed));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                               static_cast<int>(utf8.size()), out.data(), needed) == needed;
}

std::string WideToUtf8(const std::wstring &wide) {
    if (wide.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

bool IsValidUtf8String(const std::string &bytes) {
    if (bytes.empty())
        return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                               static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

std::string SanitizeToValidUtf8(const std::string &bytes, UINT fallbackCodePage) {
    if (bytes.empty() || IsValidUtf8String(bytes))
        return bytes;
    const int wideLen = MultiByteToWideChar(fallbackCodePage, 0, bytes.data(),
                                            static_cast<int>(bytes.size()), nullptr, 0);
    if (wideLen > 0) {
        std::wstring wide(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(fallbackCodePage, 0, bytes.data(),
                            static_cast<int>(bytes.size()), wide.data(), wideLen);
        std::string utf8 = WideToUtf8(wide);
        if (!utf8.empty() && IsValidUtf8String(utf8))
            return utf8;
    }
    std::string clean;
    clean.reserve(bytes.size());
    for (unsigned char ch : bytes) {
        if (ch < 0x80)
            clean.push_back(static_cast<char>(ch));
        else
            clean.append("\xEF\xBF\xBD");
    }
    return clean;
}

std::string Win32ErrorUtf8(DWORD code = GetLastError()) {
    LPWSTR buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg;
    if (len && buf) {
        msg.assign(buf, len);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
            msg.pop_back();
    }
    if (buf)
        LocalFree(buf);
    if (msg.empty())
        return "Win32 error " + std::to_string(code);
    return WideToUtf8(msg);
}

std::wstring NormalizeWideSlashes(std::wstring path) {
    for (wchar_t &ch : path) {
        if (ch == L'/')
            ch = L'\\';
    }
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    if (path.size() >= 2 && path[1] == L':' && path[0] >= L'a' && path[0] <= L'z')
        path[0] = static_cast<wchar_t>(towupper(path[0]));
    if (path.size() == 2 && path[1] == L':')
        path.push_back(L'\\');
    return path;
}

bool StripDosDevicePrefix(std::wstring &path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return false; // Reject UNC targets strictly
    if (path.rfind(L"\\\\?\\", 0) == 0)
        path.erase(0, 4);
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
        return false;
    path = NormalizeWideSlashes(std::move(path));
    return path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
}

bool SamePathInsensitive(const std::wstring &a, const std::wstring &b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool IsSameOrChildCanonicalPath(const std::wstring &canonicalRoot, const std::wstring &candidate) {
    if (SamePathInsensitive(canonicalRoot, candidate))
        return true;
    std::wstring prefix = canonicalRoot;
    if (!prefix.empty() && prefix.back() != L'\\')
        prefix.push_back(L'\\');
    if (candidate.size() < prefix.size())
        return false;
    return CompareStringOrdinal(candidate.c_str(), static_cast<int>(prefix.size()),
                                prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::string MakeRelativeUtf8(const std::wstring &canonicalRoot, const std::wstring &fullPath) {
    std::wstring prefix = canonicalRoot;
    if (!prefix.empty() && prefix.back() != L'\\')
        prefix.push_back(L'\\');
    std::wstring rel;
    if (SamePathInsensitive(canonicalRoot, fullPath)) {
        return ".";
    } else if (fullPath.size() > prefix.size() &&
               CompareStringOrdinal(fullPath.c_str(), static_cast<int>(prefix.size()),
                                    prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
        rel = fullPath.substr(prefix.size());
    } else {
        rel = fullPath;
    }
    for (wchar_t &ch : rel) {
        if (ch == L'\\')
            ch = L'/';
    }
    return WideToUtf8(rel);
}

bool IsReservedDosSegment(const std::wstring &segment) {
    if (segment.empty())
        return false;
    const auto dot = segment.find(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? segment : segment.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CLOCK$")
        return true;
    if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) &&
        stem[3] >= L'1' && stem[3] <= L'9')
        return true;
    return false;
}

bool FinalPathFromHandle(HANDLE handle, std::wstring &out, std::string &errorOut) {
    DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        errorOut = "Unable to resolve canonical path: " + Win32ErrorUtf8();
        return false;
    }
    std::wstring buf(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, buf.data(), needed + 1,
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= buf.size()) {
        errorOut = "Unable to read canonical path: " + Win32ErrorUtf8();
        return false;
    }
    buf.resize(written);
    if (!StripDosDevicePrefix(buf)) {
        errorOut = "UNC or non-local volume paths are forbidden in local workspace";
        return false;
    }
    out = std::move(buf);
    return true;
}

// Validates and resolves relative/child path strictly inside canonicalRoot.
// Blocks UNC paths, outside drives, "..", reserved DOS names, ADS ':', and reparse points below root.
bool ResolvePathInsideRoot(const std::wstring &canonicalRoot, const std::string &rawPath,
                           bool allowEmptyAsRoot, std::wstring &resolvedOut, std::string &errorOut) {
    resolvedOut.clear();
    if (rawPath.empty() || rawPath == ".") {
        if (!allowEmptyAsRoot) {
            errorOut = "Path must not be empty or root '.' for this operation";
            return false;
        }
        resolvedOut = canonicalRoot;
        return true;
    }
    if (rawPath.size() > 2048 ||
        std::any_of(rawPath.begin(), rawPath.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        errorOut = "Invalid path length or control characters";
        return false;
    }

    std::wstring wideInput;
    if (!Utf8ToWideStrict(rawPath, wideInput)) {
        errorOut = "Path must be valid UTF-8";
        return false;
    }
    for (wchar_t &ch : wideInput) {
        if (ch == L'/')
            ch = L'\\';
    }
    if (wideInput.rfind(L"\\\\", 0) == 0) {
        errorOut = "UNC and device paths are forbidden";
        return false;
    }

    std::wstring relPart;
    const bool hasDrivePrefix = wideInput.size() >= 3 &&
                                ((wideInput[0] >= L'A' && wideInput[0] <= L'Z') ||
                                 (wideInput[0] >= L'a' && wideInput[0] <= L'z')) &&
                                wideInput[1] == L':' && wideInput[2] == L'\\';
    if (hasDrivePrefix) {
        const std::wstring normAbs = NormalizeWideSlashes(wideInput);
        if (!IsSameOrChildCanonicalPath(canonicalRoot, normAbs)) {
            errorOut = "Path escapes active local workspace root";
            return false;
        }
        if (SamePathInsensitive(canonicalRoot, normAbs)) {
            if (!allowEmptyAsRoot) {
                errorOut = "Cannot target workspace root directory for this operation";
                return false;
            }
            resolvedOut = canonicalRoot;
            return true;
        }
        std::wstring prefix = canonicalRoot;
        if (prefix.back() != L'\\')
            prefix.push_back(L'\\');
        relPart = normAbs.substr(prefix.size());
    } else {
        if (wideInput.front() == L'\\' || wideInput.find(L':') != std::wstring::npos) {
            errorOut = "Path must be relative to the local workspace or inside its root";
            return false;
        }
        relPart = wideInput;
    }

    // Parse and validate every segment strictly.
    std::vector<std::wstring> segments;
    size_t pos = 0;
    while (pos < relPart.size()) {
        while (pos < relPart.size() && relPart[pos] == L'\\')
            ++pos;
        if (pos >= relPart.size())
            break;
        size_t next = relPart.find(L'\\', pos);
        if (next == std::wstring::npos)
            next = relPart.size();
        const std::wstring seg = relPart.substr(pos, next - pos);
        if (seg == L"." && segments.empty() && next == relPart.size()) {
            if (!allowEmptyAsRoot) {
                errorOut = "Path must target an entry inside the local workspace";
                return false;
            }
            resolvedOut = canonicalRoot;
            return true;
        }
        if (seg == L"." || seg == L"..") {
            errorOut = "Parent or dot path traversal is forbidden";
            return false;
        }
        for (wchar_t c : seg) {
            if (c == L':' || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') {
                errorOut = "Path contains forbidden characters or alternate data stream syntax";
                return false;
            }
        }
        if (seg.back() == L' ' || seg.back() == L'.' || IsReservedDosSegment(seg)) {
            errorOut = "Forbidden DOS reserved filename or trailing space/dot";
            return false;
        }
        segments.push_back(seg);
        pos = next;
    }

    if (segments.empty()) {
        if (!allowEmptyAsRoot) {
            errorOut = "Target cannot be workspace root directory";
            return false;
        }
        resolvedOut = canonicalRoot;
        return true;
    }

    // Walk component-by-component below canonicalRoot, verifying zero reparse points (symlinks/junctions).
    std::wstring current = canonicalRoot;
    std::wstring deepestExisting = canonicalRoot;
    std::vector<std::wstring> missingSuffix;
    bool hitMissing = false;
    for (const auto &seg : segments) {
        if (current.back() != L'\\')
            current.push_back(L'\\');
        current += seg;
        if (!hitMissing) {
            const DWORD attrs = GetFileAttributesW(current.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD err = GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    errorOut = "Unable to inspect path: " + Win32ErrorUtf8(err);
                    return false;
                }
                hitMissing = true;
                missingSuffix.push_back(seg);
            } else {
                if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
                    errorOut = "Symlinks and NTFS junctions inside local workspace are blocked by sandbox";
                    return false;
                }
                deepestExisting = current;
            }
        } else {
            missingSuffix.push_back(seg);
        }
    }

    // Verify canonical final path of the deepest existing path is still inside canonicalRoot.
    HANDLE hCheck = CreateFileW(deepestExisting.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (hCheck == INVALID_HANDLE_VALUE) {
        errorOut = "Unable to open path for canonical verification: " + Win32ErrorUtf8();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL gotInfo = GetFileInformationByHandle(hCheck, &info);
    std::wstring canonicalExisting;
    const bool okFinal = FinalPathFromHandle(hCheck, canonicalExisting, errorOut);
    CloseHandle(hCheck);
    if (!okFinal || !gotInfo || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (errorOut.empty())
            errorOut = "Reparse point blocked by sandbox";
        return false;
    }
    if (!IsSameOrChildCanonicalPath(canonicalRoot, canonicalExisting)) {
        errorOut = "Resolved target escapes local workspace sandbox";
        return false;
    }

    std::wstring result = canonicalExisting;
    for (const auto &seg : missingSuffix) {
        if (result.back() != L'\\')
            result.push_back(L'\\');
        result += seg;
    }
    resolvedOut = std::move(result);
    return true;
}

bool CreateDirectoriesInsideSandbox(const std::wstring &canonicalRoot, const std::wstring &targetDir,
                                    std::string &errorOut) {
    if (!IsSameOrChildCanonicalPath(canonicalRoot, targetDir)) {
        errorOut = "Directory target outside sandbox";
        return false;
    }
    if (SamePathInsensitive(canonicalRoot, targetDir))
        return true;

    std::wstring prefix = canonicalRoot;
    if (prefix.back() != L'\\')
        prefix.push_back(L'\\');
    if (targetDir.size() <= prefix.size())
        return true;

    std::wstring rel = targetDir.substr(prefix.size());
    std::wstring current = canonicalRoot;
    size_t pos = 0;
    while (pos < rel.size()) {
        size_t next = rel.find(L'\\', pos);
        if (next == std::wstring::npos)
            next = rel.size();
        const std::wstring seg = rel.substr(pos, next - pos);
        if (!seg.empty()) {
            if (current.back() != L'\\')
                current.push_back(L'\\');
            current += seg;
            const DWORD attrs = GetFileAttributesW(current.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(current.c_str(), nullptr)) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS) {
                        errorOut = "Failed to create directory: " + Win32ErrorUtf8(err);
                        return false;
                    }
                }
            } else {
                if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    errorOut = "Path component is a symlink/junction or existing file";
                    return false;
                }
            }
        }
        pos = next + 1;
    }
    return true;
}

bool WildcardMatchInsensitive(const std::wstring &pattern, const std::wstring &text) {
    if (pattern.empty())
        return true;
    size_t p = 0, t = 0, starIdx = std::wstring::npos, matchIdx = 0;
    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == L'?' || towlower(pattern[p]) == towlower(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starIdx = p++;
            matchIdx = t;
        } else if (starIdx != std::wstring::npos) {
            p = starIdx + 1;
            t = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*')
        ++p;
    return p == pattern.size();
}

std::string FileTimeToIsoUtc(const FILETIME &ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st))
        return {};
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool ReadFileBoundedBytes(const std::wstring &path, size_t maxBytes, std::string &bytesOut,
                          bool &truncatedOut, std::string &errorOut, const std::wstring *workspaceRoot = nullptr) {
    bytesOut.clear();
    truncatedOut = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = "Unable to open file: " + Win32ErrorUtf8();
        return false;
    }
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } closer{h};

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || !GetFileSizeEx(h, &size)) {
        errorOut = "Unable to stat file: " + Win32ErrorUtf8();
        return false;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        errorOut = "Symlink/reparse file blocked by sandbox";
        return false;
    }
    if (workspaceRoot) {
        std::wstring canonical;
        if (!FinalPathFromHandle(h, canonical, errorOut) || !IsSameOrChildCanonicalPath(*workspaceRoot, canonical)) {
            errorOut = "File is outside the active workspace";
            return false;
        }
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        errorOut = "Target is a directory, not a file";
        return false;
    }
    if (size.QuadPart < 0) {
        errorOut = "Invalid negative file size";
        return false;
    }

    const size_t toRead = static_cast<size_t>((std::min<uint64_t>)(static_cast<uint64_t>(size.QuadPart), maxBytes));
    if (static_cast<uint64_t>(size.QuadPart) > maxBytes)
        truncatedOut = true;
    bytesOut.resize(toRead);
    size_t offset = 0;
    while (offset < toRead) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>((std::min<size_t>)(toRead - offset, 65536));
        if (!ReadFile(h, bytesOut.data() + offset, want, &chunk, nullptr)) {
            errorOut = "Read error: " + Win32ErrorUtf8();
            return false;
        }
        if (chunk == 0)
            break;
        offset += chunk;
    }
    bytesOut.resize(offset);
    return true;
}

bool AtomicWriteFileBytes(const std::wstring &targetPath, const std::string &bytes, std::string &errorOut) {
    const auto slash = targetPath.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        errorOut = "Invalid target path for atomic write";
        return false;
    }
    const std::wstring dir = targetPath.substr(0, slash);

    // Reject if target itself already exists as directory or reparse point.
    const DWORD existingAttrs = GetFileAttributesW(targetPath.c_str());
    if (existingAttrs != INVALID_FILE_ATTRIBUTES) {
        if (existingAttrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            errorOut = "Symlink target rejected by sandbox";
            return false;
        }
        if (existingAttrs & FILE_ATTRIBUTE_DIRECTORY) {
            errorOut = "Target is an existing directory";
            return false;
        }
    }

    static std::atomic<uint64_t> seq{1};
    std::wstring tempPath;
    HANDLE hTemp = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const uint64_t id = seq.fetch_add(1, std::memory_order_relaxed);
        tempPath = dir + L"\\.serialctl-tmp-" + std::to_wstring(GetCurrentProcessId()) +
                   L"-" + std::to_wstring(id);
        hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTemp != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            errorOut = "Unable to create temp staging file: " + Win32ErrorUtf8();
            return false;
        }
    }
    if (hTemp == INVALID_HANDLE_VALUE) {
        errorOut = "Unable to allocate unique staging filename";
        return false;
    }

    bool writeOk = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes.size() - offset, 65536));
        if (!WriteFile(hTemp, bytes.data() + offset, want, &written, nullptr) || written != want) {
            errorOut = "Write failed: " + Win32ErrorUtf8();
            writeOk = false;
            break;
        }
        offset += written;
    }
    if (writeOk)
        FlushFileBuffers(hTemp);
    CloseHandle(hTemp);

    if (!writeOk) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        errorOut = "Atomic rename failed: " + Win32ErrorUtf8();
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool ContainsNulByte(const std::string &bytes) {
    return bytes.find('\0') != std::string::npos;
}

bool ShouldSkipAutoRecurseDir(const std::wstring &name, bool includeHidden, DWORD attrs) {
    if (!includeHidden && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
        return true;
    if (name == L".git" || name == L"node_modules" || name == L".vs" || name == L"__pycache__")
        return !includeHidden;
    return false;
}

bool DeleteDirectoryRecursiveSafe(const std::wstring &dirPath, size_t &deletedCount, std::string &errorOut) {
    if (deletedCount >= 2000) {
        errorOut = "Recursive delete safety limit (2000 items) reached";
        return false;
    }
    const DWORD dirAttrs = GetFileAttributesW(dirPath.c_str());
    if (dirAttrs == INVALID_FILE_ATTRIBUTES) {
        errorOut = "Target does not exist: " + Win32ErrorUtf8();
        return false;
    }
    if (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        errorOut = "Refusing to traverse reparse point during recursive delete";
        return false;
    }

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((dirPath + L"\\*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_FILES) {
            errorOut = "Unable to enumerate directory for delete: " + Win32ErrorUtf8(err);
            return false;
        }
    } else {
        struct CloseFind {
            HANDLE h;
            ~CloseFind() { FindClose(h); }
        } closer{hFind};
        do {
            const std::wstring name(ffd.cFileName);
            if (name == L"." || name == L"..")
                continue;
            const std::wstring child = dirPath + L"\\" + name;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                errorOut = "Symlink/junction encountered during delete; aborted for safety";
                return false;
            }
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!DeleteDirectoryRecursiveSafe(child, deletedCount, errorOut))
                    return false;
            } else {
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(child.c_str(), ffd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                if (!DeleteFileW(child.c_str())) {
                    errorOut = "Failed to delete file: " + Win32ErrorUtf8();
                    return false;
                }
                ++deletedCount;
                if (deletedCount >= 2000) {
                    errorOut = "Recursive delete safety limit (2000 items) reached";
                    return false;
                }
            }
        } while (FindNextFileW(hFind, &ffd));
    }

    if (!RemoveDirectoryW(dirPath.c_str())) {
        errorOut = "Failed to remove directory: " + Win32ErrorUtf8();
        return false;
    }
    ++deletedCount;
    return true;
}

std::wstring QuoteWin32Arg(const std::wstring &arg) {
    if (arg.empty())
        return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

void DrainPipeNonBlocking(HANDLE pipe, std::string &buf, size_t maxBytes, bool &truncated,
                          std::string *streamAppend = nullptr, size_t maxStreamPending = 256 * 1024) {
    DWORD avail = 0;
    std::array<char, 4096> tmp{};
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        DWORD readBytes = 0;
        const DWORD want = static_cast<DWORD>((std::min<DWORD>)(avail, static_cast<DWORD>(tmp.size())));
        if (!ReadFile(pipe, tmp.data(), want, &readBytes, nullptr) || readBytes == 0)
            break;
        if (streamAppend && streamAppend->size() < maxStreamPending) {
            const size_t streamTake = (std::min<size_t>)(readBytes, maxStreamPending - streamAppend->size());
            streamAppend->append(tmp.data(), streamTake);
        }
        if (buf.size() < maxBytes) {
            const size_t take = (std::min<size_t>)(readBytes, maxBytes - buf.size());
            buf.append(tmp.data(), take);
            if (take < readBytes)
                truncated = true;
        } else {
            truncated = true;
        }
    }
}

// Extracts complete UTF-8 or DBCS sequences from pending console bytes unless flushAll is true.
std::string FlushConsoleChunkUtf8(std::string &pending, UINT consoleCp, bool flushAll) {
    if (pending.empty())
        return {};
    size_t completeEnd = pending.size();
    if (!flushAll && !IsValidUtf8String(pending)) {
        size_t utfCut = pending.size();
        for (size_t back = 1; back <= 3 && back <= pending.size(); ++back) {
            const size_t cand = pending.size() - back;
            if (cand > 0 && IsValidUtf8String(pending.substr(0, cand))) {
                utfCut = cand;
                break;
            }
        }
        if (utfCut < pending.size()) {
            completeEnd = utfCut;
        } else {
            const BYTE lead0 = static_cast<BYTE>(pending[0]);
            const size_t expectedUtf8 = (lead0 >= 0xF0) ? 4 : (lead0 >= 0xE0) ? 3 : (lead0 >= 0xC2) ? 2 : 0;
            if (expectedUtf8 > pending.size() && consoleCp == CP_UTF8) {
                completeEnd = 0;
            } else {
                size_t pos = 0;
                while (pos < pending.size()) {
                    const BYTE b = static_cast<BYTE>(pending[pos]);
                    if (b < 0x80 || !IsDBCSLeadByteEx(consoleCp, b)) {
                        ++pos;
                    } else if (pos + 1 < pending.size()) {
                        pos += 2;
                    } else {
                        break;
                    }
                }
                completeEnd = pos;
            }
        }
    }
    if (completeEnd == 0)
        return {};
    std::string chunk = pending.substr(0, completeEnd);
    pending.erase(0, completeEnd);
    return SanitizeToValidUtf8(chunk, consoleCp);
}

Json ErrorJson(const std::string &code, const std::string &message) {
    return {{"ok", false}, {"error", message}, {"code", code}};
}

struct CommandFile {
    WIN32_FIND_DATAW metadata{};
    std::string content;
    bool hasContent = false;
};

struct CommandFileSnapshot {
    std::map<std::string, CommandFile> files;
    bool incomplete = false;
};

CommandFileSnapshot CaptureCommandFiles(const std::wstring &root) {
    CommandFileSnapshot result;
    const auto started = GetTickCount64();
    size_t bytesRemaining = 16 * 1024 * 1024;
    size_t entries = 0;
    std::vector<std::wstring> directories{root};
    while (!directories.empty()) {
        auto dir = std::move(directories.back());
        directories.pop_back();
        std::wstring verified;
        std::string pathError;
        if (!ResolvePathInsideRoot(root, MakeRelativeUtf8(root, dir), true, verified, pathError)) {
            result.incomplete = true;
            continue;
        }
        WIN32_FIND_DATAW data{};
        if (dir.back() != L'\\')
            dir += L'\\';
        const HANDLE search = FindFirstFileW((dir + L"*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) {
            result.incomplete |= GetLastError() != ERROR_FILE_NOT_FOUND;
            continue;
        }
        do {
            if (++entries > 20000 || GetTickCount64() - started > 2000) {
                result.incomplete = true;
                FindClose(search);
                return result;
            }
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L".." || _wcsicmp(name.c_str(), L".git") == 0 ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            const auto path = dir + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                directories.push_back(path);
                continue;
            }
            CommandFile file;
            file.metadata = data;
            if (!data.nFileSizeHigh && data.nFileSizeLow <= 64 * 1024 && data.nFileSizeLow <= bytesRemaining) {
                bool truncated = false;
                std::string error;
                file.hasContent = ReadFileBoundedBytes(path, data.nFileSizeLow, file.content, truncated, error, &root) && !truncated;
                bytesRemaining -= data.nFileSizeLow;
                if (!file.hasContent)
                    file.content.clear();
            }
            result.files.emplace(MakeRelativeUtf8(root, path), std::move(file));
        } while (FindNextFileW(search, &data));
        result.incomplete |= GetLastError() != ERROR_NO_MORE_FILES;
        FindClose(search);
    }
    return result;
}

bool SameCommandFile(const CommandFile &left, const CommandFile &right) {
    if (left.hasContent && right.hasContent)
        return left.content == right.content;
    return left.metadata.nFileSizeHigh == right.metadata.nFileSizeHigh &&
           left.metadata.nFileSizeLow == right.metadata.nFileSizeLow &&
           CompareFileTime(&left.metadata.ftLastWriteTime, &right.metadata.ftLastWriteTime) == 0;
}

bool CommandFileText(const CommandFile *file, std::string &text) {
    if (!file)
        return true;
    if (!file->hasContent)
        return false;
    const auto &bytes = file->content;
    const bool littleEndian = bytes.compare(0, 2, "\xFF\xFE") == 0;
    const bool bigEndian = bytes.compare(0, 2, "\xFE\xFF") == 0;
    if (littleEndian || bigEndian) {
        if (bytes.size() % 2)
            return false;
        std::wstring wide;
        wide.reserve(bytes.size() / 2 - 1);
        for (size_t i = 2; i < bytes.size(); i += 2) {
            const auto first = static_cast<unsigned char>(bytes[i]);
            const auto second = static_cast<unsigned char>(bytes[i + 1]);
            wide.push_back(static_cast<wchar_t>(littleEndian ? first | (second << 8) : (first << 8) | second));
        }
        text = WideToUtf8(wide);
        return !ContainsNulByte(text);
    }
    if (ContainsNulByte(bytes) || !IsValidUtf8String(bytes))
        return false;
    text = bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? bytes.substr(3) : bytes;
    return true;
}

} // namespace

bool ResolveAndValidateLocalRoot(const std::wstring &root, std::wstring &canonicalOut, std::string &errorOut) {
    canonicalOut.clear();
    if (root.empty()) {
        errorOut = "Local workspace root path is empty";
        return false;
    }
    const std::string utf8 = WideToUtf8(root);
    if (!ValidLocalWorkspacePath(utf8)) {
        errorOut = "Local workspace path must be an absolute Windows drive path without UNC or '..' traversal";
        return false;
    }

    const std::wstring normalized = NormalizeWideSlashes(root);
    const DWORD attrs = GetFileAttributesW(normalized.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        errorOut = "Local workspace folder does not exist: " + Win32ErrorUtf8();
        return false;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        errorOut = "Local workspace target is not a directory";
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        errorOut = "Local workspace root directory cannot be a symlink or NTFS junction";
        return false;
    }

    HANDLE h = CreateFileW(normalized.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        errorOut = "Unable to open local workspace directory: " + Win32ErrorUtf8();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL gotInfo = GetFileInformationByHandle(h, &info);
    std::wstring finalPath;
    const bool ok = FinalPathFromHandle(h, finalPath, errorOut);
    CloseHandle(h);
    if (!ok || !gotInfo || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (errorOut.empty())
            errorOut = "Invalid local workspace root directory attributes";
        return false;
    }
    canonicalOut = std::move(finalPath);
    return true;
}

std::vector<std::wstring> EnumerateLocalDrives() {
    std::vector<std::wstring> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        std::wstring root(1, static_cast<wchar_t>(L'A' + i));
        root += L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK || type == DRIVE_REMOTE) {
            drives.push_back(root);
        }
    }
    return drives;
}

std::vector<std::wstring> EnumerateLocalSubdirectories(const std::wstring &dir, size_t maxEntries) {
    std::vector<std::wstring> result;
    if (dir.empty()) {
        auto drives = EnumerateLocalDrives();
        if (drives.size() > maxEntries)
            drives.resize(maxEntries);
        return drives;
    }
    std::wstring canonical;
    std::string err;
    if (!ResolveAndValidateLocalRoot(dir, canonical, err))
        return result;

    std::wstring prefix = canonical;
    if (prefix.back() != L'\\')
        prefix.push_back(L'\\');
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((prefix + L"*").c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return result;
    struct CloseFind {
        HANDLE h;
        ~CloseFind() { FindClose(h); }
    } closer{hFind};

    std::set<std::wstring> sorted;
    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (ffd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM))
            continue;
        const std::wstring name(ffd.cFileName);
        if (name == L"." || name == L"..")
            continue;
        sorted.insert(prefix + name);
        if (sorted.size() >= maxEntries)
            break;
    } while (FindNextFileW(hFind, &ffd));

    result.assign(sorted.begin(), sorted.end());
    return result;
}

bool ReadLocalAgentsMd(const std::wstring &root, bool &existsOut, std::string &contentOut, std::string &errorOut) {
    existsOut = false;
    contentOut.clear();
    errorOut.clear();
    std::wstring canonicalRoot;
    if (!ResolveAndValidateLocalRoot(root, canonicalRoot, errorOut))
        return false;

    std::wstring agentsPath;
    if (!ResolvePathInsideRoot(canonicalRoot, "AGENTS.md", false, agentsPath, errorOut))
        return false;

    const DWORD attrs = GetFileAttributesW(agentsPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return true;
        errorOut = "Failed to query AGENTS.md: " + Win32ErrorUtf8(err);
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        errorOut = "AGENTS.md symlink rejected";
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        errorOut = "AGENTS.md is a directory";
        return false;
    }

    HANDLE h = CreateFileW(agentsPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            return true;
        errorOut = "Unable to open AGENTS.md: " + Win32ErrorUtf8(err);
        return false;
    }
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } closer{h};

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || !GetFileSizeEx(h, &size)) {
        errorOut = "Unable to inspect AGENTS.md";
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        errorOut = "AGENTS.md symlink or directory rejected";
        return false;
    }
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxAgentsMdBytes) {
        errorOut = "AGENTS.md exceeds 64 KiB managed size limit";
        return false;
    }

    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < raw.size()) {
        DWORD readBytes = 0;
        const DWORD want = static_cast<DWORD>((std::min<size_t>)(raw.size() - offset, 65536));
        if (!ReadFile(h, raw.data() + offset, want, &readBytes, nullptr)) {
            errorOut = "Unable to read AGENTS.md: " + Win32ErrorUtf8();
            return false;
        }
        if (readBytes == 0)
            break;
        offset += readBytes;
    }
    raw.resize(offset);
    existsOut = true;
    contentOut = std::move(raw);
    return true;
}

bool ReplaceLocalAgentsMd(const std::wstring &root, const std::string &expected, bool expectedExists,
                          const std::string &replacement, bool replacementExists, std::string &errorOut) {
    errorOut.clear();
    std::wstring canonicalRoot;
    if (!ResolveAndValidateLocalRoot(root, canonicalRoot, errorOut))
        return false;

    std::wstring agentsPath;
    if (!ResolvePathInsideRoot(canonicalRoot, "AGENTS.md", false, agentsPath, errorOut))
        return false;

    bool actualExists = false;
    std::string actualContent;
    if (!ReadLocalAgentsMd(canonicalRoot, actualExists, actualContent, errorOut))
        return false;

    if (expectedExists) {
        if (!actualExists || actualContent != expected) {
            errorOut = "AGENTS.md changed concurrently; reload required";
            return false;
        }
    } else if (actualExists) {
        errorOut = "AGENTS.md created concurrently; reload required";
        return false;
    }

    if (!replacementExists) {
        if (!actualExists)
            return true;
        if (!DeleteFileW(agentsPath.c_str())) {
            errorOut = "Failed to delete AGENTS.md: " + Win32ErrorUtf8();
            return false;
        }
        return true;
    }

    if (replacement.size() > kMaxAgentsMdBytes) {
        errorOut = "AGENTS.md replacement exceeds 64 KiB managed limit";
        return false;
    }
    return AtomicWriteFileBytes(agentsPath, replacement, errorOut);
}

LocalWorkspaceService::LocalWorkspaceService()
    : execAbortEpoch_(std::make_shared<std::atomic<bool>>(false)) {}

LocalWorkspaceService::~LocalWorkspaceService() {
    CancelAllExecs();
}

bool LocalWorkspaceService::Configure(bool active, const std::wstring &root, bool readOnly, std::string &errorOut) {
    errorOut.clear();
    if (!active) {
        CancelAllExecs();
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.active = false;
        snapshot_.readOnly = readOnly;
        snapshot_.root = NormalizeWideSlashes(root);
        snapshot_.canonicalRoot.clear();
        return true;
    }

    std::wstring canonical;
    if (!ResolveAndValidateLocalRoot(root, canonical, errorOut))
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const bool rootChanged = !SamePathInsensitive(snapshot_.canonicalRoot, canonical);
    if (rootChanged && execAbortEpoch_)
        execAbortEpoch_->store(true);
    if (rootChanged)
        execAbortEpoch_ = std::make_shared<std::atomic<bool>>(false);
    snapshot_.active = true;
    snapshot_.readOnly = readOnly;
    snapshot_.root = NormalizeWideSlashes(root);
    snapshot_.canonicalRoot = std::move(canonical);
    return true;
}

LocalWorkspaceSnapshot LocalWorkspaceService::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void LocalWorkspaceService::CancelAllExecs() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (execAbortEpoch_)
        execAbortEpoch_->store(true);
    execAbortEpoch_ = std::make_shared<std::atomic<bool>>(false);
}

std::uint64_t LocalWorkspaceService::AddObserver(ExecObserver onExec, FileChangeObserver onFileChange) {
    const std::uint64_t token = nextObserverId_.fetch_add(1);
    std::lock_guard<std::mutex> lock(observerMutex_);
    observers_.push_back({token, std::move(onExec), std::move(onFileChange)});
    return token;
}

void LocalWorkspaceService::RemoveObserver(std::uint64_t token) {
    if (!token)
        return;
    std::lock_guard<std::mutex> lock(observerMutex_);
    observers_.erase(std::remove_if(observers_.begin(), observers_.end(),
                                    [token](const ObserverEntry &entry) { return entry.id == token; }),
                     observers_.end());
}

void LocalWorkspaceService::EmitExecEvent(const ExecProgressEvent &event) const {
    std::vector<ExecObserver> callbacks;
    {
        std::lock_guard<std::mutex> lock(observerMutex_);
        callbacks.reserve(observers_.size());
        for (const auto &entry : observers_) {
            if (entry.onExec)
                callbacks.push_back(entry.onExec);
        }
    }
    for (const auto &cb : callbacks)
        cb(event);
}

void LocalWorkspaceService::EmitFileChange(const FileChangeRecord &change) const {
    std::vector<FileChangeObserver> callbacks;
    {
        std::lock_guard<std::mutex> lock(observerMutex_);
        callbacks.reserve(observers_.size());
        for (const auto &entry : observers_) {
            if (entry.onFileChange)
                callbacks.push_back(entry.onFileChange);
        }
    }
    for (const auto &cb : callbacks)
        cb(change);
}

Json LocalWorkspaceService::ExecuteTool(const std::string &toolName, const Json &arguments,
                                        const std::atomic<bool> *externalCancel) {
    const LocalWorkspaceSnapshot snap = Snapshot();
    if (toolName == "workspace.info")
        return ToolInfo(snap);

    if (!snap.active || snap.canonicalRoot.empty()) {
        return ErrorJson("LOCAL_WORKSPACE_NOT_ACTIVE",
                         "No Windows local workspace is active in SerialCtl. Switch workspace mode to Local and select a folder first.");
    }

    const Json args = arguments.is_object() ? arguments : Json::object();
    if (toolName == "workspace.list")
        return ToolList(snap, args);
    if (toolName == "workspace.search_text")
        return ToolSearchText(snap, args, externalCancel);
    if (toolName == "workspace.read_file")
        return ToolReadFile(snap, args);

    if (snap.readOnly && (toolName == "workspace.write_file" || toolName == "workspace.edit_file" ||
                          toolName == "workspace.move" || toolName == "workspace.delete" ||
                          toolName == "workspace.exec")) {
        return ErrorJson("LOCAL_WORKSPACE_READ_ONLY",
                         "The active Windows local workspace is configured as read-only.");
    }

    if (toolName == "workspace.write_file")
        return ToolWriteFile(snap, args);
    if (toolName == "workspace.edit_file")
        return ToolEditFile(snap, args);
    if (toolName == "workspace.move")
        return ToolMove(snap, args);
    if (toolName == "workspace.delete")
        return ToolDelete(snap, args);
    if (toolName == "workspace.exec")
        return ToolExec(snap, args, externalCancel);

    return ErrorJson("UNKNOWN_WORKSPACE_TOOL", "Unknown workspace tool: " + toolName);
}

Json LocalWorkspaceService::ToolInfo(const LocalWorkspaceSnapshot &snap) const {
    return {
        {"ok", true},
        {"active", snap.active},
        {"readOnly", snap.readOnly},
        {"localRoot", WideToUtf8(snap.canonicalRoot.empty() ? snap.root : snap.canonicalRoot)},
        {"gitRequired", false},
        {"builtinTools", {"workspace.info", "workspace.list", "workspace.search_text",
                          "workspace.read_file", "workspace.write_file", "workspace.edit_file",
                          "workspace.move", "workspace.delete", "workspace.exec"}}
    };
}

Json LocalWorkspaceService::ToolList(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const std::string reqPath = args.value("path", std::string("."));
    int depth = args.value("depth", 1);
    if (depth < 1)
        depth = 1;
    if (depth > 3)
        depth = 3;
    const std::string patternUtf8 = args.value("pattern", std::string());
    const bool includeHidden = args.value("includeHidden", false);

    std::wstring widePattern;
    if (!patternUtf8.empty() && !Utf8ToWideStrict(patternUtf8, widePattern))
        return ErrorJson("INVALID_PARAMS", "Pattern must be valid UTF-8");

    std::wstring baseDir;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, true, baseDir, err))
        return ErrorJson("INVALID_PATH", err);

    const DWORD baseAttrs = GetFileAttributesW(baseDir.c_str());
    if (baseAttrs == INVALID_FILE_ATTRIBUTES)
        return ErrorJson("NOT_FOUND", "Directory does not exist: " + Win32ErrorUtf8());
    if (!(baseAttrs & FILE_ATTRIBUTE_DIRECTORY))
        return ErrorJson("NOT_DIRECTORY", "Target path is not a directory");

    struct QueueItem {
        std::wstring dir;
        int depth;
    };
    std::deque<QueueItem> queue;
    queue.push_back({baseDir, 1});

    Json entries = Json::array();
    bool truncated = false;
    while (!queue.empty() && entries.size() < kMaxListEntries) {
        const auto item = queue.front();
        queue.pop_front();

        std::wstring prefix = item.dir;
        if (prefix.back() != L'\\')
            prefix.push_back(L'\\');
        WIN32_FIND_DATAW ffd{};
        HANDLE hFind = FindFirstFileW((prefix + L"*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;
        struct CloseFind {
            HANDLE h;
            ~CloseFind() { FindClose(h); }
        } closer{hFind};

        do {
            const std::wstring name(ffd.cFileName);
            if (name == L"." || name == L"..")
                continue;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue; // Never traverse or expose junctions/symlinks outside root
            const bool isDir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (ShouldSkipAutoRecurseDir(name, includeHidden, ffd.dwFileAttributes))
                continue;

            const std::wstring full = prefix + name;
            const bool matchesPattern = widePattern.empty() || WildcardMatchInsensitive(widePattern, name);
            if (matchesPattern) {
                if (entries.size() >= kMaxListEntries) {
                    truncated = true;
                    break;
                }
                const uint64_t size = isDir ? 0ull
                    : (static_cast<uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
                entries.push_back({
                    {"path", MakeRelativeUtf8(snap.canonicalRoot, full)},
                    {"name", WideToUtf8(name)},
                    {"type", isDir ? "dir" : "file"},
                    {"size", size},
                    {"modifiedUtc", FileTimeToIsoUtc(ffd.ftLastWriteTime)}
                });
            }
            if (isDir && item.depth < depth)
                queue.push_back({full, item.depth + 1});
        } while (FindNextFileW(hFind, &ffd));
    }
    if (!queue.empty())
        truncated = true;

    return {
        {"ok", true},
        {"root", WideToUtf8(snap.canonicalRoot)},
        {"path", MakeRelativeUtf8(snap.canonicalRoot, baseDir)},
        {"entries", entries},
        {"truncated", truncated}
    };
}

Json LocalWorkspaceService::ToolSearchText(const LocalWorkspaceSnapshot &snap, const Json &args,
                                           const std::atomic<bool> *externalCancel) const {
    const std::string query = args.value("query", std::string());
    if (query.empty() || query.size() > 512)
        return ErrorJson("INVALID_PARAMS", "Search query must be 1..512 bytes");
    const std::string reqPath = args.value("path", std::string("."));
    const std::string filePatternUtf8 = args.value("filePattern", std::string());
    const bool caseSensitive = args.value("caseSensitive", false);
    const bool useRegex = args.value("regex", false);
    int maxResults = args.value("maxResults", 100);
    if (maxResults < 1)
        maxResults = 1;
    if (maxResults > static_cast<int>(kMaxSearchMatches))
        maxResults = static_cast<int>(kMaxSearchMatches);

    std::wstring widePattern;
    if (!filePatternUtf8.empty() && !Utf8ToWideStrict(filePatternUtf8, widePattern))
        return ErrorJson("INVALID_PARAMS", "filePattern must be valid UTF-8");

    std::regex compiledRegex;
    if (useRegex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!caseSensitive)
                flags = static_cast<std::regex::flag_type>(flags | std::regex::icase);
            compiledRegex.assign(query, flags);
        } catch (const std::regex_error &e) {
            return ErrorJson("INVALID_REGEX", std::string("Invalid regular expression: ") + e.what());
        }
    }

    std::wstring startPath;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, true, startPath, err))
        return ErrorJson("INVALID_PATH", err);

    const DWORD startAttrs = GetFileAttributesW(startPath.c_str());
    if (startAttrs == INVALID_FILE_ATTRIBUTES)
        return ErrorJson("NOT_FOUND", "Search root path does not exist: " + Win32ErrorUtf8());

    std::string lowerQuery;
    if (!useRegex && !caseSensitive) {
        lowerQuery = query;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    Json matches = Json::array();
    size_t searchedFiles = 0;
    bool truncated = false;
    bool timedOut = false;
    const auto startedAt = std::chrono::steady_clock::now();

    auto searchSingleFile = [&](const std::wstring &filePath) {
        if (matches.size() >= static_cast<size_t>(maxResults) || searchedFiles >= 4000) {
            truncated = true;
            return;
        }
        std::string rawBytes;
        bool readTrunc = false;
        std::string readErr;
        if (!ReadFileBoundedBytes(filePath, kMaxSearchFileBytes, rawBytes, readTrunc, readErr))
            return;
        if (ContainsNulByte(rawBytes))
            return; // Skip binary files silently
        ++searchedFiles;

        // Normalize legacy codepage files cleanly to UTF-8 for matching & JSON safety.
        const std::string textUtf8 = SanitizeToValidUtf8(rawBytes, CP_ACP);
        const std::string relPath = MakeRelativeUtf8(snap.canonicalRoot, filePath);

        size_t pos = 0;
        size_t lineNo = 1;
        while (pos < textUtf8.size() && matches.size() < static_cast<size_t>(maxResults)) {
            size_t end = textUtf8.find('\n', pos);
            if (end == std::string::npos)
                end = textUtf8.size();
            size_t len = end - pos;
            if (len > 0 && textUtf8[pos + len - 1] == '\r')
                --len;
            const std::string lineStr = textUtf8.substr(pos, len);

            size_t matchCol = std::string::npos;
            if (useRegex) {
                if (lineStr.size() <= 4096) {
                    std::smatch sm;
                    if (std::regex_search(lineStr, sm, compiledRegex))
                        matchCol = static_cast<size_t>(sm.position()) + 1;
                }
            } else if (caseSensitive) {
                const auto hit = lineStr.find(query);
                if (hit != std::string::npos)
                    matchCol = hit + 1;
            } else {
                std::string lowerLine = lineStr;
                std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const auto hit = lowerLine.find(lowerQuery);
                if (hit != std::string::npos)
                    matchCol = hit + 1;
            }

            if (matchCol != std::string::npos) {
                std::string preview = lineStr.size() > 300 ? lineStr.substr(0, 300) + "..." : lineStr;
                preview = SanitizeToValidUtf8(preview, CP_UTF8);
                matches.push_back({
                    {"path", relPath},
                    {"line", lineNo},
                    {"column", matchCol},
                    {"text", std::move(preview)}
                });
            }

            pos = (end < textUtf8.size()) ? end + 1 : end;
            ++lineNo;
        }
    };

    if (!(startAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        searchSingleFile(startPath);
    } else {
        std::deque<std::wstring> dirs;
        dirs.push_back(startPath);
        while (!dirs.empty() && matches.size() < static_cast<size_t>(maxResults)) {
            if ((externalCancel && externalCancel->load()) ||
                std::chrono::steady_clock::now() - startedAt > std::chrono::milliseconds(1500)) {
                timedOut = true;
                truncated = true;
                break;
            }
            const std::wstring dir = dirs.front();
            dirs.pop_front();
            std::wstring prefix = dir;
            if (prefix.back() != L'\\')
                prefix.push_back(L'\\');

            WIN32_FIND_DATAW ffd{};
            HANDLE hFind = FindFirstFileW((prefix + L"*").c_str(), &ffd);
            if (hFind == INVALID_HANDLE_VALUE)
                continue;
            struct CloseFind {
                HANDLE h;
                ~CloseFind() { FindClose(h); }
            } closer{hFind};

            do {
                const std::wstring name(ffd.cFileName);
                if (name == L"." || name == L"..")
                    continue;
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue;
                if (ShouldSkipAutoRecurseDir(name, false, ffd.dwFileAttributes))
                    continue;

                const std::wstring child = prefix + name;
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    dirs.push_back(child);
                } else {
                    const uint64_t fsize = (static_cast<uint64_t>(ffd.nFileSizeHigh) << 32) | ffd.nFileSizeLow;
                    if (fsize > kMaxSearchFileBytes)
                        continue;
                    if (!widePattern.empty() && !WildcardMatchInsensitive(widePattern, name))
                        continue;
                    searchSingleFile(child);
                    if (matches.size() >= static_cast<size_t>(maxResults) || searchedFiles >= 4000) {
                        truncated = true;
                        break;
                    }
                    if ((externalCancel && externalCancel->load()) ||
                        std::chrono::steady_clock::now() - startedAt > std::chrono::milliseconds(1500)) {
                        timedOut = true;
                        truncated = true;
                        break;
                    }
                }
            } while (FindNextFileW(hFind, &ffd));
        }
    }

    return {
        {"ok", true},
        {"matches", matches},
        {"searchedFiles", searchedFiles},
        {"truncated", truncated},
        {"timedOut", timedOut}
    };
}

Json LocalWorkspaceService::ToolReadFile(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const std::string reqPath = args.value("path", std::string());
    int startLine = args.value("startLine", 1);
    int maxLines = args.value("maxLines", 200);
    if (startLine < 1)
        startLine = 1;
    if (maxLines < 1)
        maxLines = 1;
    if (maxLines > 400)
        maxLines = 400;

    std::wstring target;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, false, target, err))
        return ErrorJson("INVALID_PATH", err);

    std::string rawBytes;
    bool fileTruncated = false;
    // Read slightly above 64 KiB cap? Cap total file size inspection at 1 MiB to count lines cleanly.
    if (!ReadFileBoundedBytes(target, kMaxSearchFileBytes, rawBytes, fileTruncated, err))
        return ErrorJson("READ_FAILED", err);
    if (ContainsNulByte(rawBytes))
        return ErrorJson("BINARY_FILE_REJECTED", "Target appears to be a binary file (contains NUL bytes)");

    const std::string utf8 = SanitizeToValidUtf8(rawBytes, CP_ACP);
    std::vector<std::pair<size_t, size_t>> lines;
    size_t pos = 0;
    while (pos < utf8.size()) {
        size_t end = utf8.find('\n', pos);
        if (end == std::string::npos)
            end = utf8.size();
        size_t len = end - pos;
        if (len > 0 && utf8[pos + len - 1] == '\r')
            --len;
        lines.emplace_back(pos, len);
        pos = (end < utf8.size()) ? end + 1 : end;
    }

    const size_t totalLines = lines.size();
    const size_t firstIdx = static_cast<size_t>(startLine - 1);
    std::string out;
    size_t endLine = (firstIdx >= totalLines) ? totalLines : firstIdx;
    bool outputTruncated = fileTruncated;

    for (size_t i = firstIdx; i < totalLines && (i - firstIdx) < static_cast<size_t>(maxLines); ++i) {
        std::string line = utf8.substr(lines[i].first, lines[i].second);
        if (!out.empty())
            out.push_back('\n');
        if (out.size() + line.size() > kMaxReadFileBytes) {
            outputTruncated = true;
            break;
        }
        out += line;
        endLine = i + 1;
    }
    if (endLine < totalLines)
        outputTruncated = true;

    return {
        {"ok", true},
        {"path", MakeRelativeUtf8(snap.canonicalRoot, target)},
        {"startLine", startLine},
        {"endLine", endLine},
        {"totalLines", totalLines},
        {"truncated", outputTruncated},
        {"content", out}
    };
}

Json LocalWorkspaceService::ToolWriteFile(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const std::string reqPath = args.value("path", std::string());
    if (!args.contains("content") || !args["content"].is_string())
        return ErrorJson("INVALID_PARAMS", "content string parameter is required");
    const std::string content = args["content"].get<std::string>();
    if (content.size() > kMaxWriteFileBytes)
        return ErrorJson("SIZE_LIMIT_EXCEEDED", "content exceeds 512 KiB single-write limit");
    if (ContainsNulByte(content))
        return ErrorJson("BINARY_CONTENT_REJECTED", "Binary NUL bytes are rejected by workspace.write_file");
    if (!IsValidUtf8String(content))
        return ErrorJson("INVALID_ENCODING", "content must be valid UTF-8");
    const bool createDirs = args.value("createDirs", true);

    std::wstring target;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, false, target, err))
        return ErrorJson("INVALID_PATH", err);

    const auto slash = target.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return ErrorJson("INVALID_PATH", "Invalid target path");
    const std::wstring parentDir = target.substr(0, slash);
    const DWORD parentAttrs = GetFileAttributesW(parentDir.c_str());
    if (parentAttrs == INVALID_FILE_ATTRIBUTES) {
        if (!createDirs)
            return ErrorJson("PARENT_NOT_FOUND", "Parent directory does not exist and createDirs=false");
        if (!CreateDirectoriesInsideSandbox(snap.canonicalRoot, parentDir, err))
            return ErrorJson("MKDIR_FAILED", err);
    } else if (!(parentAttrs & FILE_ATTRIBUTE_DIRECTORY) || (parentAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return ErrorJson("INVALID_PARENT", "Parent directory is a file or reparse point");
    }

    const bool created = GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES;
    std::string previousContent;
    if (!created) {
        bool prevTrunc = false;
        std::string prevErr;
        if (!ReadFileBoundedBytes(target, kMaxWriteFileBytes, previousContent, prevTrunc, prevErr) ||
            ContainsNulByte(previousContent) || !IsValidUtf8String(previousContent)) {
            previousContent.clear();
        }
    }
    if (!AtomicWriteFileBytes(target, content, err))
        return ErrorJson("WRITE_FAILED", err);

    const std::string relPath = MakeRelativeUtf8(snap.canonicalRoot, target);
    const std::string changeId = "ws-chg-" + std::to_string(nextChangeSeq_.fetch_add(1));
    FileChangeRecord record;
    record.changeId = changeId;
    record.path = relPath;
    record.action = created ? "create" : "edit";
    record.diffText = ComputeUnifiedDiff(relPath, previousContent, content, 48 * 1024,
                                         &record.addedLines, &record.removedLines, &record.truncated);
    record.contentPreview = content;
    if (record.contentPreview.size() > 256 * 1024)
        TruncateUtf8Safe(record.contentPreview, 256 * 1024, u8"\n[文件内容预览已截断]");
    EmitFileChange(record);

    return {
        {"ok", true},
        {"path", relPath},
        {"action", record.action},
        {"changeId", changeId},
        {"addedLines", record.addedLines},
        {"removedLines", record.removedLines},
        {"bytesWritten", content.size()},
        {"created", created}
    };
}

Json LocalWorkspaceService::ToolEditFile(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const std::string reqPath = args.value("path", std::string());
    const std::string oldText = args.value("oldText", std::string());
    if (oldText.empty())
        return ErrorJson("INVALID_PARAMS", "oldText must not be empty");
    if (!args.contains("newText") || !args["newText"].is_string())
        return ErrorJson("INVALID_PARAMS", "newText string parameter is required");
    const std::string newText = args["newText"].get<std::string>();
    const bool replaceAll = args.value("replaceAll", false);
    if (ContainsNulByte(oldText) || ContainsNulByte(newText) || !IsValidUtf8String(oldText) || !IsValidUtf8String(newText))
        return ErrorJson("INVALID_ENCODING", "oldText/newText must be valid UTF-8 text without NUL");

    std::wstring target;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, false, target, err))
        return ErrorJson("INVALID_PATH", err);

    std::string raw;
    bool truncated = false;
    if (!ReadFileBoundedBytes(target, kMaxWriteFileBytes, raw, truncated, err))
        return ErrorJson("READ_FAILED", err);
    if (truncated)
        return ErrorJson("SIZE_LIMIT_EXCEEDED", "Target file exceeds 512 KiB edit limit");
    if (ContainsNulByte(raw))
        return ErrorJson("BINARY_FILE_REJECTED", "Cannot edit binary file containing NUL bytes");

    std::string effectiveOld = oldText;
    std::string effectiveNew = newText;
    auto toCrlf = [](const std::string &in) {
        std::string out;
        out.reserve(in.size() + 16);
        for (size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '\n' && (i == 0 || in[i - 1] != '\r'))
                out.push_back('\r');
            out.push_back(in[i]);
        }
        return out;
    };
    if (raw.find(effectiveOld) == std::string::npos &&
        raw.find("\r\n") != std::string::npos && oldText.find('\n') != std::string::npos) {
        const std::string crlfOld = toCrlf(oldText);
        if (raw.find(crlfOld) != std::string::npos) {
            effectiveOld = crlfOld;
            effectiveNew = toCrlf(newText);
        }
    }

    size_t count = 0;
    size_t searchPos = 0;
    while ((searchPos = raw.find(effectiveOld, searchPos)) != std::string::npos) {
        ++count;
        searchPos += effectiveOld.size();
        if (!replaceAll && count > 1)
            break;
    }
    if (count == 0)
        return ErrorJson("MATCH_NOT_FOUND", "oldText not found in target file");
    if (!replaceAll && count > 1)
        return ErrorJson("AMBIGUOUS_MATCH", "oldText appears multiple times; provide more context or set replaceAll=true");

    std::string updated;
    updated.reserve(raw.size() + effectiveNew.size());
    size_t pos = 0;
    size_t replacements = 0;
    while (pos < raw.size()) {
        const size_t hit = raw.find(effectiveOld, pos);
        if (hit == std::string::npos) {
            updated.append(raw, pos, raw.size() - pos);
            break;
        }
        updated.append(raw, pos, hit - pos);
        updated.append(effectiveNew);
        pos = hit + effectiveOld.size();
        ++replacements;
        if (!replaceAll) {
            updated.append(raw, pos, raw.size() - pos);
            break;
        }
    }
    if (updated.size() > kMaxWriteFileBytes)
        return ErrorJson("SIZE_LIMIT_EXCEEDED", "Edited file exceeds 512 KiB limit");
    if (!AtomicWriteFileBytes(target, updated, err))
        return ErrorJson("WRITE_FAILED", err);

    const std::string relPath = MakeRelativeUtf8(snap.canonicalRoot, target);
    const std::string changeId = "ws-chg-" + std::to_string(nextChangeSeq_.fetch_add(1));
    FileChangeRecord record;
    record.changeId = changeId;
    record.path = relPath;
    record.action = "edit";
    record.diffText = ComputeUnifiedDiff(relPath, raw, updated, 48 * 1024,
                                         &record.addedLines, &record.removedLines, &record.truncated);
    record.contentPreview = updated;
    if (record.contentPreview.size() > 256 * 1024)
        TruncateUtf8Safe(record.contentPreview, 256 * 1024, u8"\n[文件内容预览已截断]");
    EmitFileChange(record);

    return {
        {"ok", true},
        {"path", relPath},
        {"action", "edit"},
        {"changeId", changeId},
        {"addedLines", record.addedLines},
        {"removedLines", record.removedLines},
        {"replacements", replacements},
        {"bytesWritten", updated.size()}
    };
}

Json LocalWorkspaceService::ToolMove(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const std::string fromPath = args.value("from", std::string());
    const std::string toPath = args.value("to", std::string());
    const bool overwrite = args.value("overwrite", false);

    std::wstring fromW, toW;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, fromPath, false, fromW, err))
        return ErrorJson("INVALID_SOURCE", err);
    if (!ResolvePathInsideRoot(snap.canonicalRoot, toPath, false, toW, err))
        return ErrorJson("INVALID_DESTINATION", err);

    const DWORD fromAttrs = GetFileAttributesW(fromW.c_str());
    if (fromAttrs == INVALID_FILE_ATTRIBUTES)
        return ErrorJson("NOT_FOUND", "Source does not exist: " + Win32ErrorUtf8());
    if (fromAttrs & FILE_ATTRIBUTE_REPARSE_POINT)
        return ErrorJson("SANDBOX_BLOCKED", "Symlinks/junctions cannot be moved");

    const auto slash = toW.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return ErrorJson("INVALID_DESTINATION", "Invalid destination parent");
    const std::wstring destParent = toW.substr(0, slash);
    if (GetFileAttributesW(destParent.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoriesInsideSandbox(snap.canonicalRoot, destParent, err))
            return ErrorJson("MKDIR_FAILED", err);
    }

    const DWORD flags = overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
    if (!MoveFileExW(fromW.c_str(), toW.c_str(), flags))
        return ErrorJson("MOVE_FAILED", "Move failed: " + Win32ErrorUtf8());

    const std::string fromRel = MakeRelativeUtf8(snap.canonicalRoot, fromW);
    const std::string toRel = MakeRelativeUtf8(snap.canonicalRoot, toW);
    const std::string changeId = "ws-chg-" + std::to_string(nextChangeSeq_.fetch_add(1));
    FileChangeRecord record;
    record.changeId = changeId;
    record.path = toRel;
    record.action = "move";
    record.detail = fromRel + " -> " + toRel;
    record.diffText = "--- a/" + fromRel + "\n+++ b/" + toRel + "\n";
    if (!(fromAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        bool trunc = false;
        std::string preview, rerr;
        if (ReadFileBoundedBytes(toW, 256 * 1024, preview, trunc, rerr) && !ContainsNulByte(preview) && IsValidUtf8String(preview))
            record.contentPreview = std::move(preview);
    }
    EmitFileChange(record);

    return {
        {"ok", true},
        {"action", "move"},
        {"changeId", changeId},
        {"from", fromRel},
        {"to", toRel}
    };
}

Json LocalWorkspaceService::ToolDelete(const LocalWorkspaceSnapshot &snap, const Json &args) const {
    const bool confirm = args.value("confirm", false);
    if (!confirm) {
        return ErrorJson("CONFIRMATION_REQUIRED",
                         "workspace.delete requires explicit 'confirm: true' parameter.");
    }
    const std::string reqPath = args.value("path", std::string());
    const bool recursive = args.value("recursive", false);

    std::wstring target;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqPath, false, target, err))
        return ErrorJson("INVALID_PATH", err);

    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return ErrorJson("NOT_FOUND", "Target does not exist");
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        return ErrorJson("SANDBOX_BLOCKED", "Symlinks/junctions blocked from deletion");

    std::string deletedPreview;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        bool trunc = false;
        std::string rerr;
        if (ReadFileBoundedBytes(target, kMaxWriteFileBytes, deletedPreview, trunc, rerr) &&
            !ContainsNulByte(deletedPreview) && IsValidUtf8String(deletedPreview)) {
            if (trunc)
                deletedPreview.clear();
        } else {
            deletedPreview.clear();
        }
    }

    size_t deletedCount = 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (recursive) {
            if (!DeleteDirectoryRecursiveSafe(target, deletedCount, err))
                return ErrorJson("DELETE_FAILED", err);
        } else {
            if (!RemoveDirectoryW(target.c_str()))
                return ErrorJson("DELETE_FAILED", "Directory delete failed (set recursive=true if non-empty): " + Win32ErrorUtf8());
            deletedCount = 1;
        }
    } else {
        if (attrs & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(target.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        if (!DeleteFileW(target.c_str()))
            return ErrorJson("DELETE_FAILED", "File delete failed: " + Win32ErrorUtf8());
        deletedCount = 1;
    }

    const std::string relPath = MakeRelativeUtf8(snap.canonicalRoot, target);
    const std::string changeId = "ws-chg-" + std::to_string(nextChangeSeq_.fetch_add(1));
    FileChangeRecord record;
    record.changeId = changeId;
    record.path = relPath;
    record.action = "delete";
    record.diffText = deletedPreview.empty()
        ? ("--- a/" + relPath + "\n+++ /dev/null\n")
        : ComputeUnifiedDiff(relPath, deletedPreview, "", 48 * 1024, &record.addedLines, &record.removedLines, &record.truncated);
    EmitFileChange(record);

    return {
        {"ok", true},
        {"action", "delete"},
        {"changeId", changeId},
        {"path", relPath},
        {"deletedItems", deletedCount}
    };
}

Json LocalWorkspaceService::ToolExec(const LocalWorkspaceSnapshot &snap, const Json &args,
                                     const std::atomic<bool> *externalCancel) {
    const std::string command = args.value("command", std::string());
    if (command.empty() || command.size() > 4096 ||
        std::any_of(command.begin(), command.end(), [](unsigned char c) { return c == '\0' || c == '\r' || c == '\n'; })) {
        return ErrorJson("INVALID_PARAMS", "command must be a single non-empty line <= 4096 bytes");
    }
    const std::string reqCwd = args.value("cwd", std::string("."));
    int timeoutMs = args.value("timeoutMs", 30000);
    if (timeoutMs < 100)
        timeoutMs = 100;
    if (timeoutMs > 120000)
        timeoutMs = 120000;

    std::wstring execCwd;
    std::string err;
    if (!ResolvePathInsideRoot(snap.canonicalRoot, reqCwd, true, execCwd, err))
        return ErrorJson("INVALID_CWD", err);
    const DWORD cwdAttrs = GetFileAttributesW(execCwd.c_str());
    if (cwdAttrs == INVALID_FILE_ATTRIBUTES || !(cwdAttrs & FILE_ATTRIBUTE_DIRECTORY))
        return ErrorJson("INVALID_CWD", "Working directory does not exist");

    std::wstring wideCommand;
    if (!Utf8ToWideStrict(command, wideCommand))
        return ErrorJson("INVALID_PARAMS", "command must be valid UTF-8");

    wchar_t comspecBuf[MAX_PATH]{};
    const DWORD comspecLen = GetEnvironmentVariableW(L"ComSpec", comspecBuf, MAX_PATH);
    const std::wstring comspec = (comspecLen > 0 && comspecLen < MAX_PATH) ? std::wstring(comspecBuf, comspecLen) : L"cmd.exe";
    std::wstring cmdLine = QuoteWin32Arg(comspec) + L" /d /s /c \"" + wideCommand + L"\"";
    return RunWorkspaceProcess(snap, execCwd, std::move(cmdLine), command, timeoutMs, externalCancel);
}

Json LocalWorkspaceService::RunWorkspaceProcess(const LocalWorkspaceSnapshot &snap, const std::wstring &execCwd,
                                                std::wstring cmdLine, const std::string &command, int timeoutMs,
                                                const std::atomic<bool> *externalCancel, const wchar_t *application) {
    std::shared_ptr<std::atomic<bool>> epochCancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        epochCancel = execAbortEpoch_;
    }
    const bool trackChanges = application == nullptr && !snap.readOnly;
    const auto before = trackChanges ? CaptureCommandFiles(snap.canonicalRoot) : CommandFileSnapshot{};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE errRead = nullptr, errWrite = nullptr;
    HANDLE nulIn = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        if (outRead) CloseHandle(outRead);
        if (outWrite) CloseHandle(outWrite);
        if (errRead) CloseHandle(errRead);
        if (errWrite) CloseHandle(errWrite);
        return ErrorJson("PIPE_FAILED", "Failed to create child pipes: " + Win32ErrorUtf8());
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    // Win7-compatible Job Object to clean up child trees reliably when supported.
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (nulIn != INVALID_HANDLE_VALUE) ? nulIn : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(application, cmdLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, execCwd.c_str(), &si, &pi);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (nulIn != INVALID_HANDLE_VALUE)
        CloseHandle(nulIn);

    if (!created) {
        const DWORD lastErr = GetLastError();
        CloseHandle(outRead);
        CloseHandle(errRead);
        if (hJob)
            CloseHandle(hJob);
        return ErrorJson("SPAWN_FAILED", "Failed to launch process: " + Win32ErrorUtf8(lastErr));
    }

    if (hJob && !AssignProcessToJobObject(hJob, pi.hProcess)) {
        CloseHandle(hJob);
        hJob = nullptr; // Fallback cleanly on nested-job restricted Win7 hosts
    }

    const std::string relCwd = MakeRelativeUtf8(snap.canonicalRoot, execCwd);
    const std::string execId = "ws-exec-" + std::to_string(nextExecSeq_.fetch_add(1));
    const UINT consoleCp = GetOEMCP();
    const ULONGLONG startedAt = GetTickCount64();

    if (!application)
        EmitExecEvent({ExecProgressEvent::Phase::Started, execId, "workspace.exec", command, relCwd, {}, 0, false, false, false, false, 0});

    std::string stdoutRaw, stderrRaw;
    std::string streamOutPending, streamErrPending;
    bool stdoutTrunc = false, stderrTrunc = false;
    bool timedOut = false, cancelled = false;
    const ULONGLONG deadline = startedAt + static_cast<ULONGLONG>(timeoutMs);
    ULONGLONG lastEmit = startedAt;

    auto flushStream = [&](bool flushAll) {
        std::string chunk = FlushConsoleChunkUtf8(streamOutPending, consoleCp, flushAll);
        const std::string errChunk = FlushConsoleChunkUtf8(streamErrPending, consoleCp, flushAll);
        if (!errChunk.empty())
            chunk += errChunk;
        if (!chunk.empty()) {
            if (!application)
                EmitExecEvent({ExecProgressEvent::Phase::OutputDelta, execId, "workspace.exec", command, relCwd,
                           std::move(chunk), 0, false, false, false, false, GetTickCount64() - startedAt});
            lastEmit = GetTickCount64();
        }
    };

    for (;;) {
        DrainPipeNonBlocking(outRead, stdoutRaw, kMaxExecOutputBytes, stdoutTrunc, &streamOutPending);
        DrainPipeNonBlocking(errRead, stderrRaw, kMaxExecOutputBytes, stderrTrunc, &streamErrPending);

        const ULONGLONG now = GetTickCount64();
        if ((!streamOutPending.empty() || !streamErrPending.empty()) &&
            (now - lastEmit >= 50 || streamOutPending.size() + streamErrPending.size() >= 2048)) {
            flushStream(false);
        }

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
            break;

        if ((externalCancel && externalCancel->load()) || (epochCancel && epochCancel->load())) {
            cancelled = true;
            if (hJob)
                TerminateJobObject(hJob, 1);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
        if (now >= deadline) {
            timedOut = true;
            if (hJob)
                TerminateJobObject(hJob, 1);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
        Sleep(15);
    }

    DrainPipeNonBlocking(outRead, stdoutRaw, kMaxExecOutputBytes, stdoutTrunc, &streamOutPending);
    DrainPipeNonBlocking(errRead, stderrRaw, kMaxExecOutputBytes, stderrTrunc, &streamErrPending);
    flushStream(true);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);
    CloseHandle(errRead);
    if (hJob)
        CloseHandle(hJob);

    const ULONGLONG durationMs = GetTickCount64() - startedAt;
    bool fileChangesIncomplete = false;
    if (trackChanges && !epochCancel->load()) {
        const auto after = CaptureCommandFiles(snap.canonicalRoot);
        size_t emitted = 0;
        bool limited = false;
        const auto report = [&](const std::string &path, const CommandFile *oldFile, const CommandFile *newFile) {
            if (epochCancel->load())
                return;
            if (emitted >= 100) {
                limited = true;
                return;
            }
            ++emitted;
            FileChangeRecord record;
            record.changeId = "ws-chg-" + std::to_string(nextChangeSeq_.fetch_add(1));
            record.path = path;
            record.action = !oldFile ? "create" : !newFile ? "delete" : "edit";
            record.detectedByCommand = true;
            std::string oldText, newText;
            if (CommandFileText(oldFile, oldText) && CommandFileText(newFile, newText)) {
                record.diffText = ComputeUnifiedDiff(path, oldText, newText, 48 * 1024,
                    &record.addedLines, &record.removedLines, &record.truncated);
            }
            if (record.diffText.empty()) {
                record.detail = u8"文件已修改，文本预览不可用";
            }
            EmitFileChange(record);
        };
        for (const auto &[path, file] : after.files) {
            const auto old = before.files.find(path);
            if (old == before.files.end()) {
                if (!before.incomplete)
                    report(path, nullptr, &file);
            } else if (!SameCommandFile(old->second, file)) {
                report(path, &old->second, &file);
            }
        }
        if (!after.incomplete) {
            for (const auto &[path, file] : before.files)
                if (after.files.count(path) == 0)
                    report(path, &file, nullptr);
        }
        fileChangesIncomplete = before.incomplete || after.incomplete || limited;
        if (fileChangesIncomplete) {
            const std::string notice = u8"\n[工作区文件变更检测达到扫描限制或无法读取部分目录，结果可能不完整]\n";
            EmitExecEvent({ExecProgressEvent::Phase::OutputDelta, execId, "workspace.exec", command, relCwd, notice});
        }
    }
    const bool failed = timedOut || cancelled || exitCode != 0;
    if (!application)
        EmitExecEvent({ExecProgressEvent::Phase::Completed, execId, "workspace.exec", command, relCwd, {},
                   static_cast<int>(exitCode), timedOut, cancelled, stdoutTrunc || stderrTrunc, failed, durationMs});

    Json result = {
        {"ok", !timedOut && !cancelled},
        {"execId", execId},
        {"exitCode", static_cast<int>(exitCode)},
        {"durationMs", durationMs},
        {"stdout", application ? std::move(stdoutRaw) : SanitizeToValidUtf8(stdoutRaw, consoleCp)},
        {"stderr", SanitizeToValidUtf8(stderrRaw, consoleCp)},
        {"timedOut", timedOut},
        {"cancelled", cancelled},
        {"truncated", stdoutTrunc || stderrTrunc},
        {"workingDirectory", relCwd}
    };
    if (fileChangesIncomplete)
        result["fileChangesIncomplete"] = true;
    return result;
}

Json LocalWorkspaceService::RunGitCommand(const LocalWorkspaceSnapshot &snap, const std::wstring &arguments) {
    const DWORD pathSize = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(pathSize, L'\0');
    if (!pathSize || !GetEnvironmentVariableW(L"PATH", path.data(), pathSize))
        return ErrorJson("GIT_NOT_FOUND", u8"未找到 Git，请检查 PATH");
    wchar_t git[MAX_PATH]{};
    const DWORD found = SearchPathW(path.c_str(), L"git.exe", nullptr, MAX_PATH, git, nullptr);
    if (!found || found >= MAX_PATH)
        return ErrorJson("GIT_NOT_FOUND", u8"未找到 Git，请检查 PATH");

    auto command = QuoteWin32Arg(git) +
        L" --no-pager --no-optional-locks -c core.fsmonitor=false -c core.untrackedCache=false " + arguments;
    return RunWorkspaceProcess(snap, snap.canonicalRoot, std::move(command), {}, 5000, nullptr, git);
}

GitFileChanges LocalWorkspaceService::ReadGitFileChanges(const LocalWorkspaceSnapshot &snap, bool history,
                                                         const std::string &previewId) {
    if (!snap.active)
        return {{}, u8"请先选择本地工作区", {}};
    const auto output = RunGitCommand(snap, history
        ? L"log -30 --encoding=UTF-8 --format=%x00%H%x09%s --name-status --no-renames --no-relative -z -- ."
        : L"status --porcelain=v1 -z --no-renames --untracked-files=all --ignore-submodules=dirty -- .");
    if (!output.value("ok", false) || output.value("exitCode", 1) != 0) {
        if (output.value("code", std::string()) == "GIT_NOT_FOUND")
            return {{}, u8"未找到 Git，请检查 PATH", {}};
        const auto error = output.value("stderr", output.value("error", std::string()));
        if (error.find("not a git repository") != std::string::npos)
            return {{}, u8"当前工作区不在 Git 仓库中", {}};
        if (history && error.find("does not have any commits yet") != std::string::npos)
            return {{}, u8"仓库暂无提交", {}};
        return {{}, u8"无法读取 Git 数据" + (error.empty() ? std::string() : "\n" + error), {}};
    }

    GitFileChanges result;
    const auto bytes = output.value("stdout", std::string());
    size_t pos = 0;
    const auto nextToken = [&]() {
        const size_t end = bytes.find('\0', pos);
        if (end == std::string::npos) {
            pos = bytes.size();
            return std::string{};
        }
        auto token = SanitizeToValidUtf8(bytes.substr(pos, end - pos), GetOEMCP());
        pos = end + 1;
        return token;
    };
    std::string commit;
    std::string subject;
    constexpr size_t maxRecords = 500;
    while (pos < bytes.size() && result.records.size() < maxRecords) {
        auto status = nextToken();
        if (status.empty())
            continue;
        if (history) {
            if (status.front() == '\n')
                status.erase(0, 1);
            const auto tab = status.find('\t');
            if (tab != std::string::npos) {
                commit = status.substr(0, tab);
                subject = status.substr(tab + 1);
                continue;
            }
            if (status.empty() || commit.empty())
                continue;
        } else if (status.size() < 4 || status[2] != ' ') {
            continue;
        }
        const auto path = history ? nextToken() : status.substr(3);
        if (path.empty())
            continue;
        const auto addRecord = [&](FileChangeRecord::GitLayer layer, char change, std::string detail) {
            if (result.records.size() >= maxRecords)
                return;
            FileChangeRecord record;
            record.path = path;
            record.gitLayer = layer;
            record.gitRevision = history ? commit : std::string{};
            record.changeId = "git:" + std::to_string(static_cast<int>(layer)) + ":" + record.gitRevision + ":" + path;
            record.action = change == 'A' || change == '?' ? "create" : change == 'D' ? "delete" : "edit";
            record.detail = std::move(detail);
            result.records.push_back(std::move(record));
        };
        using Layer = FileChangeRecord::GitLayer;
        if (history) {
            if ((commit.size() == 40 || commit.size() == 64) &&
                commit.find_first_not_of("0123456789abcdef") == std::string::npos)
                addRecord(Layer::Commit, status.front(), commit + "  " + subject + "\n" + status + "  " + path);
        } else if (status.compare(0, 2, "??") == 0) {
            addRecord(Layer::Untracked, '?', u8"未跟踪\n" + path);
        } else {
            if (status[0] != ' ')
                addRecord(Layer::Staged, status[0], u8"暂存区  " + status.substr(0, 1) + "\n" + path);
            if (status[1] != ' ')
                addRecord(Layer::Unstaged, status[1], u8"工作区  " + status.substr(1, 1) + "\n" + path);
        }
    }
    if (!result.records.empty()) {
        auto selected = std::find_if(result.records.begin(), result.records.end(), [&](const auto &record) {
            return record.changeId == previewId;
        });
        if (selected == result.records.end())
            selected = result.records.begin();
        result.previewId = selected->changeId;
        ReadGitFileDiff(snap, *selected);
    }
    std::reverse(result.records.begin(), result.records.end());
    if (pos < bytes.size() || output.value("truncated", false))
        result.notice = u8"Git 结果超过显示上限，仅显示部分文件";
    else if (result.records.empty())
        result.notice = history ? u8"暂无已提交的文件改动" : u8"Git 工作区干净";
    return result;
}

void LocalWorkspaceService::ReadGitFileDiff(const LocalWorkspaceSnapshot &snap, FileChangeRecord &record) {
    using Layer = FileChangeRecord::GitLayer;
    if (record.gitLayer == Layer::Untracked) {
        const auto prefixResult = RunGitCommand(snap, L"rev-parse --show-prefix");
        auto prefix = prefixResult.value("stdout", std::string());
        if (!prefix.empty() && prefix.back() == '\n')
            prefix.pop_back();
        if (!prefix.empty() && prefix.back() == '\r')
            prefix.pop_back();
        std::wstring path;
        std::string error;
        CommandFile file;
        if (!prefixResult.value("ok", false) || prefixResult.value("exitCode", 1) != 0 ||
            record.path.rfind(prefix, 0) != 0 ||
            !ResolvePathInsideRoot(snap.canonicalRoot, record.path.substr(prefix.size()), false, path, error) ||
            !ReadFileBoundedBytes(path, kMaxExecOutputBytes, file.content, record.truncated, error, &snap.canonicalRoot)) {
            record.detail += u8"\n无法读取未跟踪文件";
            return;
        }
        file.hasContent = !record.truncated;
        std::string text;
        if (!CommandFileText(&file, text)) {
            record.detail += record.truncated ? u8"\n文件超过文本预览上限" : u8"\n文件没有可显示的文本差异";
            return;
        }
        record.diffText = ComputeUnifiedDiff(record.path, {}, text, 48 * 1024,
            &record.addedLines, &record.removedLines, &record.truncated);
        return;
    }

    std::wstring path;
    if (!Utf8ToWideStrict(record.path, path))
        return;
    const bool history = record.gitLayer == Layer::Commit;
    std::wstring command = history ? L"show --format= " : L"diff ";
    if (record.gitLayer == Layer::Staged)
        command += L"--cached ";
    command += L"--patch --no-color --no-ext-diff --no-textconv --no-renames --no-relative --submodule=short ";
    if (history)
        command += std::wstring(record.gitRevision.begin(), record.gitRevision.end()) + L" ";
    const auto patch = RunGitCommand(snap, command + L"-- " + QuoteWin32Arg(L":(top,literal)" + path));
    if (!patch.value("ok", false) || patch.value("exitCode", 1) != 0) {
        record.detail += u8"\n无法读取 Git 补丁";
        const auto error = patch.value("stderr", patch.value("error", std::string()));
        if (!error.empty())
            record.detail += "\n" + error;
        return;
    }
    record.truncated = patch.value("truncated", false);
    if (record.truncated) {
        record.detail += u8"\n补丁达到显示上限";
    }
    auto bytes = patch.value("stdout", std::string());
    if (record.truncated) {
        const auto lineEnd = bytes.rfind('\n');
        bytes.resize(lineEnd == std::string::npos ? 0 : lineEnd + 1);
    }
    record.diffText = SanitizeToValidUtf8(bytes, GetOEMCP());
    CountDiffChanges(record.diffText, record.addedLines, record.removedLines);
}

} // namespace serialctl::ai
