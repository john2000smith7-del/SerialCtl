#pragma once

#include "SftpClient.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace serialctl {

struct SftpReferenceDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

struct SftpModelListing {
    std::wstring directory;
    std::vector<SftpEntry> entries;
};

struct SftpBreadcrumb {
    std::wstring label;
    std::wstring path;
};

enum class SftpSortColumn {
    Name,
    Size,
    Modified
};

std::wstring JoinSftpRemotePath(const std::wstring& directory, const std::wstring& name);
std::wstring ParentSftpRemotePath(const std::wstring& path);
std::wstring BaseNameSftpRemotePath(const std::wstring& path);
std::vector<SftpBreadcrumb> BuildSftpBreadcrumbs(const std::wstring& path);
std::wstring QuotePsftpBatchWord(const std::wstring& value);
std::wstring QuotePsftpWildcardLiteral(const std::wstring& value);

SftpModelListing ParseSftpListingOutput(const std::wstring& output,
    const std::wstring& fallbackDirectory, SftpReferenceDate referenceDate);

void SortSftpEntries(std::vector<SftpEntry>& entries, SftpSortColumn column, bool ascending);

// Returns the most recent valid percentage in a psftp output fragment, or -1
// when the fragment does not contain one.
int ParsePsftpProgressPercent(std::wstring_view output);

} // namespace serialctl
