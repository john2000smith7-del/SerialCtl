#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace serialctl::ai {

inline constexpr std::string_view kPracticeBlockBegin = "<!-- SerialCtl workspace practices BEGIN -->";
inline constexpr std::string_view kPracticeBlockEnd = "<!-- SerialCtl workspace practices END -->";

struct ExtractedManagedBlock {
    std::string prefix;
    std::string managedContent;
    std::string suffix;
    bool hasManagedBlock = false;
};

inline ExtractedManagedBlock ExtractManagedBlock(std::string_view fileContent) {
    ExtractedManagedBlock result;
    const auto beginPos = fileContent.find(kPracticeBlockBegin);
    if (beginPos == std::string_view::npos) {
        result.prefix = std::string(fileContent);
        result.hasManagedBlock = false;
        return result;
    }
    const auto contentStart = beginPos + kPracticeBlockBegin.size();
    const auto endPos = fileContent.find(kPracticeBlockEnd, contentStart);
    if (endPos == std::string_view::npos) {
        result.prefix = std::string(fileContent);
        result.hasManagedBlock = false;
        return result;
    }
    result.hasManagedBlock = true;
    result.prefix = std::string(fileContent.substr(0, beginPos));
    result.managedContent = std::string(fileContent.substr(contentStart, endPos - contentStart));
    result.suffix = std::string(fileContent.substr(endPos + kPracticeBlockEnd.size()));

    // Trim leading/trailing whitespace from managed content
    while (!result.managedContent.empty() && (result.managedContent.front() == '\r' || result.managedContent.front() == '\n')) {
        result.managedContent.erase(result.managedContent.begin());
    }
    while (!result.managedContent.empty() && (result.managedContent.back() == '\r' || result.managedContent.back() == '\n' || result.managedContent.back() == ' ')) {
        result.managedContent.pop_back();
    }
    return result;
}

