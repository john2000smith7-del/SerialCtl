#pragma once
#include "json.hpp"
#include "PlatformConnection.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace serialctl {
// Configuration history only: passwords and trust decisions have separate owners.
inline nlohmann::json NormalizeSshHistory(const nlohmann::json& input) {
    auto out=nlohmann::json::array();
    if(!input.is_array())return out;
    for(const auto& row:input) {
        if(out.size()>=50)break;
        if(!row.is_object() || !row.contains("host") || !row["host"].is_string() ||
           !row.contains("username") || !row["username"].is_string() || !row.contains("port") ||
           !row["port"].is_number_unsigned())continue;
        const auto host=row["host"].get<std::string>(), user=row["username"].get<std::string>();
        const auto port=row["port"].get<unsigned>();
        if(host.empty() || host.size()>253 || user.empty() || user.size()>128 || !port || port>65535)continue;
        if (row.contains("employee") && !row["employee"].is_string()) continue;
        const auto employee=row.value("employee",std::string());
        if(employee.size()>12)continue;
        if (!employee.empty()) { unsigned actual=0; if (!PlatformSshPort(std::wstring(employee.begin(),employee.end()),actual) || actual!=port) continue; }
        const auto stamp = row.contains("lastSuccess") && row["lastSuccess"].is_number_unsigned() ? row["lastSuccess"].get<std::uint64_t>() : 0;
        out.push_back({{"host",host},{"username",user},{"port",port},{"employee",employee},{"mode",employee.empty()?"direct":"platform"},{"lastSuccess",stamp}});
    }
    return out;
}
inline nlohmann::json RememberSshConnection(const nlohmann::json& input, nlohmann::json row) {
    auto checked=NormalizeSshHistory(nlohmann::json::array({row}));
    auto out=NormalizeSshHistory(input);
    if(checked.empty())return out;
    row=checked.front();
    const auto key=[](const nlohmann::json& r) {
        auto h=r["host"].get<std::string>();
        std::transform(h.begin(),h.end(),h.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        return h+":"+std::to_string(r["port"].get<unsigned>())+"/"+r["username"].get<std::string>();
    };
    for(auto i=out.begin();i!=out.end();)if(key(*i)==key(row))i=out.erase(i);else ++i;
    out.insert(out.begin(),row);
    if(out.size()>50)out.erase(out.begin()+50,out.end());
    return out;
}
} // namespace serialctl
