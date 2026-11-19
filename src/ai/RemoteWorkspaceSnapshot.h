#pragma once
#include "WorkspaceActivity.h"
#include "json.hpp"

namespace serialctl::ai {

// Read only regular files anchored to directory descriptors. Never open a symlink,
// special file, or repository metadata. Cap the walk, preview bytes, and records.
inline constexpr wchar_t RemoteSnapshotProgram[] = LR"PY(
import hashlib, json, os, stat, sys
root = os.path.realpath(sys.argv[1])
if root == "/" or not os.path.isdir(root):
    sys.exit(2)
identity = os.stat(root)
files = {}
remaining = 1024 * 1024
scanned = 0
incomplete = False
def unreadable(_):
    global incomplete
    incomplete = True
skip = {".git", ".svn", ".ssh", ".gnupg", ".aws", "node_modules", "__pycache__", ".venv"}
for folder, dirs, names, fd in os.fwalk(root, onerror=unreadable):
    dirs[:] = sorted(d for d in dirs if d not in skip)
    scanned += len(dirs) + len(names)
    if scanned > 4000:
        incomplete = True
        break
    for name in sorted(names):
        path = os.path.relpath(os.path.join(folder, name), root)
        try:
            path.encode("utf-8")
            info = os.stat(name, dir_fd=fd, follow_symlinks=False)
            if not stat.S_ISREG(info.st_mode):
                continue
            file = {"stamp": [info.st_size, info.st_mtime_ns, info.st_ctime_ns]}
            if 0 <= info.st_size <= min(65536, remaining):
                handle = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
                with os.fdopen(handle, "rb") as stream:
                    current = os.fstat(stream.fileno())
                    if not stat.S_ISREG(current.st_mode) or (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino):
                        incomplete = True
                        continue
                    raw = stream.read(info.st_size + 1)
                    final = os.fstat(stream.fileno())
                if len(raw) != info.st_size or (final.st_mtime_ns, final.st_ctime_ns) != (info.st_mtime_ns, info.st_ctime_ns):
                    incomplete = True
                    continue
                remaining -= len(raw)
                file["digest"] = hashlib.sha256(raw).hexdigest()
                if b"\0" not in raw:
                    try:
                        file["text"] = raw.decode("utf-8")
                    except UnicodeDecodeError:
                        pass
            files[path] = file
        except (OSError, UnicodeError):
            incomplete = True
final_root = os.stat(root)
if (identity.st_dev, identity.st_ino) != (final_root.st_dev, final_root.st_ino):
    sys.exit(2)
print(json.dumps({"files": files, "complete": not incomplete,
                  "root": [root, identity.st_dev, identity.st_ino]}, ensure_ascii=True, separators=(",", ":")))
)PY";

inline std::vector<FileChangeRecord> CompareRemoteWorkspaceSnapshots(const nlohmann::json &before,
                                                                      const nlohmann::json &after) {
    std::vector<FileChangeRecord> records;
    const auto &oldFiles = before.at("files");
    const auto &newFiles = after.at("files");
    const auto add = [&](const std::string &path, const nlohmann::json *oldFile, const nlohmann::json *newFile) {
        if (records.size() >= 100) return;
        FileChangeRecord record;
        record.path = path;
        record.action = !oldFile ? "create" : !newFile ? "delete" : "edit";
        record.detectedByCommand = true;
        if ((!oldFile || oldFile->contains("text")) && (!newFile || newFile->contains("text"))) {
            record.diffText = ComputeUnifiedDiff(path, oldFile ? oldFile->at("text").get<std::string>() : std::string(),
                newFile ? newFile->at("text").get<std::string>() : std::string(), 48 * 1024,
                &record.addedLines, &record.removedLines, &record.truncated);
        }
        if (record.diffText.empty()) record.detail = u8"检测到远程文件变动，文本预览不可用";
        records.push_back(std::move(record));
    };
    for (auto it = newFiles.begin(); it != newFiles.end(); ++it) {
        const auto oldFile = oldFiles.find(it.key());
        if (oldFile == oldFiles.end()) {
            if (before.value("complete", false)) add(it.key(), nullptr, &it.value());
        } else {
            const bool haveBothDigests = oldFile->contains("digest") && it->contains("digest");
            if (haveBothDigests ? oldFile->at("digest") != it->at("digest") : oldFile->at("stamp") != it->at("stamp"))
                add(it.key(), &*oldFile, &it.value());
        }
    }
    if (after.value("complete", false)) {
        for (auto it = oldFiles.begin(); it != oldFiles.end(); ++it) {
            if (!newFiles.contains(it.key())) add(it.key(), &it.value(), nullptr);
        }
    }
    return records;
}
}
