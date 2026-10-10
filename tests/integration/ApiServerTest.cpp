#include "ApiServer.h"
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
    int writes = 0;
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
    server.Stop();
    if (server.Running() || !server.Token().empty())
        return 11;
    WSACleanup();
    std::cout << "API authorization, client recycling and bounded events passed\n";
    return 0;
}
