#pragma once
#include <windows.h>
#include <wincred.h>
#include <limits>
#include <string>

namespace serialctl {
// Ordinary SSH credentials are separate from both connection history and AI.
// Length prefixes keep host/user delimiters from aliasing another account.
inline std::wstring SshCredentialTarget(std::wstring host, unsigned port, const std::wstring& user) {
    for (auto& c : host) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return L"SerialCtl/SSH/" + std::to_wstring(host.size()) + L":" + host + L"/" +
           std::to_wstring(port) + L"/" + std::to_wstring(user.size()) + L":" + user;
}
inline void ClearSshSecret(std::wstring& password) {
    if (!password.empty()) SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    password.clear();
}
enum class SshCredentialResult { Missing, Loaded, Error };
inline SshCredentialResult ReadSshCredential(const std::wstring& target, std::wstring& password, DWORD& error) {
    ClearSshSecret(password);
    PCREDENTIALW value = nullptr;
    error = ERROR_SUCCESS;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &value)) {
        error = GetLastError();
        return error == ERROR_NOT_FOUND ? SshCredentialResult::Missing : SshCredentialResult::Error;
    }
    struct Owner {
        PCREDENTIALW value;
        ~Owner() {if(value->CredentialBlobSize&&value->CredentialBlob)SecureZeroMemory(value->CredentialBlob,value->CredentialBlobSize);CredFree(value);}
    } owner{value};
    const bool valid = value->CredentialBlobSize % sizeof(wchar_t) == 0 &&
                       value->CredentialBlobSize <= CRED_MAX_CREDENTIAL_BLOB_SIZE &&
                       (!value->CredentialBlobSize || value->CredentialBlob);
    if (valid && value->CredentialBlobSize)
        password.assign(reinterpret_cast<const wchar_t*>(value->CredentialBlob), value->CredentialBlobSize / sizeof(wchar_t));
    if (!valid) { error = ERROR_INVALID_DATA; return SshCredentialResult::Error; }
    return SshCredentialResult::Loaded;
}
inline bool SaveSshCredential(const std::wstring& target, const std::wstring& user,
                              const std::wstring& password, DWORD& error) {
    if (password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE / sizeof(wchar_t)) {
        error = ERROR_BAD_LENGTH; return false;
    }
    CREDENTIALW value{};
    value.Type = CRED_TYPE_GENERIC;
    value.TargetName = const_cast<wchar_t*>(target.c_str());
    value.UserName = const_cast<wchar_t*>(user.c_str());
    value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    value.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
    value.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(password.data()));
    if (CredWriteW(&value, 0)) { error = ERROR_SUCCESS; return true; }
    error = GetLastError(); return false;
}
inline bool ForgetSshCredential(const std::wstring& target, DWORD& error) {
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) { error = ERROR_SUCCESS; return true; }
    error = GetLastError();
    return error == ERROR_NOT_FOUND;
}
} // namespace serialctl
