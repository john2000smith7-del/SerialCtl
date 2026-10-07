#include "SftpModel.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <cwctype>
#include <limits>
#include <sstream>

namespace serialctl {
namespace {

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring NormalizeRemotePath(std::wstring path) {
    if (path.empty()) return L".";
    std::wstring normalized;
    normalized.reserve(path.size());
    for (wchar_t character : path) {
        if (character == L'/' && !normalized.empty() && normalized.back() == L'/') continue;
        normalized.push_back(character);
    }
    while (normalized.size() > 1 && normalized.back() == L'/') normalized.pop_back();
    return normalized.empty() ? L"/" : normalized;
}

int MonthNumber(const std::wstring& month) {
    static const std::array<const wchar_t*, 12> Months = {
        L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
        L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
    std::wstring lowered;
    lowered.reserve(month.size());
    for (wchar_t character : month) lowered.push_back(static_cast<wchar_t>(towlower(character)));
    for (size_t index = 0; index < Months.size(); ++index) {
        if (lowered == Months[index]) return static_cast<int>(index) + 1;
    }
    return 0;
}

bool ParseUnsigned(const std::wstring& value, std::uint64_t& parsed) {
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long number = _wcstoui64(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || !end || *end != L'\0') return false;
    parsed = static_cast<std::uint64_t>(number);
    return true;
}

bool ParseInteger(const std::wstring& value, int& parsed) {
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long number = wcstol(value.c_str(), &end, 10);
    if (errno == ERANGE || end == value.c_str() || !end || *end != L'\0' ||
        number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) return false;
    parsed = static_cast<int>(number);
    return true;
}

std::uint64_t ModifiedSortKey(const std::wstring& monthText, const std::wstring& dayText,
    const std::wstring& timeOrYear, SftpReferenceDate referenceDate) {
    const int month = MonthNumber(monthText);
    int day = 0;
    if (month == 0 || !ParseInteger(dayText, day) || day < 1 || day > 31) return 0;

    int year = 0;
    int hour = 0;
    int minute = 0;
    const size_t colon = timeOrYear.find(L':');
    if (colon == std::wstring::npos) {
        if (!ParseInteger(timeOrYear, year) || year <= 0 || year > 9999) return 0;
    } else {
        if (!ParseInteger(timeOrYear.substr(0, colon), hour) ||
            !ParseInteger(timeOrYear.substr(colon + 1), minute) ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 || referenceDate.year <= 0) return 0;
        year = referenceDate.year;
        // Long listings omit the year for dates near the reference date. Pick
        // the closest year when the visible month crosses the year boundary.
        if (referenceDate.month > 0 && month - referenceDate.month > 6) --year;
        else if (referenceDate.month > 0 && referenceDate.month - month > 6) ++year;
    }
    return static_cast<std::uint64_t>(year) * 100000000ULL +
        static_cast<std::uint64_t>(month) * 1000000ULL +
        static_cast<std::uint64_t>(day) * 10000ULL +
        static_cast<std::uint64_t>(hour) * 100ULL + static_cast<std::uint64_t>(minute);
}

int CompareCaseInsensitive(const std::wstring& left, const std::wstring& right) {
    const size_t common = std::min(left.size(), right.size());
    for (size_t index = 0; index < common; ++index) {
        const wchar_t leftCharacter = static_cast<wchar_t>(towlower(left[index]));
        const wchar_t rightCharacter = static_cast<wchar_t>(towlower(right[index]));
        if (leftCharacter < rightCharacter) return -1;
        if (leftCharacter > rightCharacter) return 1;
    }
    if (left.size() < right.size()) return -1;
    if (left.size() > right.size()) return 1;
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

bool LooksLikePermissions(const std::wstring& value) {
    if (value.size() < 10) return false;
    return value.front() == L'-' || value.front() == L'd' || value.front() == L'l' ||
        value.front() == L'b' || value.front() == L'c' || value.front() == L'p' ||
        value.front() == L's';
}

int HexValue(wchar_t character) {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
}

bool DecodeHexText(const std::wstring& encoded, std::wstring& decoded,
    bool* validUtf8 = nullptr) {
    if (encoded.size() % 2 != 0) return false;
    std::string bytes;
    bytes.reserve(encoded.size() / 2);
    for (size_t index = 0; index < encoded.size(); index += 2) {
        const int high = HexValue(encoded[index]);
        const int low = HexValue(encoded[index + 1]);
        if (high < 0 || low < 0) return false;
        const char value = static_cast<char>((high << 4) | low);
        if (value == '\0') return false;
        bytes.push_back(value);
    }
    if (bytes.empty()) {
        decoded.clear();
        return true;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    const bool utf8 = count > 0;
    if (validUtf8) *validUtf8 = utf8;
    const UINT codePage = utf8 ? CP_UTF8 : CP_ACP;
    const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    count = MultiByteToWideChar(codePage, flags,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return false;
    decoded.assign(static_cast<size_t>(count), L'\0');
    return MultiByteToWideChar(codePage, flags, bytes.data(),
        static_cast<int>(bytes.size()), decoded.data(), count) == count;
}

std::vector<std::wstring> SplitTabs(const std::wstring& text) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = text.find(L'\t', start);
        fields.push_back(text.substr(start,
            tab == std::wstring::npos ? std::wstring::npos : tab - start));
        if (tab == std::wstring::npos) return fields;
        start = tab + 1;
    }
}

std::wstring FormatPermissions(std::uint64_t mode) {
    wchar_t type = L'-';
    switch (mode & 0170000ULL) {
    case 0040000ULL: type = L'd'; break;
    case 0120000ULL: type = L'l'; break;
    case 0060000ULL: type = L'b'; break;
    case 0020000ULL: type = L'c'; break;
    case 0010000ULL: type = L'p'; break;
    case 0140000ULL: type = L's'; break;
    default: break;
    }
    std::wstring result(10, L'-');
    result[0] = type;
    constexpr std::uint64_t Bits[] = {
        0400ULL, 0200ULL, 0100ULL, 0040ULL, 0020ULL, 0010ULL, 0004ULL, 0002ULL, 0001ULL};
    constexpr wchar_t Letters[] = {L'r', L'w', L'x', L'r', L'w', L'x', L'r', L'w', L'x'};
    for (size_t index = 0; index < std::size(Bits); ++index)
        if (mode & Bits[index]) result[index + 1] = Letters[index];
    return result;
}

std::wstring FormatUnixTime(std::uint64_t seconds) {
    if (seconds == 0 || seconds > static_cast<std::uint64_t>(std::numeric_limits<__time64_t>::max()))
        return {};
    const __time64_t value = static_cast<__time64_t>(seconds);
    tm local{};
    if (_localtime64_s(&local, &value) != 0) return {};
    wchar_t text[32]{};
    swprintf_s(text, L"%04d-%02d-%02d %02d:%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
    return text;
}

bool ParseMachineEntry(const std::wstring& line, SftpEntry& entry) {
    constexpr wchar_t Prefix[] = L"SERIALCTL_ENTRY\t";
    if (line.rfind(Prefix, 0) != 0) return false;
    const std::vector<std::wstring> fields = SplitTabs(line.substr(std::size(Prefix) - 1));
    if (fields.size() != 5) return false;
    std::uint64_t flags = 0;
    std::uint64_t size = 0;
    std::uint64_t permissions = 0;
    std::uint64_t modified = 0;
    bool validUtf8 = false;
    if (!ParseUnsigned(fields[0], flags) || !ParseUnsigned(fields[1], size) ||
        !ParseUnsigned(fields[2], permissions) || !ParseUnsigned(fields[3], modified) ||
        !DecodeHexText(fields[4], entry.name, &validUtf8) || entry.name.empty() ||
        entry.name == L"." || entry.name == L"..") return false;
    constexpr std::uint64_t AttrSize = 0x00000001ULL;
    constexpr std::uint64_t AttrPermissions = 0x00000004ULL;
    constexpr std::uint64_t AttrTimes = 0x00000008ULL;
    if (flags & AttrSize) entry.size = size;
    if (flags & AttrPermissions) {
        entry.permissions = FormatPermissions(permissions);
        entry.directory = (permissions & 0170000ULL) == 0040000ULL;
        entry.symlink = (permissions & 0170000ULL) == 0120000ULL;
    }
    if (flags & AttrTimes) {
        entry.modifiedSortKey = modified;
        entry.modifiedText = FormatUnixTime(modified);
    }
    // READDIR names are path components, so '/' is never a valid literal
    // filename byte. U+001A is also unsafe in PuTTY's Windows text-mode
    // batch files because the CRT interprets it as end-of-file.
    entry.pathSafe = validUtf8 &&
        entry.name != L"." && entry.name != L".." &&
        entry.name.find_first_of(L"/\r\n\x001A") == std::wstring::npos;
    return true;
}

} // namespace

std::wstring JoinSftpRemotePath(const std::wstring& directory, const std::wstring& name) {
    if (!name.empty() && name.front() == L'/') return NormalizeRemotePath(name);
    const std::wstring parent = NormalizeRemotePath(directory);
    if (name.empty()) return parent;
    if (parent == L"." || parent.empty()) return name;
    if (parent == L"/") return L"/" + name;
    return parent + L"/" + name;
}

std::wstring ParentSftpRemotePath(const std::wstring& path) {
    const std::wstring normalized = NormalizeRemotePath(path);
    if (normalized == L"/" || normalized == L".") return normalized;
    const size_t slash = normalized.find_last_of(L'/');
    if (slash == std::wstring::npos) return L".";
    return slash == 0 ? L"/" : normalized.substr(0, slash);
}

std::wstring BaseNameSftpRemotePath(const std::wstring& path) {
    const std::wstring normalized = NormalizeRemotePath(path);
    if (normalized == L"/" || normalized == L".") return normalized;
    const size_t slash = normalized.find_last_of(L'/');
    return slash == std::wstring::npos ? normalized : normalized.substr(slash + 1);
}

std::vector<SftpBreadcrumb> BuildSftpBreadcrumbs(const std::wstring& path) {
    const std::wstring normalized = NormalizeRemotePath(path);
    if (normalized == L"/") return {{L"/", L"/"}};
    if (normalized == L".") return {{L".", L"."}};

    std::vector<SftpBreadcrumb> result;
    const bool absolute = normalized.front() == L'/';
    std::wstring current = absolute ? L"/" : std::wstring();
    if (absolute) result.push_back({L"/", L"/"});

    size_t position = absolute ? 1 : 0;
    while (position < normalized.size()) {
        const size_t separator = normalized.find(L'/', position);
        const std::wstring label = normalized.substr(position,
            separator == std::wstring::npos ? std::wstring::npos : separator - position);
        if (!label.empty()) {
            if (current.empty()) current = label;
            else if (current == L"/") current += label;
            else current += L"/" + label;
            result.push_back({label, current});
        }
        if (separator == std::wstring::npos) break;
        position = separator + 1;
    }
    return result;
}

std::wstring QuotePsftpBatchWord(const std::wstring& value) {
    // PuTTY opens batch files in CRT text mode, where U+001A is treated as
    // end-of-file and could otherwise truncate a generated command.
    if (value.find_first_of(L"\r\n\x001A") != std::wstring::npos) return {};
    std::wstring quoted = L"\"";
    for (wchar_t character : value) {
        if (character == L'\"') quoted += L"\"\"";
        else quoted.push_back(character);
    }
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring QuotePsftpWildcardLiteral(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() * 2);
    for (wchar_t character : value) {
        if (character == L'\\' || character == L'*' || character == L'?' ||
            character == L'[' || character == L']')
            escaped.push_back(L'\\');
        escaped.push_back(character);
    }
    return QuotePsftpBatchWord(escaped);
}

SftpModelListing ParseSftpListingOutput(const std::wstring& output,
    const std::wstring& fallbackDirectory, SftpReferenceDate referenceDate) {
    SftpModelListing listing;
    listing.directory = fallbackDirectory.empty() ? L"." : NormalizeRemotePath(fallbackDirectory);
    std::wistringstream lines(output);
    std::wstring line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        constexpr wchar_t MachineDirectoryPrefix[] = L"SERIALCTL_PWD\t";
        if (line.rfind(MachineDirectoryPrefix, 0) == 0) {
            std::wstring directory;
            bool validUtf8 = false;
            if (DecodeHexText(line.substr(std::size(MachineDirectoryPrefix) - 1),
                    directory, &validUtf8) && validUtf8 && !directory.empty())
                listing.directory = NormalizeRemotePath(directory);
            continue;
        }
        SftpEntry machineEntry;
        if (ParseMachineEntry(line, machineEntry)) {
            listing.entries.push_back(std::move(machineEntry));
            continue;
        }
        constexpr wchar_t RemotePrefix[] = L"Remote directory is ";
        if (line.rfind(RemotePrefix, 0) == 0) {
            const std::wstring directory = Trim(line.substr(std::size(RemotePrefix) - 1));
            if (!directory.empty()) listing.directory = NormalizeRemotePath(directory);
            continue;
        }

        std::wistringstream fields(line);
        std::wstring permissions;
        std::wstring links;
        std::wstring owner;
        std::wstring group;
        std::wstring sizeText;
        std::wstring month;
        std::wstring day;
        std::wstring timeOrYear;
        if (!(fields >> permissions >> links >> owner >> group >> sizeText >> month >> day >> timeOrYear) ||
            !LooksLikePermissions(permissions)) continue;

        std::wstring name;
        std::getline(fields, name);
        if (!name.empty() && (name.front() == L' ' || name.front() == L'\t')) name.erase(name.begin());
        if (name.empty() || name == L"." || name == L"..") continue;

        SftpEntry entry;
        entry.permissions = permissions;
        entry.directory = permissions.front() == L'd';
        entry.symlink = permissions.front() == L'l';
        if (entry.symlink) {
            const size_t arrow = name.rfind(L" -> ");
            if (arrow != std::wstring::npos) {
                entry.linkTarget = name.substr(arrow + 4);
                name.resize(arrow);
            }
        }
        entry.name = name;
        ParseUnsigned(sizeText, entry.size);
        entry.modifiedText = month + L" " + day + L" " + timeOrYear;
        entry.modifiedSortKey = ModifiedSortKey(month, day, timeOrYear, referenceDate);
        // Stock psftp emits a human-formatted long listing, which cannot
        // represent every POSIX filename unambiguously. Keep it browseable,
        // but only the bundled machine-readable mode is safe for mutations.
        entry.pathSafe = false;
        listing.entries.push_back(std::move(entry));
    }
    return listing;
}

void SortSftpEntries(std::vector<SftpEntry>& entries, SftpSortColumn column, bool ascending) {
    std::stable_sort(entries.begin(), entries.end(), [column, ascending](
        const SftpEntry& left, const SftpEntry& right) {
        if (left.directory != right.directory) return left.directory;

        int comparison = 0;
        if (column == SftpSortColumn::Name) {
            comparison = CompareCaseInsensitive(left.name, right.name);
        } else if (column == SftpSortColumn::Size) {
            comparison = left.size < right.size ? -1 : (left.size > right.size ? 1 : 0);
        } else {
            if ((left.modifiedSortKey == 0) != (right.modifiedSortKey == 0))
                return left.modifiedSortKey != 0;
            if (left.modifiedSortKey < right.modifiedSortKey) comparison = -1;
            else if (left.modifiedSortKey > right.modifiedSortKey) comparison = 1;
        }
        if (comparison != 0) return ascending ? comparison < 0 : comparison > 0;
        return CompareCaseInsensitive(left.name, right.name) < 0;
    });
}

int ParsePsftpProgressPercent(std::wstring_view output) {
    int latest = -1;
    for (size_t percent = 0; percent < output.size(); ++percent) {
        if (output[percent] != L'%') continue;
        size_t end = percent;
        while (end > 0 && iswspace(output[end - 1])) --end;
        size_t begin = end;
        while (begin > 0 && iswdigit(output[begin - 1])) --begin;
        if (begin == end) continue;
        int value = 0;
        bool valid = true;
        for (size_t index = begin; index < end; ++index) {
            value = value * 10 + (output[index] - L'0');
            if (value > 100) {
                valid = false;
                break;
            }
        }
        if (valid) latest = value;
    }
    return latest;
}

} // namespace serialctl
