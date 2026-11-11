#pragma once
#include "ByteTypes.h"
#include "json.hpp"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <memory>
#include <string>

namespace serialctl::ai {
inline constexpr size_t AttachmentCountLimit = 8;
inline constexpr size_t AttachmentSizeLimit = 8 * 1024 * 1024;
inline constexpr size_t AttachmentTotalLimit = 16 * 1024 * 1024;
inline constexpr size_t AttachmentChunkSize = 48 * 1024;
struct Attachment {
    std::wstring name;
    std::shared_ptr<const Bytes> bytes;
    std::shared_ptr<Gdiplus::Bitmap> thumbnail;
    bool image = false;
    std::string suffix;
};
Attachment ReadAttachment(const std::wstring &path);
bool ValidAttachmentDirectory(const std::string &path);
nlohmann::json AttachmentDirectoryCommand();
nlohmann::json AttachmentChunkCommand(const std::string &path, const Attachment &, size_t offset);
} // namespace serialctl::ai
