#pragma once
#include "Connection.h"
#include <map>
#include <stdexcept>
#include <utility>
#include <atomic>
namespace serialctl::ws {
inline constexpr size_t MaxMessage = 128 * 1024;
// Hard limit for finishing a started frame or fragmented message. Idle time between
// messages is deliberately not limited by this value.
inline constexpr DWORD FrameTimeoutMs = 10000;
inline constexpr const char *Protocol = "serialctl.v1";
inline constexpr const char *Path = "/serialctl";
struct Error : std::runtime_error {
    unsigned code;
    std::string instance;
    Error(unsigned c, const char *text, std::string serverInstance = {})
        : std::runtime_error(text), code(c), instance(std::move(serverInstance)) {}
};
std::string Base64(const Bytes &);
Bytes Unbase64(const std::string &);
std::string RandomHex(size_t bytes = 16);
std::string AcceptKey(const std::string &);
void FinishSend(SOCKET, DWORD grace = 250);
bool SendAll(SOCKET, const void *, size_t, ULONGLONG deadline = 0, const std::atomic_bool *cancel = nullptr);
void SocketOptions(SOCKET, DWORD receiveTimeout = 0);
struct Header {
    std::string first;
    std::map<std::string, std::string> fields;
};
// Reads only the HTTP header, retaining any coalesced WebSocket bytes.
Header ReadHeader(SOCKET, Bytes &, DWORD timeout = 1500, const std::atomic_bool* cancel = nullptr);
std::string Lower(std::string);
bool Token(const std::string &, const std::string &);
bool Utf8(const Bytes &);
Bytes ClosePayload(unsigned, const std::string &);
class Stream {
  public:
    Stream(SOCKET socket, bool client, Bytes buffered = {}, HANDLE readCancel = nullptr, DWORD timeout = INFINITE)
        : socket_(socket), client_(client), buffered_(std::move(buffered)), readCancel_(readCancel),
          readTimeout_(timeout) {
        // Synchronous callers historically relied on SO_RCVTIMEO. The readiness
        // wait must honor that socket budget too, rather than waiting forever
        // before recv gets a chance to enforce it.
        if (!readCancel_ && timeout == INFINITE) {
            DWORD receiveTimeout = 0; int size = sizeof(receiveTimeout);
            if (getsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&receiveTimeout), &size) == 0 && receiveTimeout)
                readTimeout_ = receiveTimeout;
        }
    }
    ~Stream() { if (readEvent_ != WSA_INVALID_EVENT) WSACloseEvent(readEvent_); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    void CancelToken(const std::atomic_bool* cancel) { cancelToken_ = cancel; }
    // Caller serializes sends; server has exactly one sender per socket.
    bool Send(unsigned opcode, const Bytes &, bool final = true);
    struct Message {
        unsigned opcode;
        Bytes data;
    };
    void Timeout(DWORD timeout) {
        readTimeout_ = timeout;
    }
    void FrameTimeout(DWORD timeout) {
        frameTimeout_ = timeout;
    }
    void Deadline(ULONGLONG deadline) { absoluteDeadline_ = deadline; }
    Message Receive(); // control frames returned, fragments reassembled, strict RFC 6455
  private:
    void Exact(void *, size_t, bool idleWait);
    SOCKET socket_;
    bool client_;
    Bytes buffered_, fragment_;
    size_t bufferedOffset_ = 0;
    WSAEVENT readEvent_ = WSA_INVALID_EVENT;
    const std::atomic_bool* cancelToken_ = nullptr;
    unsigned fragmentOpcode_ = 0;
    HANDLE readCancel_ = nullptr;
    DWORD readTimeout_ = INFINITE;
    DWORD frameTimeout_ = INFINITE;
    ULONGLONG frameDeadline_ = 0;
    ULONGLONG absoluteDeadline_ = 0;
};
// Client handshake validates accept key and subprotocol. Socket ownership stays with caller.
Bytes UpgradeClient(SOCKET, const std::wstring &host, unsigned port, const std::atomic_bool* cancel = nullptr,
                    DWORD timeout = 1500);
} // namespace serialctl::ws
