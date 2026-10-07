#include "SftpModel.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

void ExpectText(const std::wstring& actual, const std::wstring& expected, const char* message) {
    if (actual == expected) return;
    std::wcerr << L"FAILED: " << message << L" expected=[" << expected <<
        L"] actual=[" << actual << L"]\n";
    ++failures;
}

serialctl::SftpEntry Entry(std::wstring name, bool directory, std::uint64_t size,
    std::uint64_t modified) {
    serialctl::SftpEntry entry;
    entry.name = std::move(name);
    entry.directory = directory;
    entry.size = size;
    entry.modifiedSortKey = std::move(modified);
    return entry;
}

void TestRemotePaths() {
    using namespace serialctl;
    ExpectText(JoinSftpRemotePath(L"/home/linux/", L"项目 文档.txt"),
        L"/home/linux/项目 文档.txt", "join normalizes the separator");
    ExpectText(JoinSftpRemotePath(L".", L"file.txt"), L"file.txt",
        "join preserves relative current-directory behavior");
    ExpectText(JoinSftpRemotePath(L"/ignored", L"/var/log"), L"/var/log",
        "an absolute child replaces its parent");
    ExpectText(JoinSftpRemotePath(L"/home/linux", L"foo\\bar"), L"/home/linux/foo\\bar",
        "a POSIX file name preserves a literal backslash");
    ExpectText(ParentSftpRemotePath(L"/home/linux/"), L"/home",
        "parent ignores a trailing separator");
    ExpectText(ParentSftpRemotePath(L"/"), L"/", "root is its own parent");
    ExpectText(ParentSftpRemotePath(L"relative"), L".", "single relative component has dot parent");
    ExpectText(BaseNameSftpRemotePath(L"/home/linux/"), L"linux", "base name ignores trailing slash");
    ExpectText(BaseNameSftpRemotePath(L"/"), L"/", "root base name remains root");

    const auto breadcrumbs = BuildSftpBreadcrumbs(L"//home//linux/项目/");
    Expect(breadcrumbs.size() == 4, "absolute path produces root and component breadcrumbs");
    if (breadcrumbs.size() == 4) {
        ExpectText(breadcrumbs[0].label, L"/", "first breadcrumb is root");
        ExpectText(breadcrumbs[1].path, L"/home", "first component breadcrumb has navigable path");
        ExpectText(breadcrumbs[2].path, L"/home/linux", "second component breadcrumb accumulates path");
        ExpectText(breadcrumbs[3].path, L"/home/linux/项目", "unicode breadcrumb accumulates path");
    }
}

void TestPsftpBatchQuoting() {
    using namespace serialctl;
    ExpectText(QuotePsftpBatchWord(L"a b\"c"), L"\"a b\"\"c\"",
        "batch quoting preserves a literal double quote");
    ExpectText(QuotePsftpWildcardLiteral(L"/tmp/\\*?[x]"),
        L"\"/tmp/\\\\\\*\\?\\[x\\]\"",
        "wildcard quoting escapes every PuTTY wildcard metacharacter");
    Expect(QuotePsftpBatchWord(L"unsafe\nname").empty(),
        "batch quoting rejects command line delimiters");
    Expect(QuotePsftpBatchWord(L"unsafe\x001Aname").empty(),
        "batch quoting rejects Windows text-mode EOF characters");
}

