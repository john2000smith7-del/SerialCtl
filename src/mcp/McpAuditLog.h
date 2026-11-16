#pragma once
#include "json.hpp"
#include "McpSecurity.h"
#include <deque>
#include <mutex>
#include <string>

namespace serialctl::mcp {

using Json = nlohmann::json;

struct McpAuditEntry {
    std::string timestamp;
    std::string clientIp;
    std::string toolName;
    std::string sessionId;
    int resultCode = 0;
    std::string errorMessage;
    Json arguments;
};

class McpAuditLog {
public:
    static McpAuditLog& Instance() {
        static McpAuditLog s_instance;
        return s_instance;
    }

    void Record(const std::string& timestamp, const std::string& clientIp,
                const std::string& toolName, const std::string& sessionId,
                int resultCode, const std::string& errorMessage, const Json& arguments) {
        std::lock_guard<std::mutex> lock(mutex_);
        McpAuditEntry entry;
        entry.timestamp = timestamp;
        entry.clientIp = clientIp;
        entry.toolName = toolName;
        entry.sessionId = sessionId;
        entry.resultCode = resultCode;
        entry.errorMessage = errorMessage;
        entry.arguments = McpSecurity::SanitizeJson(arguments);

        entries_.push_back(std::move(entry));
        while (entries_.size() > kMaxEntries) {
            entries_.pop_front();
        }
    }

    Json ToJson() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Json arr = Json::array();
        for (const auto& item : entries_) {
            arr.push_back({
                {"timestamp", item.timestamp},
                {"clientIp", item.clientIp},
                {"toolName", item.toolName},
                {"sessionId", item.sessionId},
                {"resultCode", item.resultCode},
                {"errorMessage", item.errorMessage},
                {"arguments", item.arguments}
            });
        }
        return arr;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    McpAuditLog() = default;
    static constexpr size_t kMaxEntries = 300;
    mutable std::mutex mutex_;
    std::deque<McpAuditEntry> entries_;
};

} // namespace serialctl::mcp
