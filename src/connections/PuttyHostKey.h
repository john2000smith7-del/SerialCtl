#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace serialctl {

enum class HostKeyReason { FirstConnection, Changed, Reverify };
struct HostKeyChallenge {
    std::wstring host;
    std::uint16_t port = 22;
    std::wstring previousFingerprint, fingerprint;
    HostKeyReason reason = HostKeyReason::FirstConnection;
};
using HostKeyConfirmation = std::function<bool(const HostKeyChallenge&)>;

// Reads only SerialCtl's own identity record. Never clears another SSH client's cache.
std::wstring TrustedPuttyHostKey(const std::wstring& host, std::uint16_t port);
bool IsPuttyHostKeyFailure(const std::wstring& message);
bool IsPuttyAuthenticationFailure(const std::wstring& message);

// Resolves and verifies a PuTTY host key without ever leaving an interactive
// confirmation prompt connected to the terminal session. The key is cached for
// this process and supplied explicitly to plink/psftp.
bool ResolvePuttyHostKey(const std::wstring& executable, const std::wstring& host,
    std::uint16_t port, const std::wstring& username, const std::wstring& password,
    std::wstring& hostKey, std::wstring& error,
    const std::atomic_bool* cancel = nullptr,
    const HostKeyConfirmation& confirm = {}, bool reverify = false,
    std::uint64_t deadline = 0);

} // namespace serialctl