void TestListingParser() {
    const std::wstring output =
        L"psftp> pwd\r\n"
        L"Remote directory is /home/linux/workspace\r\n"
        L"drwxr-xr-x  4 linux linux 4096 Sep 30 2026 项目 目录\r\n"
        L"-rw-r--r--  1 linux linux 1536 Oct  3 15:40 报告 final.txt\r\n"
        L"lrwxrwxrwx  1 linux linux   11 Oct  2 2026 current link -> releases/v2\r\n"
        L"-rw-r--r--  1 linux linux    4 Oct  1 2026 a -> b.txt\r\n"
        L"drwxr-xr-x  2 linux linux 4096 Oct  3 15:00 .\r\n"
        L"unrelated diagnostic text\r\n";
    const auto listing = serialctl::ParseSftpListingOutput(output, L".", {2026, 10, 3});
    ExpectText(listing.directory, L"/home/linux/workspace", "parser reads the resolved remote directory");
    Expect(listing.entries.size() == 4, "parser keeps valid entries and skips dot/diagnostics");
    if (listing.entries.size() != 4) return;

    const auto& directory = listing.entries[0];
    Expect(directory.directory, "permissions identify directories");
    ExpectText(directory.name, L"项目 目录", "parser preserves unicode names with spaces");
    ExpectText(directory.permissions, L"drwxr-xr-x", "parser preserves permissions");
    ExpectText(directory.modifiedText, L"Sep 30 2026", "parser preserves display modification text");
    Expect(directory.modifiedSortKey == 202609300000ULL, "parser normalizes a year timestamp");

    const auto& recent = listing.entries[1];
    ExpectText(recent.name, L"报告 final.txt", "parser preserves a unicode file name with spaces");
    Expect(recent.size == 1536, "parser reads a 64-bit file size");
    Expect(recent.modifiedSortKey == 202610031540ULL,
        "parser infers the reference year for recent files");

    const auto& link = listing.entries[2];
    Expect(link.symlink, "link permissions mark a symlink");
    Expect(!link.directory, "symlink is not classified as a directory from its target");
    ExpectText(link.name, L"current link", "parser removes a symlink target from its name");
    ExpectText(link.linkTarget, L"releases/v2", "parser preserves a symlink target");

    const auto& arrowFile = listing.entries[3];
    Expect(!arrowFile.symlink, "regular file containing an arrow remains a regular file");
    ExpectText(arrowFile.name, L"a -> b.txt", "regular file name keeps arrow text");

    const auto newYear = serialctl::ParseSftpListingOutput(
        L"-rw-r--r-- 1 u g 1 Dec 31 23:59 previous.txt\n", L"/", {2027, 1, 2});
    Expect(newYear.entries.size() == 1, "new-year sample parses");
    if (!newYear.entries.empty())
        Expect(newYear.entries[0].modifiedSortKey == 202612312359ULL,
            "recent December file is assigned to the preceding year in January");

    const auto unusual = serialctl::ParseSftpListingOutput(
        L"-rw-r--r-- 1 u g 1 Oct 3 2026  leading and trailing  \n"
        L"lrwxrwxrwx 1 u g 1 Oct 3 2026 foo -> bar -> baz\n", L"/", {2026, 10, 3});
    Expect(unusual.entries.size() == 2, "fallback parser keeps unusual valid rows");
    if (unusual.entries.size() == 2) {
        ExpectText(unusual.entries[0].name, L" leading and trailing  ",
            "fallback parser preserves filename leading and trailing spaces");
        ExpectText(unusual.entries[1].name, L"foo -> bar",
            "fallback symlink parsing uses the final arrow separator");
        Expect(!unusual.entries[0].pathSafe && !unusual.entries[1].pathSafe,
            "ambiguous human listings are read-only");
    }
}

