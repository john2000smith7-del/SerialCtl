#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace serialctl {

struct TftpSourceInfo {
    std::string type;             // "local"
    std::wstring displayPath;     // Local file path
    uint64_t size = 0;
    bool sizeKnown = false;
    std::string status = "Ready"; // "Ready", "Failed"
    std::wstring error;
    bool notFound = false;
};

struct TftpDirectoryEntry {
    std::string name;
    uint64_t size = 0;
    bool sizeKnown = false;
};

class LocalTftpFileSource {
public:
    explicit LocalTftpFileSource(std::wstring path);

    const TftpSourceInfo& Info() const { return info_; }
    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
    TftpSourceInfo info_;
};

bool ValidateLocalTftpDirectory(const std::wstring& dirPath, std::wstring& normalizedPath, std::wstring& error);
bool ListLocalTftpDirectory(const std::wstring& dirPath, std::vector<TftpDirectoryEntry>& entries, std::wstring& error);

} // namespace serialctl
