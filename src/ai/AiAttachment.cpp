#include "AiAttachment.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <atomic>
#include <objidl.h>
#include <string_view>

namespace serialctl::ai {
namespace {
std::string Base64(const Bytes &bytes, size_t offset, size_t length) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((length + 2) / 3 * 4);
    for (size_t i = offset; i < offset + length; i += 3) {
        const size_t left = offset + length - i;
        const unsigned bits = (unsigned(bytes[i]) << 16) |
            (left > 1 ? unsigned(bytes[i + 1]) << 8 : 0) | (left > 2 ? bytes[i + 2] : 0);
        encoded += alphabet[(bits >> 18) & 63];
        encoded += alphabet[(bits >> 12) & 63];
        encoded += left > 1 ? alphabet[(bits >> 6) & 63] : '=';
        encoded += left > 2 ? alphabet[bits & 63] : '=';
    }
    return encoded;
}
}
Attachment ReadAttachment(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error(u8"无法读取附件");
    struct CloseFile { HANDLE file; ~CloseFile() { CloseHandle(file); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > AttachmentSizeLimit)
        throw std::runtime_error(u8"请选择文件，单个附件最大 8 MiB");
    auto bytes = std::make_shared<Bytes>(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!bytes->empty() && (!ReadFile(file, bytes->data(), static_cast<DWORD>(bytes->size()), &count, nullptr) ||
                            count != bytes->size()))
        throw std::runtime_error(u8"附件读取未完成");
    Attachment attachment;
    const auto slash = path.find_last_of(L"/\\");
    attachment.name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    attachment.bytes = bytes;
    const auto dot = attachment.name.find_last_of(L'.');
    if (dot != std::wstring::npos && attachment.name.size() - dot <= 12) {
        const auto extension = attachment.name.substr(dot + 1);
        if (!extension.empty() && std::all_of(extension.begin(), extension.end(), [](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
            }))
            attachment.suffix = "." + WideToMultiByte(extension, CP_UTF8);
    }
    const bool png = bytes->size() >= 8 && (*bytes)[0] == 137 && (*bytes)[1] == 'P' && (*bytes)[2] == 'N' && (*bytes)[3] == 'G';
    const bool jpeg = bytes->size() >= 3 && (*bytes)[0] == 0xff && (*bytes)[1] == 0xd8 && (*bytes)[2] == 0xff;
    const bool gif = bytes->size() >= 6 && (*bytes)[0] == 'G' && (*bytes)[1] == 'I' && (*bytes)[2] == 'F';
    const bool webp = bytes->size() >= 12 && memcmp(bytes->data(), "RIFF", 4) == 0 && memcmp(bytes->data() + 8, "WEBP", 4) == 0;
    const bool bmp = bytes->size() >= 14 && (*bytes)[0] == 'B' && (*bytes)[1] == 'M';
    attachment.image = png || jpeg || gif || webp || bmp;
    if (attachment.image) attachment.suffix = png ? ".png" : jpeg ? ".jpg" : gif ? ".gif" : bmp ? ".bmp" : ".webp";
    if (attachment.suffix.empty()) attachment.suffix = ".bin";
    if (attachment.image) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes->size());
        if (memory) {
            void *address = GlobalLock(memory);
            if (address) {
                memcpy(address, bytes->data(), bytes->size());
                GlobalUnlock(memory);
                IStream *stream = nullptr;
                if (SUCCEEDED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
                    {
                        Gdiplus::Bitmap image(stream);
                        const auto width = image.GetWidth(), height = image.GetHeight();
                        if (image.GetLastStatus() == Gdiplus::Ok && width && height &&
                            std::uint64_t(width) * height <= 40000000) {
                            auto thumb = std::make_shared<Gdiplus::Bitmap>(96, 64, PixelFormat32bppPARGB);
                            Gdiplus::Graphics graphics(thumb.get());
                            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
                            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                            const float scale = std::min(96.f / width, 64.f / height);
                            graphics.DrawImage(&image, (96.f - width * scale) / 2, (64.f - height * scale) / 2,
                                               width * scale, height * scale);
                            attachment.thumbnail = std::move(thumb);
                        }
                    }
                    stream->Release();
                } else GlobalFree(memory);
            } else GlobalFree(memory);
        }
    }
    return attachment;
}
bool ValidAttachmentDirectory(const std::string &path) {
    constexpr std::string_view prefix = "/tmp/serialctl-ai.";
    return path.size() == prefix.size() + 10 && path.rfind(prefix, 0) == 0 &&
        std::all_of(path.begin() + prefix.size(), path.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
}
nlohmann::json AttachmentDirectoryCommand() {
    return {{"command", {"sh", "-c", "umask 077; mktemp -d /tmp/serialctl-ai.XXXXXXXXXX"}},
            {"timeoutMs", 10000}, {"outputBytesCap", 1024}};
}
nlohmann::json AttachmentChunkCommand(const std::string &path, const Attachment &attachment, size_t offset) {
    if (!attachment.bytes || offset > attachment.bytes->size())
        throw std::invalid_argument("Invalid attachment chunk");
    const auto length = std::min(AttachmentChunkSize, attachment.bytes->size() - offset);
    const char *script = offset == 0
        ? "set -eu; umask 077; set -C; printf '%s' \"$2\" | base64 -d > \"$1\"; wc -c < \"$1\""
        : "set -eu; printf '%s' \"$2\" | base64 -d >> \"$1\"; wc -c < \"$1\"";
    return {{"command", {"sh", "-c", script, "serialctl-attachment", path, Base64(*attachment.bytes, offset, length)}},
            {"timeoutMs", 10000}, {"outputBytesCap", 1024}};
}
bool StageLocalAttachments(const std::vector<Attachment> &files, std::string &directoryOut,
                           std::vector<std::string> &stagedPathsOut, std::wstring &errorOut) {
    directoryOut.clear();
    stagedPathsOut.clear();
    errorOut.clear();

    wchar_t baseBuf[32768]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", baseBuf, 32768);
    std::wstring baseDir;
    if (n > 0 && n < 32768) {
        baseDir = std::wstring(baseBuf, n) + L"\\SerialCtl";
        CreateDirectoryW(baseDir.c_str(), nullptr);
        baseDir += L"\\ai-attachments";
        CreateDirectoryW(baseDir.c_str(), nullptr);
    } else {
        DWORD t = GetTempPathW(32768, baseBuf);
        if (t == 0 || t >= 32768) {
            errorOut = L"无法定位本机附件暂存目录";
            return false;
        }
        baseDir = std::wstring(baseBuf, t) + L"serialctl-ai-attachments";
        CreateDirectoryW(baseDir.c_str(), nullptr);
    }

    static std::atomic<uint64_t> seq{1};
    std::wstring dir;
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::wstring candidate = baseDir + L"\\serialctl-ai." + std::to_wstring(GetCurrentProcessId()) +
                                 L"-" + std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed));
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            dir = std::move(candidate);
            break;
        }
    }
    if (dir.empty()) {
        errorOut = L"无法创建本机附件暂存目录：" + Win32ErrorMessage();
        return false;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        const auto &att = files[i];
        std::wstring suffix = MultiByteToWide(reinterpret_cast<const uint8_t *>(att.suffix.data()),
                                              att.suffix.size(), CP_UTF8);
        if (suffix.empty())
            suffix = L".bin";
        const std::wstring target = dir + L"\\" + std::to_wstring(i + 1) + suffix;
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            errorOut = L"无法写入本机暂存附件：" + Win32ErrorMessage();
            CleanupLocalAttachmentDirectory(WideToMultiByte(dir, CP_UTF8));
            return false;
        }
        bool ok = true;
        if (att.bytes && !att.bytes->empty()) {
            size_t offset = 0;
            while (offset < att.bytes->size()) {
                DWORD written = 0;
                const DWORD want = static_cast<DWORD>(std::min<size_t>(att.bytes->size() - offset, 65536));
                if (!WriteFile(h, att.bytes->data() + offset, want, &written, nullptr) || written != want) {
                    ok = false;
                    break;
                }
                offset += written;
            }
        }
        CloseHandle(h);
        if (!ok) {
            errorOut = L"写入本机暂存附件未完成";
            CleanupLocalAttachmentDirectory(WideToMultiByte(dir, CP_UTF8));
            return false;
        }
        stagedPathsOut.push_back(WideToMultiByte(target, CP_UTF8));
    }
    directoryOut = WideToMultiByte(dir, CP_UTF8);
    return true;
}
void CleanupLocalAttachmentDirectory(const std::string &directory) {
    if (directory.empty())
        return;
    const std::wstring wide = MultiByteToWide(reinterpret_cast<const uint8_t *>(directory.data()),
                                              directory.size(), CP_UTF8);
    if (wide.empty() || wide.find(L"serialctl-ai.") == std::wstring::npos)
        return;
    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileW((wide + L"\\*").c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(ffd.cFileName);
            if (name == L"." || name == L"..")
                continue;
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                DeleteFileW((wide + L"\\" + name).c_str());
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
    RemoveDirectoryW(wide.c_str());
}
} // namespace serialctl::ai