inline std::string ToLowerAscii(std::string_view s) {
    std::string lower(s);
    for (char &c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower;
}

inline std::string SanitizePracticeEntry(std::string_view rawEntry) {
    // Strip leading/trailing spaces and newlines
    size_t start = 0;
    while (start < rawEntry.size() && (std::isspace(static_cast<unsigned char>(rawEntry[start])))) {
        ++start;
    }
    size_t end = rawEntry.size();
    while (end > start && (std::isspace(static_cast<unsigned char>(rawEntry[end - 1])))) {
        --end;
    }
    if (start >= end) return {};

    std::string entry(rawEntry.substr(start, end - start));
    // A single practice entry must never add additional Markdown lines or
    // contain a hidden NUL/control character.
    for (unsigned char c : entry) {
        if (c == '\n' || c == '\r' || c == 0 || (c < 0x20 && c != '\t')) return {};
    }
    const auto lower = ToLowerAscii(entry);

    // Reject credentials even when there are spaces before '=' or ':',
    // or a bare value follows the name (e.g. "密码 123456").
    // Recognize identifier boundaries so "passwordless" is not mistaken for
    // an actual password assignment.
    auto hasSensitiveWord = [&](std::string_view token) {
        size_t at = lower.find(token);
        while (at != std::string::npos) {
            const auto isAlphaNum = [](unsigned char c) { return std::isalnum(c) != 0; };
            const bool left = at == 0 || !isAlphaNum(static_cast<unsigned char>(lower[at - 1]));
            const size_t after = at + token.size();
            const bool right = after == lower.size() || !isAlphaNum(static_cast<unsigned char>(lower[after]));
            if (left && right) return true;
            at = lower.find(token, at + 1);
        }
        return false;
    };
    const char* sensitiveWords[] = {
        "password", "passwd", "passphrase", "pwd", "api_key", "api-key", "api key",
        "apikey", "secret", "client_secret", "access_key", "access_token",
        "refresh_token", "auth_token", "token", "bearer", "authorization",
        "credential", "sshpass", "private_key", "private key", "cookie"
    };
    for (const auto* word : sensitiveWords) {
        if (hasSensitiveWord(word)) return {};
    }
    const char* sensitiveFragments[] = {
        "-----begin", "private key-----", "sk-", "ghp_", "github_pat_",
        "xoxb-", "xoxp-", "akia", "明文口令", "密码", "口令", "私钥", "密钥", "令牌"
    };
    for (const auto* fragment : sensitiveFragments) {
        if (lower.find(fragment) != std::string::npos) return {};
    }
    // Never persist credentials embedded in a URI such as scheme://user:pass@host.
    const size_t scheme = lower.find("://");
    if (scheme != std::string::npos) {
        const size_t userInfoStart = scheme + 3;
        const size_t authorityEnd = lower.find_first_of("/?#", userInfoStart);
        const auto userInfo = lower.substr(userInfoStart,
            authorityEnd == std::string::npos ? std::string::npos : authorityEnd - userInfoStart);
        if (userInfo.find('@') != std::string::npos) return {};
    }

    // Strip bullet prefix if user typed one (we normalize bullets on formatting)
    if (entry.rfind("- ", 0) == 0 || entry.rfind("* ", 0) == 0) {
        entry = entry.substr(2);
        while (!entry.empty() && entry.front() == ' ') entry.erase(entry.begin());
    }

    // Remove backticks
    entry.erase(std::remove(entry.begin(), entry.end(), '`'), entry.end());

    // Strip leading/trailing spaces again
    while (!entry.empty() && (std::isspace(static_cast<unsigned char>(entry.front())))) entry.erase(entry.begin());
    while (!entry.empty() && (std::isspace(static_cast<unsigned char>(entry.back())))) entry.pop_back();

    if (entry.size() > 300) {
        entry.resize(300);
    }
    return entry;
}

inline std::vector<std::string> ParsePracticeEntries(std::string_view managedContent) {
    std::vector<std::string> entries;
    size_t pos = 0;
    while (pos < managedContent.size()) {
        const auto nextNewline = managedContent.find('\n', pos);
        const auto line = managedContent.substr(pos, nextNewline == std::string_view::npos ? std::string_view::npos : nextNewline - pos);
        auto sanitized = SanitizePracticeEntry(line);
        if (!sanitized.empty() && sanitized.find("### ") != 0) {
            entries.push_back(sanitized);
        }
        if (nextNewline == std::string_view::npos) break;
        pos = nextNewline + 1;
    }
    return entries;
}

inline std::string Base64Encode(std::string_view data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const unsigned int b0 = static_cast<unsigned char>(data[i]);
        const unsigned int b1 = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 0;
        const unsigned int b2 = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 0;

        encoded.push_back(kTable[(b0 >> 2) & 0x3F]);
        encoded.push_back(kTable[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        encoded.push_back((i + 1 < data.size()) ? kTable[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=');
        encoded.push_back((i + 2 < data.size()) ? kTable[b2 & 0x3F] : '=');
    }
    return encoded;
}

// Commands run only in the selected remote workspace. Capture absence separately
// from an empty file, use private unique staging, and reject stale snapshots.
inline std::string WorkspaceReadCommand() {
    return "set -eu; test ! -L AGENTS.md || { echo 'AGENTS.md symlink rejected' >&2; exit 1; }; "
           "if test -e AGENTS.md; then test -f AGENTS.md; "
           "test $(wc -c < AGENTS.md) -le 65536; printf 'PRESENT\\n'; cat AGENTS.md; "
           "else printf 'ABSENT\\n'; fi";
}

inline std::string WorkspaceReplaceCommand(std::string_view expected, bool expectedExists,
                                            std::string_view replacement, bool replacementExists) {
    std::string script =
        "set -eu; umask 077; "
        "mkdir .serialctl-agents.lock 2>/dev/null || { echo 'AGENTS.md writer busy' >&2; exit 1; }; "
        "stage=''; trap 'test -z \"$stage\" || rm -rf -- \"$stage\"; rmdir .serialctl-agents.lock' EXIT HUP INT TERM; "
        "stage=$(mktemp -d .serialctl-agents.XXXXXXXX); "
        "test ! -L AGENTS.md || { echo 'AGENTS.md symlink rejected' >&2; exit 1; }; ";
    script += "printf '%s' '" + Base64Encode(expected) + "' | base64 -d > \"$stage/expected\"; ";
    if (expectedExists)
        script += "test -f AGENTS.md && cmp -s AGENTS.md \"$stage/expected\" || { echo 'AGENTS.md changed; reload required' >&2; exit 1; }; ";
    else
        script += "test ! -e AGENTS.md || { echo 'AGENTS.md created concurrently; reload required' >&2; exit 1; }; ";
    if (replacementExists) {
        script += "printf '%s' '" + Base64Encode(replacement) + "' | base64 -d > \"$stage/replacement\"; ";
        if (expectedExists)
            script += "test -w AGENTS.md; chmod --reference=AGENTS.md \"$stage/replacement\"; "
                      "test ! -L AGENTS.md && cmp -s AGENTS.md \"$stage/expected\" || exit 1; ";
        else script += "test ! -e AGENTS.md && test ! -L AGENTS.md || exit 1; ";
        script += "mv -f -- \"$stage/replacement\" AGENTS.md; ";
    } else {
        script += "test ! -L AGENTS.md && cmp -s AGENTS.md \"$stage/expected\" || exit 1; rm -- AGENTS.md; ";
    }
    return script;
}

inline std::string MergeManagedBlock(std::string_view existingFileContent,
                                    const std::vector<std::string>& newEntries,
                                    size_t maxBytes = 8192) {
    auto extracted = ExtractManagedBlock(existingFileContent);
    auto entries = ParsePracticeEntries(extracted.managedContent);

    for (const auto& raw : newEntries) {
        auto sanitized = SanitizePracticeEntry(raw);
        if (sanitized.empty()) continue;

        // Deduplicate against existing entries (case-insensitive check)
        const auto lower = ToLowerAscii(sanitized);
        auto it = std::find_if(entries.begin(), entries.end(), [&](const std::string& existing) {
            return ToLowerAscii(existing) == lower;
        });
        if (it != entries.end()) {
            // Already present; update to latest wording at end
            entries.erase(it);
        }
        entries.push_back(sanitized);
    }

    // Limit maximum number of high-value items to 60
    constexpr size_t kMaxItems = 60;
    if (entries.size() > kMaxItems) {
        entries.erase(entries.begin(), entries.begin() + (entries.size() - kMaxItems));
    }

    // Build block content while respecting maxBytes limit
    auto assemble = [&](const std::vector<std::string>& items) -> std::string {
        std::string body;
        for (const auto& item : items) {
            body += "- " + item + "\n";
        }
        return body;
    };

    while (!entries.empty()) {
        std::string candidate = assemble(entries);
        const size_t totalBlockSize = kPracticeBlockBegin.size() + 1 + candidate.size() + kPracticeBlockEnd.size() + 2;
        if (totalBlockSize <= maxBytes) {
            break;
        }
        // Drop oldest entries to fit capacity limit
        entries.erase(entries.begin());
    }

    std::string formattedBody = assemble(entries);
    std::string block;
    block.reserve(kPracticeBlockBegin.size() + formattedBody.size() + kPracticeBlockEnd.size() + 4);
    block.append(kPracticeBlockBegin);
    block.push_back('\n');
    block.append(formattedBody);
    block.append(kPracticeBlockEnd);

    if (extracted.hasManagedBlock) {
        return extracted.prefix + block + extracted.suffix;
    }
    // Append managed block to the end of user content, preserving all original content
    std::string result(existingFileContent);
    if (!result.empty() && result.back() != '\n') {
        result.append("\n\n");
    } else if (!result.empty() && result.size() >= 2 && result[result.size() - 2] != '\n') {
        result.push_back('\n');
    }
    result.append(block);
    result.push_back('\n');
    return result;
}

inline std::string BuildTurnContextPrefix(std::string_view fileOrBlockContent, size_t maxPromptBytes = 2048) {
    if (fileOrBlockContent.empty()) return {};

    auto extracted = ExtractManagedBlock(fileOrBlockContent);
    std::string_view block = extracted.hasManagedBlock ? std::string_view(extracted.managedContent) : fileOrBlockContent;
    auto entries = ParsePracticeEntries(block);
    if (entries.empty()) return {};

    std::string prefix = "【工作空间经验 (AGENTS.md)】\n";
    for (const auto& entry : entries) {
        std::string line = "- " + entry + "\n";
        if (prefix.size() + line.size() > maxPromptBytes) {
            prefix += "[- 已按长度限制精简后续工作空间经验]\n";
            break;
        }
        prefix += line;
    }
    return prefix;
}

} // namespace serialctl::ai