void TestMachineListingParser() {
    const std::wstring output =
        L"SERIALCTL_PWD\t2F686F6D652F6C696E7578\n"
        L"SERIALCTL_ENTRY\t13\t42\t33188\t1791024000\t20666F6F205C2A3F5B785D20\n"
        L"SERIALCTL_ENTRY\t13\t1\t33188\t1791024000\t6261640A6E616D65\n"
        L"SERIALCTL_ENTRY\t13\t2\t33188\t1791024000\t2F6162736F6C757465\n"
        L"SERIALCTL_ENTRY\t13\t3\t33188\t1791024000\t2E2E2F78\n"
        L"SERIALCTL_ENTRY\t13\t4\t33188\t1791024000\t6261641A6E616D65\n";
    const auto listing = serialctl::ParseSftpListingOutput(output, L".", {2026, 10, 3});
    ExpectText(listing.directory, L"/home/linux", "machine listing decodes the exact pwd");
    Expect(listing.entries.size() == 5, "machine listing parses all encoded entries");
    if (listing.entries.size() == 5) {
        ExpectText(listing.entries[0].name, L" foo \\*?[x] ",
            "machine listing preserves spaces, backslash, and wildcard characters");
        Expect(listing.entries[0].pathSafe,
            "machine filename without line delimiters is safe for exact operations");
        ExpectText(listing.entries[0].permissions, L"-rw-r--r--",
            "machine listing formats permission bits");
        Expect(listing.entries[0].size == 42 && listing.entries[0].modifiedSortKey == 1791024000ULL,
            "machine listing preserves size and epoch modification time");
        Expect(!listing.entries[1].pathSafe,
            "machine filename containing a newline is shown but protected from operations");
        ExpectText(listing.entries[2].name, L"/absolute",
            "machine listing preserves a non-conforming absolute entry name for display");
        Expect(!listing.entries[2].pathSafe,
            "machine filename beginning with a slash is protected from operations");
        ExpectText(listing.entries[3].name, L"../x",
            "machine listing preserves a non-conforming parent-relative entry name for display");
        Expect(!listing.entries[3].pathSafe,
            "machine filename containing a slash is protected from operations");
        Expect(!listing.entries[4].pathSafe,
            "machine filename containing Ctrl-Z is protected from batch operations");
    }
}

void TestSorting() {
    using namespace serialctl;
    std::vector<SftpEntry> entries = {
        Entry(L"zeta.txt", false, 100, 202601010000ULL),
        Entry(L"Beta", true, 900, 202601030000ULL),
        Entry(L"alpha.txt", false, 5, 202601020000ULL),
        Entry(L"alpha", true, 1000, 202601040000ULL),
        Entry(L"unknown.txt", false, 1, 0)};

    SortSftpEntries(entries, SftpSortColumn::Name, true);
    Expect(entries[0].directory && entries[1].directory,
        "directories remain first when sorting names ascending");
    ExpectText(entries[0].name, L"alpha", "directory names sort case-insensitively");
    ExpectText(entries[2].name, L"alpha.txt", "file names sort after all directories");

    SortSftpEntries(entries, SftpSortColumn::Size, false);
    Expect(entries[0].directory && entries[1].directory,
        "directories remain first when sorting sizes descending");
    ExpectText(entries[0].name, L"alpha", "directory group applies descending size sort");
    ExpectText(entries[2].name, L"zeta.txt", "file group applies descending size sort");

    SortSftpEntries(entries, SftpSortColumn::Modified, false);
    ExpectText(entries[0].name, L"alpha", "directories use descending modified sort");
    ExpectText(entries[2].name, L"alpha.txt", "files use descending modified sort");
    ExpectText(entries.back().name, L"unknown.txt", "unknown timestamps remain last for either direction");
}

void TestProgressParser() {
    using serialctl::ParsePsftpProgressPercent;
    Expect(ParsePsftpProgressPercent(L"file.bin | 512 kB | 20.0 kB/s | ETA 00:01 | 37%") == 37,
        "parser reads a standard psftp progress percentage");
    Expect(ParsePsftpProgressPercent(L"\r 4%\r 19 %\r 100%\r\n") == 100,
        "parser returns the most recent percentage in a streamed fragment");
    Expect(ParsePsftpProgressPercent(L"no progress available") == -1,
        "parser reports absence of progress");
    Expect(ParsePsftpProgressPercent(L"invalid 101% but valid 88%") == 88,
        "parser ignores percentages outside the valid range");
}

} // namespace

int main() {
    TestRemotePaths();
    TestPsftpBatchQuoting();
    TestListingParser();
    TestMachineListingParser();
    TestSorting();
    TestProgressParser();
    if (failures == 0) std::cout << "SFTP model tests passed\n";
    return failures == 0 ? 0 : 1;
}
