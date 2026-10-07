#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace serialctl {

// Resolves and verifies a PuTTY host key without ever leaving an interactive
// confirmation prompt connected to the terminal session. The key is cached for
// this process and supplied explicitly to plink/psftp.
bool ResolvePuttyHostKey(const std::wstring& executable, const std::wstring& host,
    std::uint16_t port, const std::wstring& username, const std::wstring& password,
    std::wstring& hostKey, std::wstring& error,
    const std::atomic_bool* cancel = nullptr);

} // namespace serialctl
