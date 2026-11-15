#pragma once
#include "AiProtocol.h"
#include <cctype>
#include <set>

namespace serialctl::ai {
inline bool ValidRemoteWorkspacePath(const std::string &path) {
    return !path.empty() && path.front() == '/' && path.size() <= 4096 &&
           std::none_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

inline bool ValidLocalWorkspacePath(const std::string &path) {
    if (path.size() < 3 || path.size() > 4096)
        return false;
    if (std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return false;
    // Disallow UNC (\\server\share) and Win32 device namespace (\\.\, \\?\)
    if ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'))
        return false;
    const unsigned char drive = static_cast<unsigned char>(path[0]);
    if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) ||
        path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
        return false;
    for (size_t i = 2; i < path.size(); ++i) {
        const char ch = path[i];
        if (ch == ':' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*')
            return false;
    }
    // Reject explicit parent-traversal segments
    size_t pos = 2;
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == '\\' || path[pos] == '/'))
            ++pos;
        if (pos >= path.size())
            break;
        size_t next = path.find_first_of("\\/", pos);
        if (next == std::string::npos)
            next = path.size();
        const std::string seg = path.substr(pos, next - pos);
        if (seg == "." || seg == "..")
            return false;
        pos = next;
    }
    return true;
}

inline bool IsLocalWorkspacePath(const std::string &path) {
    return ValidLocalWorkspacePath(path);
}

inline bool ValidWorkspacePath(const std::string &path) {
    return ValidRemoteWorkspacePath(path) || ValidLocalWorkspacePath(path);
}

inline bool ValidWorkspacePath(const std::string &path, bool local) {
    return local ? ValidLocalWorkspacePath(path) : ValidRemoteWorkspacePath(path);
}

inline std::string NormalizeLocalWorkspacePathUtf8(std::string path) {
    if (!ValidLocalWorkspacePath(path))
        return {};
    path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    for (char &ch : path) {
        if (ch == '/')
            ch = '\\';
    }
    std::string collapsed;
    collapsed.reserve(path.size());
    for (char ch : path) {
        if (ch == '\\' && !collapsed.empty() && collapsed.back() == '\\')
            continue;
        collapsed.push_back(ch);
    }
    while (collapsed.size() > 3 && collapsed.back() == '\\')
        collapsed.pop_back();
    if (collapsed.size() == 2 && collapsed[1] == ':')
        collapsed.push_back('\\');
    return collapsed;
}

inline std::string WorkspaceParent(std::string path) {
    if (ValidLocalWorkspacePath(path)) {
        path = NormalizeLocalWorkspacePathUtf8(std::move(path));
        if (path.size() <= 3)
            return path;
        const auto slash = path.rfind('\\');
        if (slash == std::string::npos || slash <= 2)
            return path.substr(0, 3);
        return path.substr(0, slash);
    }
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

inline std::wstring WorkspaceLeafLabel(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'/' || path.back() == L'\\')) {
        if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
            break;
        path.pop_back();
    }
    if (path.size() == 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        path[0] = static_cast<wchar_t>(std::toupper(static_cast<wint_t>(path[0])));
        path[2] = L'\\';
        return path;
    }
    const auto slash = path.find_last_of(L"/\\");
    if (slash != std::wstring::npos && slash + 1 < path.size())
        return path.substr(slash + 1);
    return path.empty() ? std::wstring(L"工作空间") : path;
}

inline Json WorkspaceDirectories(const std::string &parent, const Json &entries) {
    Json result = Json::array();
    if (!ValidRemoteWorkspacePath(parent) || !entries.is_array())
        return result;
    std::set<std::string> paths;
    for (const auto &entry : entries) {
        if (!entry.is_object() || !entry.value("isDirectory", false) || !entry.contains("fileName") ||
            !entry["fileName"].is_string())
            continue;
        const auto name = entry["fileName"].get<std::string>();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
            name.find_first_of("\r\n") != std::string::npos || name.find('\0') != std::string::npos)
            continue;
        auto path = parent + (parent.back() == '/' ? "" : "/") + name;
        if (ValidRemoteWorkspacePath(path))
            paths.insert(std::move(path));
        if (paths.size() >= 500)
            break;
    }
    for (const auto &path : paths)
        result.push_back(path);
    return result;
}
inline bool WorkspaceProfileMatches(const std::string &stored, const std::string &server) {
    return stored == server || stored.rfind(server + "/project:", 0) == 0;
}
inline std::string StoredWorkspace(const Json &record) {
    if (record.contains("cwd") && record["cwd"].is_string() && ValidWorkspacePath(record["cwd"].get<std::string>()))
        return record["cwd"].get<std::string>();
    const auto profile = record.value("profile", std::string());
    const auto at = profile.find("/project:");
    return at == std::string::npos ? std::string() : profile.substr(at + 9);
}
struct StoredWorkspaceBinding {
    bool hasExplicitMode = false;
    bool isLocal = false;
    bool localReadOnly = false;
    std::string localPath;
    std::string remotePath;
    std::string remoteSessionId;
};
inline StoredWorkspaceBinding ParseStoredWorkspaceBinding(const Json &record) {
    StoredWorkspaceBinding out;
    if (record.contains("workspaceIsLocal") && record["workspaceIsLocal"].is_boolean()) {
        out.hasExplicitMode = true;
        out.isLocal = record["workspaceIsLocal"].get<bool>();
    }
    if (record.contains("localReadOnly") && record["localReadOnly"].is_boolean()) {
        out.localReadOnly = record["localReadOnly"].get<bool>();
    }
    if (record.contains("localWorkspace") && record["localWorkspace"].is_string()) {
        const auto p = record["localWorkspace"].get<std::string>();
        if (ValidLocalWorkspacePath(p))
            out.localPath = NormalizeLocalWorkspacePathUtf8(p);
    }
    if (record.contains("remoteWorkspace") && record["remoteWorkspace"].is_string()) {
        const auto p = record["remoteWorkspace"].get<std::string>();
        if (ValidRemoteWorkspacePath(p))
            out.remotePath = p;
    }
    if (record.contains("remoteSshSessionId") && record["remoteSshSessionId"].is_string()) {
        out.remoteSessionId = record["remoteSshSessionId"].get<std::string>();
    }
    const auto legacy = StoredWorkspace(record);
    if (!legacy.empty()) {
        if (ValidLocalWorkspacePath(legacy)) {
            if (!out.hasExplicitMode) {
                out.hasExplicitMode = true;
                out.isLocal = true;
            }
            if (out.localPath.empty())
                out.localPath = NormalizeLocalWorkspacePathUtf8(legacy);
        } else if (ValidRemoteWorkspacePath(legacy)) {
            if (out.remotePath.empty())
                out.remotePath = legacy;
        }
    }
    return out;
}
inline std::string WorkspaceTargetIdentity(const Json &session) {
    const auto kind = session.value("kind", std::string());
    if (kind == "cmd")
        return Json{{"kind", kind},
                    {"instance", session.value("instance", std::string())},
                    {"id", session.value("id", std::string())}}
            .dump();
    return Json{{"kind", kind},
                {"name", session.value("name", std::string())},
                {"host", session.value("remoteIp", std::string())},
                {"port", session.value("remotePort", 0)},
                {"user", session.value("username", std::string())},
                {"com", session.value("com", std::string())}}
        .dump();
}
} // namespace serialctl::ai
