#include "WebSocket.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <array>
#include <sstream>
#include <wincrypt.h>
namespace serialctl::ws {
std::string Base64(const Bytes &bytes) {
    static const char *d = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned n = unsigned(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            n |= unsigned(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size())
            n |= bytes[i + 2];
        s += d[(n >> 18) & 63];
        s += d[(n >> 12) & 63];
        s += i + 1 < bytes.size() ? d[(n >> 6) & 63] : '=';
        s += i + 2 < bytes.size() ? d[n & 63] : '=';
    }
    return s;
}
Bytes Unbase64(const std::string &text) {
    if (text.size() % 4 || text.size() > MaxMessage)
        throw Error(1007, "INVALID_BASE64");
    const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes out;
    for (size_t i = 0; i < text.size(); i += 4) {
        unsigned n = 0;
        int padding = 0;
        for (size_t j = 0; j < 4; ++j) {
            auto pos = digits.find(text[i + j]);
            if (text[i + j] == '=') {
                if (j < 2 || i + 4 != text.size())
                    throw Error(1007, "INVALID_BASE64");
                ++padding;
                n <<= 6;
            } else {
                if (pos == std::string::npos || padding)
                    throw Error(1007, "INVALID_BASE64");
                n = (n << 6) | unsigned(pos);
            }
        }
        if ((padding == 2 && (n & 0xffff)) || (padding == 1 && (n & 0xff)))
            throw Error(1007, "NONCANONICAL_BASE64");
        out.push_back(std::uint8_t(n >> 16));
        if (padding < 2)
            out.push_back(std::uint8_t(n >> 8));
        if (padding < 1)
            out.push_back(std::uint8_t(n));
    }
    return out;
}
static Bytes RandomBytes(size_t size) {
    HCRYPTPROV p = 0;
    Bytes b(size);
    if (!CryptAcquireContextW(&p, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        throw Error(1011, "RANDOM_UNAVAILABLE");
    bool ok = CryptGenRandom(p, DWORD(size), b.data()) != FALSE;
    CryptReleaseContext(p, 0);
    if (!ok)
        throw Error(1011, "RANDOM_FAILED");
    return b;
}
std::string RandomHex(size_t count) {
    std::string s;
    for (auto b : RandomBytes(count)) {
        s += "0123456789abcdef"[b >> 4];
        s += "0123456789abcdef"[b & 15];
    }
    return s;
}
std::string AcceptKey(const std::string &key) {
    if (Unbase64(key).size() != 16)
        throw Error(1002, "INVALID_KEY");
    const auto input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV p = 0;
    HCRYPTHASH h = 0;
    Bytes digest(20);
    DWORD n = 20;
    if (!CryptAcquireContextW(&p, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        throw Error(1011, "SHA1_UNAVAILABLE");
    bool ok = CryptCreateHash(p, CALG_SHA1, 0, 0, &h) != FALSE;
    if (ok)
        ok = CryptHashData(h, reinterpret_cast<const BYTE *>(input.data()), DWORD(input.size()), 0) &&
             CryptGetHashParam(h, HP_HASHVAL, digest.data(), &n, 0);
    if (h)
        CryptDestroyHash(h);
    CryptReleaseContext(p, 0);
    if (!ok)
        throw Error(1011, "SHA1_FAILED");
    return Base64(digest);
}
bool SendAll(SOCKET s, const void *data, size_t n) {
    auto p = static_cast<const char *>(data);
    while (n) {
        int k = send(s, p, int(std::min<size_t>(n, 65536)), 0);
        if (k <= 0)
            return false;
        p += k;
        n -= k;
    }
    return true;
}
void FinishSend(SOCKET socket) {
    // Windows resets TCP when closing with unread bytes. Give the already sent
    // protocol/HTTP error a bounded graceful delivery window, without executing
    // any further business input. A peer may be hostile or never acknowledge.
    shutdown(socket, SD_SEND);
    const auto deadline = GetTickCount64() + 250;
    DWORD timeout = 100;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    char discard[2048];
    size_t drained = 0;
    while (drained < 8192 && GetTickCount64() < deadline) {
        int n = recv(socket, discard, sizeof(discard), 0);
        if (n <= 0)
            break;
        drained += size_t(n);
    }
}
void SocketOptions(SOCKET s, DWORD timeout) {
    BOOL yes = TRUE;
    DWORD sendTimeout = 3000;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&yes), sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&yes), sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sendTimeout), sizeof(sendTimeout));
}
std::string Lower(std::string s) {
    for (char &c : s)
        c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}
bool Token(const std::string &s, const std::string &token) {
    std::istringstream in(Lower(s));
    std::string part;
    while (std::getline(in, part, ',')) {
        auto a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
        if (a != std::string::npos && part.substr(a, b - a + 1) == Lower(token))
            return true;
    }
    return false;
}
Header ReadHeader(SOCKET s, Bytes &tail) {
    std::string header;
    std::array<char, 2048> b{};
    for (;;) {
        auto end = header.find("\r\n\r\n");
        if (end != std::string::npos) {
            tail.assign(header.begin() + end + 4, header.end());
            header.resize(end);
            break;
        }
        if (header.size() > 8192)
            throw Error(1002, "HEADER_TOO_LARGE");
        int n = recv(s, b.data(), int(b.size()), 0);
        if (n <= 0)
            throw Error(1002, "HANDSHAKE_CLOSED_OR_TIMEOUT");
        header.append(b.data(), n);
        // Telnet / old clients get an upgrade error immediately, before any device routing.
        if (header.size() >= 4 && header.rfind("GET ", 0) != 0 && header.rfind("HTTP", 0) != 0)
            throw Error(1002, "UPGRADE_REQUIRED");
    }
    if (header.size() > 8192)
        throw Error(1002, "HEADER_TOO_LARGE");
    std::istringstream in(header);
    Header h;
    std::getline(in, h.first);
    if (!h.first.empty() && h.first.back() == '\r')
        h.first.pop_back();
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto c = line.find(':');
        if (c == std::string::npos)
            throw Error(1002, "BAD_HEADER");
        auto key = Lower(line.substr(0, c));
        auto val = line.substr(c + 1);
        auto a = val.find_first_not_of(" \t");
        val = a == std::string::npos ? "" : val.substr(a);
        if (h.fields.count(key))
            throw Error(1002, "DUPLICATE_HEADER");
        h.fields[key] = val;
    }
    return h;
}
bool Utf8(const Bytes &bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        unsigned c = bytes[i++], n = 0, min = 0;
        if (c < 128)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            c &= 31;
            n = 1;
            min = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            c &= 15;
            n = 2;
            min = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            c &= 7;
            n = 3;
            min = 65536;
        } else
            return false;
        if (i + n > bytes.size())
            return false;
        while (n--) {
            if ((bytes[i] & 192) != 128)
                return false;
            c = (c << 6) | (bytes[i++] & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
Bytes ClosePayload(unsigned code, const std::string &reason) {
    Bytes b{std::uint8_t(code >> 8), std::uint8_t(code)};
    b.insert(b.end(), reason.begin(), reason.begin() + std::min<size_t>(reason.size(), 123));
    return b;
}
void Stream::Exact(void *data, size_t n) {
    if (readCancel_ && WaitForSingleObject(readCancel_, 0) == WAIT_OBJECT_0)
        throw Error(1006, "CANCELED");
    auto p = static_cast<std::uint8_t *>(data);
    size_t k = std::min(n, buffered_.size());
    std::copy_n(buffered_.begin(), k, p);
    buffered_.erase(buffered_.begin(), buffered_.begin() + k);
    p += k;
    n -= k;
    while (n) {
        int got = 0;
        if (!readCancel_)
            got = recv(socket_, reinterpret_cast<char *>(p), int(std::min<size_t>(n, 65536)), 0);
        else {
            // A cancel event wakes overlapped socket reads on Win7 without closing
            // and potentially reusing the SOCKET while the reader still owns it.
            WSAOVERLAPPED io{};
            io.hEvent = WSACreateEvent();
            if (io.hEvent == WSA_INVALID_EVENT)
                throw Error(1011, "READ_EVENT_FAILED");
            WSABUF buffer{static_cast<ULONG>(std::min<size_t>(n, 65536)), reinterpret_cast<char *>(p)};
            DWORD received = 0, flags = 0;
            bool ok = false;
            if (WSARecv(socket_, &buffer, 1, &received, &flags, &io, nullptr) == 0)
                ok = true;
            else if (WSAGetLastError() == WSA_IO_PENDING) {
                HANDLE waits[] = {readCancel_, io.hEvent};
                auto signal = WaitForMultipleObjects(2, waits, FALSE, readTimeout_);
                if (signal == WAIT_OBJECT_0 + 1)
                    ok = WSAGetOverlappedResult(socket_, &io, &received, FALSE, &flags) != FALSE;
                else {
                    CancelIoEx(reinterpret_cast<HANDLE>(socket_), &io);
                    // The OVERLAPPED stack storage must outlive cancellation completion.
                    WSAGetOverlappedResult(socket_, &io, &received, TRUE, &flags);
                }
            }
            WSACloseEvent(io.hEvent);
            if (!ok)
                throw Error(1006, "CANCELED_OR_DISCONNECTED_OR_TIMEOUT");
            got = static_cast<int>(received);
        }
        if (got <= 0)
            throw Error(1006, "DISCONNECTED");
        p += got;
        n -= got;
    }
}
bool Stream::Send(unsigned op, const Bytes &bytes, bool fin) {
    Bytes frame{std::uint8_t((fin ? 128 : 0) | op)};
    const unsigned mask = client_ ? 128 : 0;
    if (bytes.size() < 126)
        frame.push_back(std::uint8_t(mask | bytes.size()));
    else if (bytes.size() <= 65535) {
        frame.push_back(std::uint8_t(mask | 126));
        frame.push_back(std::uint8_t(bytes.size() >> 8));
        frame.push_back(std::uint8_t(bytes.size()));
    } else {
        frame.push_back(std::uint8_t(mask | 127));
        for (int i = 7; i >= 0; --i)
            frame.push_back(std::uint8_t(std::uint64_t(bytes.size()) >> (8 * i)));
    }
    Bytes masking;
    if (client_) {
        masking = RandomBytes(4);
        frame.insert(frame.end(), masking.begin(), masking.end());
    }
    for (size_t i = 0; i < bytes.size(); ++i)
        frame.push_back(bytes[i] ^ (client_ ? masking[i % 4] : 0));
    return SendAll(socket_, frame.data(), frame.size());
}
Stream::Message Stream::Receive() {
    for (;;) {
        std::uint8_t h[2];
        Exact(h, 2);
        bool fin = (h[0] & 128) != 0, mask = (h[1] & 128) != 0;
        unsigned op = h[0] & 15;
        if ((h[0] & 112) || mask == client_ || !(op == 0 || op == 1 || op == 2 || op == 8 || op == 9 || op == 10))
            throw Error(1002, "INVALID_FRAME");
        std::uint64_t n = h[1] & 127;
        unsigned marker = unsigned(n);
        if (n == 126) {
            std::uint8_t x[2];
            Exact(x, 2);
            n = (unsigned(x[0]) << 8) | x[1];
            if (n < 126)
                throw Error(1002, "NONCANONICAL_LENGTH");
        } else if (n == 127) {
            std::uint8_t x[8];
            Exact(x, 8);
            if (x[0] & 128)
                throw Error(1002, "INVALID_LENGTH");
            n = 0;
            for (auto c : x)
                n = (n << 8) | c;
            if (n <= 65535)
                throw Error(1002, "NONCANONICAL_LENGTH");
        }
        if ((op & 8) && (!fin || marker >= 126))
            throw Error(1002, "INVALID_CONTROL");
        if (n > MaxMessage || (!(op & 8) && fragment_.size() + n > MaxMessage))
            throw Error(1009, "MESSAGE_TOO_LARGE");
        std::uint8_t key[4]{};
        if (mask)
            Exact(key, 4);
        Bytes b(static_cast<size_t>(n));
        Exact(b.data(), b.size());
        if (mask)
            for (size_t i = 0; i < b.size(); ++i)
                b[i] ^= key[i % 4];
        if (op == 8) {
            if (b.size() == 1)
                throw Error(1002, "INVALID_CLOSE");
            if (b.size() >= 2) {
                unsigned code = (unsigned(b[0]) << 8) | b[1];
                if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015 ||
                    (code >= 1016 && code < 3000))
                    throw Error(1002, "INVALID_CLOSE_CODE");
                if (!Utf8(Bytes(b.begin() + 2, b.end())))
                    throw Error(1007, "INVALID_CLOSE_UTF8");
            }
            return {op, std::move(b)};
        }
        if (op & 8)
            return {op, std::move(b)};
        if (op == 0) {
            if (!fragmentOpcode_)
                throw Error(1002, "UNEXPECTED_CONTINUATION");
            fragment_.insert(fragment_.end(), b.begin(), b.end());
            if (!fin)
                continue;
            op = fragmentOpcode_;
            fragmentOpcode_ = 0;
            b.swap(fragment_);
            fragment_.clear();
        } else {
            if (fragmentOpcode_)
                throw Error(1002, "INTERLEAVED_MESSAGE");
            if (!fin) {
                fragmentOpcode_ = op;
                fragment_ = std::move(b);
                continue;
            }
        }
        if (op == 1 && !Utf8(b))
            throw Error(1007, "INVALID_UTF8");
        return {op, std::move(b)};
    }
}
Bytes UpgradeClient(SOCKET s, const std::wstring &host, unsigned port) {
    SocketOptions(s, 1500);
    auto key = Base64(RandomBytes(16));
    auto request =
        "GET " + std::string(Path) + " HTTP/1.1\r\nHost: " + WideToMultiByte(host, CP_UTF8) + ":" +
        std::to_string(port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: " +
        Protocol + "\r\nSec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!SendAll(s, request.data(), request.size()))
        throw Error(1006, "HANDSHAKE_SEND_FAILED");
    Bytes tail;
    auto h = ReadHeader(s, tail);
    if (h.first.rfind("HTTP/1.1 503", 0) == 0)
        throw Error(1013, "CLIENT_LIMIT");
    if (h.first != "HTTP/1.1 101 Switching Protocols" || Lower(h.fields["upgrade"]) != "websocket" ||
        !Token(h.fields["connection"], "upgrade") || h.fields["sec-websocket-accept"] != AcceptKey(key) ||
        h.fields["sec-websocket-protocol"] != Protocol)
        throw Error(1002, "UPGRADE_OR_PROTOCOL_MISMATCH");
    SocketOptions(s, 3000);
    return tail;
}
} // namespace serialctl::ws
