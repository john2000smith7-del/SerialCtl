#pragma once
#include <cwctype>
#include <string>

namespace serialctl {
// A UI employee number is never stored back into the transport's port field.
inline bool PlatformSshPort(const std::wstring& employee, unsigned& port) {
    if(employee.empty() || employee.size()>12) return false;
    unsigned value=0;
    for(const auto c:employee) {
        if(c<L'0' || c>L'9') return false;
        value=value*10+static_cast<unsigned>(c-L'0');
        if(value>55535) return false;
    }
    if(!value) return false;
    port=10000+value;
    return true;
}
inline std::wstring PlatformConnectionFailure(const std::wstring& error) {
    std::wstring lower=error;
    for(auto& c:lower)c=static_cast<wchar_t>(towlower(c));
    if(lower.find(L"refused")!=std::wstring::npos || lower.find(L"timed out")!=std::wstring::npos ||
       lower.find(L"timeout")!=std::wstring::npos || lower.find(L"unreachable")!=std::wstring::npos ||
       lower.find(L"拒绝连接")!=std::wstring::npos || lower.find(L"超时")!=std::wstring::npos)
        return error+L"\nSSH 服务未连通，请检查一体化平台网站 IP 地址和员工工号。若尚未开通 SSH 访问，请联系一体化平台负责人开通对应端口或访问权限。";
    return error;
}
} // namespace serialctl
