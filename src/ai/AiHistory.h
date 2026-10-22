#pragma once
#include "AiProtocol.h"

namespace serialctl::ai {
// V1.0.9 accidentally stored each record as three adjacent [key, value]
// arrays. Read both layouts without treating local storage errors as wire errors.
inline bool ValidHistoryRecord(const Json &record) {
    if (!record.is_object())
        return false;
    for (const auto *field : {"id", "title", "profile"})
        if (!record.contains(field) || !record[field].is_string())
            return false;
    return !record["id"].get_ref<const std::string &>().empty() &&
           !record["profile"].get_ref<const std::string &>().empty();
}
inline Json NormalizeHistory(const Json &stored) {
    Json result = Json::array();
    if (!stored.is_array())
        return result;
    const char *fields[] = {"id", "title", "profile"};
    for (size_t index = 0; index < stored.size() && result.size() < 100; ++index) {
        Json record;
        if (stored[index].is_object())
            record = stored[index];
        else if (index + 2 < stored.size()) {
            record = Json::object();
            bool complete = true;
            for (size_t field = 0; field < 3; ++field) {
                const auto &pair = stored[index + field];
                if (!pair.is_array() || pair.size() != 2 || pair[0] != fields[field] || !pair[1].is_string()) {
                    complete = false;
                    break;
                }
                record[fields[field]] = pair[1];
            }
            if (!complete)
                continue;
            index += 2;
        }
        if (ValidHistoryRecord(record))
            result.push_back(std::move(record));
    }
    return result;
}
inline Json RememberHistory(const Json &stored, const std::string &id, const std::string &title,
                            const std::string &profile) {
    auto entries = NormalizeHistory(stored);
    Json record = {{"id", id}, {"title", title}, {"profile", profile}};
    if (!ValidHistoryRecord(record))
        return entries;
    for (auto it = entries.begin(); it != entries.end();)
        if (it->at("id") == id && it->at("profile") == profile)
            it = entries.erase(it);
        else
            ++it;
    // Pass a Json object explicitly: the initializer-list insert overload
    // would insert three pair arrays and corrupt the next conversation.
    entries.insert(entries.begin(), record);
    if (entries.size() > 100)
        entries.erase(entries.end() - 1);
    return entries;
}
} // namespace serialctl::ai
