#include "AiAttachment.h"
#include "Win32Helpers.h"
#include <algorithm>
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
} // namespace serialctl::ai
