#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace serialctl {

using HostKeyConfirmation = std::function<bool(const std::wstring&, std::uint16_t, const std::wstring&)>;

// Resolves and verifies a PuTTY host key without ever leaving an interactive
// confirmation prompt connected to the terminal session. The key is cached for
// this process and supplied explicitly to plink/psftp.
bool ResolvePuttyHostKey(const std::wstring& executable, const std::wstring& host,
    std::uint16_t port, const std::wstring& username, const std::wstring& password,
    std::wstring& hostKey, std::wstring& error,
    const std::atomic_bool* cancel = nullptr,
    const HostKeyConfirmation& confirm = {});

} // namespace serialctl
