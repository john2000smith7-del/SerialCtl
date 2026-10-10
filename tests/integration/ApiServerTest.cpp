#include "ApiServer.h"
#include <atomic>
#include <iostream>
#include <thread>
using namespace serialctl;
std::string Request(unsigned port, const std::string &token, const std::string &path,
                    const std::string &body = "")
{
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    if (connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
        return "Connect failed";
    DWORD timeout = 3000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
               sizeof(timeout));
    std::string request = (body.empty() ? "GET " : "POST ") + path +
                          " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + token +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send(socket, request.data(), static_cast<int>(request.size()), 0);
    std::string output;
    char buffer[4096];
    int count;
    while ((count = recv(socket, buffer, sizeof(buffer), 0)) > 0)
        output.append(buffer, count);
    closesocket(socket);
    return output;
}
int main()
{
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    ApiServer server;
    std::wstring error;
    std::atomic<int> writes{0};
    auto handler = [&](const std::string &method, const std::string &path,
                       const Json &body) -> Json {
        if (path == "/api/v1/resources")
            return {{"resources", Json::array({Json{{"id", "session-1"}}, Json{{"id", "session-2"}},
                                               Json{{"id", "power-1"}}})}};
        if (method == "POST")
            ++writes;
        return {{"ok", true}};
    };
    if (!server.Start(handler, {{"session-1", {3, 7}}, {"power-1", {5, 1}}}, error))
    {
        std::wcerr << error;
        return 1;
    }
    auto token = server.Token();
    unsigned port = server.Port();
    if (Request(port, "wrong", "/api/v1/resources").find("401") == std::string::npos)
        return 2;
    auto list = Request(port, token, "/api/v1/resources");
    if (list.find("session-1") == std::string::npos || list.find("session-2") != std::string::npos)
        return 3;
    if (Request(port, token, "/api/v1/sessions/session-2/input", "{\"data\":\"QQ==\"}")
            .find("403") == std::string::npos)
        return 4;
    if (Request(port, token, "/api/v1/power-supplies/power-1/channels/output",
                "{\"channels\":[2],\"enabled\":true}")
            .find("403") == std::string::npos)
        return 5;
    if (Request(port, token, "/api/v1/power-supplies/power-1/channels/output",
                "{\"channels\":[4294967297],\"enabled\":true}")
            .find("403") == std::string::npos)
        return 22;
    if (writes != 0)
        return 6;
    for (int i = 0; i < 64; ++i)
    {
        auto response = Request(port, token, "/api/v1/resources");
        if (response.find("200") == std::string::npos)
        {
            std::cerr << "Sequential client " << i << " failed\n";
            return 7;
        }
    }
    if (Request(port, token, "/api/v1/sessions/session-1/input", "{\"data\":\"QQ==\"}")
                .find("200") == std::string::npos ||
        writes != 1)
        return 8;
    server.Publish("session-1", Bytes{0, 1, 2, 255});
    auto events = server.Events("session-1", 0);
    if (Decode64(events["events"][0]["data"]) != Bytes({0, 1, 2, 255}))
        return 9;
    for (int i = 0; i < 1100; ++i)
        server.Publish("session-1", Bytes{1});
    if (!server.Events("session-1", 1)["gap"].get<bool>())
        return 10;
    // Verify an actual RFC 6455 stream, masked binary input and exact binary output.
    SOCKET stream = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    if (connect(stream, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
        return 12;
    DWORD timeout = 3000;
    setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
               sizeof(timeout));
    std::string upgrade = "GET /api/v1/sessions/session-1/stream HTTP/1.1\r\nHost: "
                          "localhost\r\nAuthorization: Bearer " +
                          token +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
                          "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    send(stream, upgrade.data(), static_cast<int>(upgrade.size()), 0);
    std::string handshake;
    char ch = 0;
    while (handshake.size() < 4096 && handshake.find("\r\n\r\n") == std::string::npos &&
           recv(stream, &ch, 1, 0) == 1)
        handshake += ch;
    if (handshake.find("101 Switching Protocols") == std::string::npos ||
        handshake.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == std::string::npos)
        return 13;
    auto frame = [&]() {
        unsigned char head[2]{};
        for (int i = 0; i < 2; ++i)
            if (recv(stream, reinterpret_cast<char *>(&head[i]), 1, 0) != 1)
                return std::pair<unsigned, Bytes>{0, {}};
        size_t length = head[1] & 127;
        if (length > 125)
            return std::pair<unsigned, Bytes>{0, {}};
        Bytes bytes(length);
        for (size_t i = 0; i < length; ++i)
            if (recv(stream, reinterpret_cast<char *>(&bytes[i]), 1, 0) != 1)
                return std::pair<unsigned, Bytes>{0, {}};
        return std::pair<unsigned, Bytes>{head[0] & 15, bytes};
    };
    // A ping establishes that the server has entered its live-stream loop.
    const unsigned char ping[] = {0x89, 0x80, 1, 2, 3, 4};
    send(stream, reinterpret_cast<const char *>(ping), sizeof(ping), 0);
    if (frame().first != 10)
        return 14;
    const unsigned char input[] = {0x82,
                                   0x82,
                                   1,
                                   2,
                                   3,
                                   4,
                                   static_cast<unsigned char>('A' ^ 1),
                                   static_cast<unsigned char>('B' ^ 2)};
    send(stream, reinterpret_cast<const char *>(input), sizeof(input), 0);
    if (frame().first != 1 || writes != 2)
        return 15;
    server.Publish("session-1", Bytes{0, 255, 13, 10});
    auto binary = frame();
    if (binary.first != 2 || binary.second != Bytes({0, 255, 13, 10}))
        return 16;
    closesocket(stream);
    // Updating grants rotates the token and terminates established clients.
    if (!server.Start(handler, {{"session-1", {1, 7}}, {"power-1", {9, 1}}}, error))
        return 17;
    if (Request(server.Port(), token, "/api/v1/resources").find("401") == std::string::npos)
        return 18;
    token = server.Token();
    port = server.Port();
    if (Request(port, token, "/api/v1/sessions/session-1/input", "{\"data\":\"QQ==\"}")
            .find("403") == std::string::npos)
        return 19;
    if (Request(port, token, "/api/v1/power-supplies/power-1/channels/protection",
                "{\"channels\":[1],\"enabled\":true}")
            .find("200") == std::string::npos)
        return 20;
    if (Request(port, token, "/api/v1/power-supplies/power-1/channels/output",
                "{\"channels\":[1],\"enabled\":true}")
            .find("403") == std::string::npos)
        return 21;
    server.Stop();
    if (server.Running() || !server.Token().empty())
        return 11;
    WSACleanup();
    std::cout << "API authorization, client recycling and bounded events passed\n";
    return 0;
}
