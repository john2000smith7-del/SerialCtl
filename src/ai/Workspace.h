#pragma once
#include "AiProtocol.h"
#include <set>

namespace serialctl::ai {
inline bool ValidWorkspacePath(const std::string &path) {
    return !path.empty() && path.front() == '/' && path.size() <= 4096 &&
        std::none_of(path.begin(),path.end(),[](unsigned char c){return c<32 || c==127;});
}
inline std::string WorkspaceParent(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}
inline Json WorkspaceDirectories(const std::string &parent, const Json &entries) {
    Json result = Json::array();
    if (!ValidWorkspacePath(parent) || !entries.is_array()) return result;
    std::set<std::string> paths;
    for (const auto &entry : entries) {
        if (!entry.is_object() || !entry.value("isDirectory", false) || !entry.contains("fileName") || !entry["fileName"].is_string()) continue;
        const auto name = entry["fileName"].get<std::string>();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
            name.find_first_of("\r\n") != std::string::npos || name.find('\0') != std::string::npos) continue;
        auto path = parent + (parent.back() == '/' ? "" : "/") + name;
        if (ValidWorkspacePath(path)) paths.insert(std::move(path));
        if (paths.size() >= 500) break;
    }
    for (const auto &path : paths) result.push_back(path);
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
inline std::string WorkspaceTargetIdentity(const Json &session) {
    const auto kind = session.value("kind", std::string());
    if (kind == "cmd") return Json{{"kind", kind}, {"instance", session.value("instance", std::string())},
        {"id", session.value("id", std::string())}}.dump();
    return Json{{"kind", kind}, {"name", session.value("name", std::string())},
        {"host", session.value("remoteIp", std::string())}, {"port", session.value("remotePort", 0)},
        {"user", session.value("username",std::string())},
        {"com", session.value("com", std::string())}}.dump();
}
} // namespace serialctl::ai
