#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace serialctl::ai {

struct ExecProgressEvent {
    enum class Phase { Started, OutputDelta, Completed };
    Phase phase = Phase::Started;
    std::string execId;
    std::string toolName;
    std::string command;
    std::string cwd;
    std::string chunk;
    int exitCode = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool truncated = false;
    bool failed = false;
    std::uint64_t durationMs = 0;
};

struct FileChangeRecord {
    enum class GitLayer { None, Staged, Unstaged, Untracked, Commit };
    std::string changeId;
    std::string path;
    std::string action; // "create", "edit", "move", "delete"
    std::string detail;
    std::string diffText;
    std::string contentPreview;
    size_t addedLines = 0;
    size_t removedLines = 0;
    std::uint64_t turnSeq = 0;
    bool truncated = false;
    bool detectedByCommand = false;
    GitLayer gitLayer = GitLayer::None;
    std::string gitRevision;
};

inline void TruncateUtf8Safe(std::string &text, size_t maxBytes, const std::string &suffix = {}) {
    if (text.size() <= maxBytes)
        return;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    if (cut > 0 && (static_cast<unsigned char>(text[cut - 1]) & 0x80) != 0) {
        size_t start = cut - 1;
        while (start > 0 && (cut - 1 - start) < 3 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
            --start;
        const unsigned char lead = static_cast<unsigned char>(text[start]);
        const size_t expected = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (lead >= 0xC0 && (cut - start) < expected)
            cut = start;
    }
    text.resize(cut);
    if (!suffix.empty())
        text += suffix;
}

inline std::string SingleLineCommandSummary(std::string_view cmd, size_t maxBytes = 120) {
    std::string out;
    out.reserve((std::min)(cmd.size(), maxBytes + 8));
    bool prevSpace = true;
    for (unsigned char ch : cmd) {
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else if (ch >= 0x20 || ch >= 0x80) {
            out.push_back(static_cast<char>(ch));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    if (out.size() > maxBytes)
        TruncateUtf8Safe(out, maxBytes, u8"…");
    return out;
}

inline std::string FormatInlineCodeMarkdown(std::string_view code) {
    size_t maxRun = 0, run = 0;
    for (char ch : code) {
        if (ch == '`') {
            ++run;
            if (run > maxRun)
                maxRun = run;
        } else {
            run = 0;
        }
    }
    const std::string fence(maxRun + 1, '`');
    const bool pad = !code.empty() && (code.front() == '`' || code.back() == '`');
    return fence + (pad ? " " : "") + std::string(code) + (pad ? " " : "") + fence;
}

inline std::string FormatExecDuration(std::uint64_t durationMs) {
    if (durationMs == 0)
        return {};
    char buf[32]{};
    if (durationMs < 1000) {
        std::snprintf(buf, sizeof(buf), "%llums", static_cast<unsigned long long>(durationMs));
    } else {
        const double sec = static_cast<double>(durationMs) / 1000.0;
        std::snprintf(buf, sizeof(buf), "%.1fs", sec);
    }
    return buf;
}

inline std::string FormatCodeFenceMarkdown(const std::string &language, const std::string &code) {
    size_t longestRun = 0;
    size_t currentRun = 0;
    for (unsigned char ch : code) {
        if (ch == '`') {
            ++currentRun;
            if (currentRun > longestRun)
                longestRun = currentRun;
        } else {
            currentRun = 0;
        }
    }
    const std::string fence(std::max<size_t>(3, longestRun + 1), '`');
    return fence + language + "\n" + code + "\n" + fence;
}

inline std::string FormatCompactExecMarkdown(const std::string &title, const std::string &command,
                                             const std::string &execId, bool completed, bool failed, int exitCode,
                                             bool timedOut, bool cancelled, std::uint64_t durationMs) {
    std::string text = "**" + (title.empty() ? std::string(u8"命令执行") : title) + "** · ";
    if (!completed) {
        text += u8"执行中";
    } else if (cancelled) {
        text += u8"已取消";
    } else if (timedOut) {
        text += u8"已超时";
    } else {
        text += failed ? u8"失败" : u8"已完成";
        std::string meta = u8"退出码 " + std::to_string(exitCode);
        const std::string dur = FormatExecDuration(durationMs);
        if (!dur.empty())
            meta += u8"，" + dur;
        text += u8"（" + meta + u8"）";
    }
    const std::string lineCmd = SingleLineCommandSummary(command, 220);
    if (!lineCmd.empty())
        text += "\n\n" + FormatCodeFenceMarkdown("bash", lineCmd);
    if (!execId.empty())
        text += u8"\n\n[右侧查看](serialctl-exec://" + execId + ")";
    return text;
}

inline std::string FormatFileChangeActionLabel(std::string_view action) {
    if (action == "create")
        return u8"新建文件";
    if (action == "delete")
        return u8"删除文件";
    if (action == "move")
        return u8"重命名 / 移动";
    return u8"修改文件";
}

inline std::string EscapeMarkdownLinkLabel(std::string_view label) {
    std::string out;
    out.reserve(label.size());
    for (char ch : label) {
        if (ch == '[' || ch == ']')
            out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

inline std::string FormatFileChangeLinkMarkdown(const FileChangeRecord &rec) {
    std::string label = EscapeMarkdownLinkLabel(rec.path);
    if (rec.addedLines > 0 || rec.removedLines > 0) {
        label += " (+" + std::to_string(rec.addedLines) + " / -" + std::to_string(rec.removedLines) + ")";
    } else if (rec.action == "create") {
        label += " (+0)";
    } else if (rec.action == "delete") {
        label += " (-0)";
    }
    if (rec.changeId.empty())
        return label;
    return "[" + label + "](serialctl-change://" + rec.changeId + ")";
}

inline std::string FormatCompactFileChangeCardMarkdown(const FileChangeRecord &rec) {
    std::string text = "**" + FormatFileChangeActionLabel(rec.action) + u8"** · 已完成\n\n";
    text += FormatFileChangeLinkMarkdown(rec);
    if (!rec.detail.empty() && rec.action == "move")
        text += "\n\n" + rec.detail;
    return text;
}

inline std::vector<std::string> SplitDiffLines(std::string_view text) {
    std::vector<std::string> lines;
    if (text.empty())
        return lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            size_t len = text.size() - start;
            if (len > 0 && text[start + len - 1] == '\r')
                --len;
            lines.emplace_back(text.substr(start, len));
            break;
        }
        size_t len = nl - start;
        if (len > 0 && text[start + len - 1] == '\r')
            --len;
        lines.emplace_back(text.substr(start, len));
        start = nl + 1;
    }
    return lines;
}

inline std::string NormalizeLineEndingsForTerminal(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() + 32);
    for (size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\n' && (i == 0 || utf8[i - 1] != '\r'))
            out.push_back('\r');
        out.push_back(utf8[i]);
    }
    return out;
}

inline void CountDiffChanges(std::string_view diff, size_t &added, size_t &removed) {
    added = 0;
    removed = 0;
    for (const auto &line : SplitDiffLines(diff)) {
        if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0 || line.rfind("@@", 0) == 0)
            continue;
        if (!line.empty() && line[0] == '+')
            ++added;
        else if (!line.empty() && line[0] == '-')
            ++removed;
    }
}

inline std::string ComputeUnifiedDiff(const std::string &path, const std::string &oldText, const std::string &newText,
                                        size_t maxDiffBytes = 48 * 1024, size_t *addedOut = nullptr,
                                        size_t *removedOut = nullptr, bool *truncatedOut = nullptr) {
    if (addedOut)
        *addedOut = 0;
    if (removedOut)
        *removedOut = 0;
    if (truncatedOut)
        *truncatedOut = false;

    const auto oldLines = SplitDiffLines(oldText);
    const auto newLines = SplitDiffLines(newText);
    const size_t totalOld = oldLines.size();
    const size_t totalNew = newLines.size();

    size_t prefix = 0;
    while (prefix < totalOld && prefix < totalNew && oldLines[prefix] == newLines[prefix])
        ++prefix;
    size_t suffix = 0;
    while (suffix + prefix < totalOld && suffix + prefix < totalNew &&
           oldLines[totalOld - 1 - suffix] == newLines[totalNew - 1 - suffix])
        ++suffix;

    if (prefix == totalOld && prefix == totalNew)
        return {};

    const std::string dispPath = path.empty() ? std::string("file") : path;
    std::string out;
    out += "--- " + (totalOld == 0 ? std::string("/dev/null") : ("a/" + dispPath)) + "\n";
    out += "+++ " + (totalNew == 0 ? std::string("/dev/null") : ("b/" + dispPath)) + "\n";

    struct DiffOp {
        char kind; // ' ', '-', '+'
        size_t oldIdx;
        size_t newIdx;
    };
    std::vector<DiffOp> ops;

    const size_t midOld = totalOld - prefix - suffix;
    const size_t midNew = totalNew - prefix - suffix;

    if (midOld == 0 || midNew == 0 || midOld * midNew > 250000) {
        const size_t ctxStart = prefix > 3 ? prefix - 3 : 0;
        const size_t ctxEndOld = (std::min)(totalOld, totalOld - suffix + 3);
        const size_t ctxEndNew = (std::min)(totalNew, totalNew - suffix + 3);
        const size_t oldCount = ctxEndOld - ctxStart;
        const size_t newCount = ctxEndNew - (prefix > 3 ? prefix - 3 : 0);
        size_t added = midNew;
        size_t removed = midOld;
        if (addedOut)
            *addedOut = added;
        if (removedOut)
            *removedOut = removed;
        char hdr[96]{};
        std::snprintf(hdr, sizeof(hdr), "@@ -%zu,%zu +%zu,%zu @@\n",
                      oldCount == 0 ? 0 : (ctxStart + 1), oldCount,
                      newCount == 0 ? 0 : (ctxStart + 1), newCount);
        out += hdr;
        for (size_t i = ctxStart; i < prefix; ++i)
            out += " " + oldLines[i] + "\n";
        for (size_t i = prefix; i < totalOld - suffix; ++i)
            out += "-" + oldLines[i] + "\n";
        for (size_t i = prefix; i < totalNew - suffix; ++i)
            out += "+" + newLines[i] + "\n";
        for (size_t i = totalOld - suffix; i < ctxEndOld; ++i)
            out += " " + oldLines[i] + "\n";
    } else {
        const size_t rows = midOld + 1;
        const size_t cols = midNew + 1;
        std::vector<std::uint32_t> dp(rows * cols, 0);
        for (size_t i = midOld; i-- > 0;) {
            for (size_t j = midNew; j-- > 0;) {
                if (oldLines[prefix + i] == newLines[prefix + j])
                    dp[i * cols + j] = dp[(i + 1) * cols + (j + 1)] + 1;
                else
                    dp[i * cols + j] = (std::max)(dp[(i + 1) * cols + j], dp[i * cols + (j + 1)]);
            }
        }
        ops.reserve(prefix + midOld + midNew + suffix);
        for (size_t i = 0; i < prefix; ++i)
            ops.push_back({' ', i, i});
        size_t i = 0, j = 0;
        size_t added = 0, removed = 0;
        while (i < midOld && j < midNew) {
            if (oldLines[prefix + i] == newLines[prefix + j]) {
                ops.push_back({' ', prefix + i, prefix + j});
                ++i;
                ++j;
            } else if (dp[(i + 1) * cols + j] >= dp[i * cols + (j + 1)]) {
                ops.push_back({'-', prefix + i, prefix + j});
                ++removed;
                ++i;
            } else {
                ops.push_back({'+', prefix + i, prefix + j});
                ++added;
                ++j;
            }
        }
        while (i < midOld) {
            ops.push_back({'-', prefix + i, prefix + j});
            ++removed;
            ++i;
        }
        while (j < midNew) {
            ops.push_back({'+', prefix + i, prefix + j});
            ++added;
            ++j;
        }
        for (size_t s = 0; s < suffix; ++s)
            ops.push_back({' ', totalOld - suffix + s, totalNew - suffix + s});

        if (addedOut)
            *addedOut = added;
        if (removedOut)
            *removedOut = removed;

        constexpr size_t kContext = 3;
        size_t pos = 0;
        while (pos < ops.size()) {
            while (pos < ops.size() && ops[pos].kind == ' ')
                ++pos;
            if (pos >= ops.size())
                break;
            const size_t hunkStart = (pos > kContext) ? (pos - kContext) : 0;
            size_t end = pos;
            while (end < ops.size()) {
                if (ops[end].kind != ' ') {
                    ++end;
                    continue;
                }
                size_t gap = 0;
                while (end + gap < ops.size() && ops[end + gap].kind == ' ')
                    ++gap;
                if (end + gap == ops.size() || gap > kContext * 2) {
                    end = (std::min)(ops.size(), end + kContext);
                    break;
                }
                end += gap;
            }
            size_t oldCount = 0, newCount = 0;
            for (size_t k = hunkStart; k < end; ++k) {
                if (ops[k].kind != '+')
                    ++oldCount;
                if (ops[k].kind != '-')
                    ++newCount;
            }
            const size_t oldStart = oldCount == 0 ? ops[hunkStart].oldIdx : (ops[hunkStart].oldIdx + 1);
            const size_t newStart = newCount == 0 ? ops[hunkStart].newIdx : (ops[hunkStart].newIdx + 1);
            char hdr[96]{};
            std::snprintf(hdr, sizeof(hdr), "@@ -%zu,%zu +%zu,%zu @@\n", oldStart, oldCount, newStart, newCount);
            out += hdr;
            for (size_t k = hunkStart; k < end; ++k) {
                out.push_back(ops[k].kind);
                out += (ops[k].kind == '+') ? newLines[ops[k].newIdx] : oldLines[ops[k].oldIdx];
                out.push_back('\n');
            }
            pos = end;
        }
    }

    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    if (out.size() > maxDiffBytes) {
        if (truncatedOut)
            *truncatedOut = true;
        TruncateUtf8Safe(out, maxDiffBytes, u8"\n[Diff 内容较长，已截断]");
    }
    return out;
}

} // namespace serialctl::ai
