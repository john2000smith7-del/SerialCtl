#pragma once
#include "Connection.h"
#include <stdexcept>
#include <map>
namespace serialctl::ws {
inline constexpr size_t MaxMessage = 128 * 1024;
inline constexpr const char* Protocol = "serialctl.v1";
inline constexpr const char* Path = "/serialctl";
struct Error : std::runtime_error {
    unsigned code;
    Error(unsigned c, const char* text) : std::runtime_error(text), code(c) {}
};
std::string Base64(const Bytes&);
Bytes Unbase64(const std::string&);
std::string RandomHex(size_t bytes = 16);
std::string AcceptKey(const std::string&);
bool SendAll(SOCKET, const void*, size_t);
void SocketOptions(SOCKET, DWORD receiveTimeout = 0);
struct Header {
    std::string first;
    std::map<std::string, std::string> fields;
};
// Reads only the HTTP header, retaining any coalesced WebSocket bytes.
Header ReadHeader(SOCKET, Bytes&);
std::string Lower(std::string);
bool Token(const std::string&, const std::string&);
bool Utf8(const Bytes&);
Bytes ClosePayload(unsigned, const std::string&);
class Stream {
public:
    Stream(SOCKET socket, bool client, Bytes buffered = {})
        : socket_(socket), client_(client), buffered_(std::move(buffered)) {}
    // Caller serializes sends; server has exactly one sender per socket.
    bool Send(unsigned opcode, const Bytes&, bool final = true);
    struct Message { unsigned opcode; Bytes data; };
    Message Receive(); // control frames returned, fragments reassembled, strict RFC 6455
private:
    void Exact(void*, size_t);
    SOCKET socket_;
    bool client_;
    Bytes buffered_, fragment_;
    unsigned fragmentOpcode_ = 0;
};
// Client handshake validates accept key and subprotocol. Socket ownership stays with caller.
Bytes UpgradeClient(SOCKET, const std::wstring& host, unsigned port);
} // namespace serialctl::ws
