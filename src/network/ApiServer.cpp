#include "ApiServer.h"
#include "DiscoveryInfo.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <wincrypt.h>
#include <ws2tcpip.h>
namespace serialctl
{
namespace
{
std::string Random()
{
    HCRYPTPROV provider = 0;
    std::array<BYTE, 32> bytes{};
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        throw std::runtime_error("Random generator unavailable");
    bool ok = CryptGenRandom(provider, static_cast<DWORD>(bytes.size()), bytes.data()) != FALSE;
    CryptReleaseContext(provider, 0);
    if (!ok)
        throw std::runtime_error("Random generator failed");
    std::string value;
    for (auto b : bytes)
    {
        value += "0123456789abcdef"[b >> 4];
        value += "0123456789abcdef"[b & 15];
    }
    return value;
}
bool SendAll(SOCKET socket, const void *data, size_t size)
{
    auto *p = static_cast<const char *>(data);
    while (size)
    {
        int count = send(socket, p, static_cast<int>(std::min<size_t>(size, 65536)), 0);
        if (count <= 0)
            return false;
        p += count;
        size -= count;
    }
    return true;
}
bool ReadAll(SOCKET socket, void *data, size_t size)
{
    auto *p = static_cast<char *>(data);
    while (size)
    {
        int count = recv(socket, p, static_cast<int>(std::min<size_t>(size, 65536)), 0);
        if (count <= 0)
            return false;
        p += count;
        size -= count;
    }
    return true;
}
SOCKET Bind(unsigned first, unsigned last, unsigned &chosen)
{
    for (unsigned port = first; port <= last; ++port)
    {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET)
            break;
        BOOL exclusive = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<u_short>(port));
        if (bind(socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
            listen(socket, 16) == 0)
        {
            chosen = port;
            return socket;
        }
        closesocket(socket);
    }
    return INVALID_SOCKET;
}
std::vector<std::string> Segments(std::string path)
{
    std::vector<std::string> parts;
    size_t start = 1;
    while (start < path.size())
    {
        auto end = path.find('/', start);
        parts.push_back(path.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return parts;
}
std::string Lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
bool EqualToken(const std::string &a, const std::string &b)
{
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    for (size_t i = 0; i < b.size(); ++i)
        diff |= static_cast<unsigned>((i < a.size() ? a[i] : 0) ^ b[i]);
    return diff == 0;
}
bool Frame(SOCKET socket, unsigned opcode, const Bytes &bytes)
{
    Bytes frame{static_cast<std::uint8_t>(0x80 | opcode)};
    if (bytes.size() < 126)
        frame.push_back(static_cast<std::uint8_t>(bytes.size()));
    else if (bytes.size() <= 65535)
    {
        frame.push_back(126);
        frame.push_back(static_cast<std::uint8_t>(bytes.size() >> 8));
        frame.push_back(static_cast<std::uint8_t>(bytes.size()));
    }
    else
    {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(
                static_cast<std::uint8_t>(static_cast<std::uint64_t>(bytes.size()) >> (i * 8)));
    }
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    return SendAll(socket, frame.data(), frame.size());
}
} // namespace
std::string Encode64(const Bytes &bytes)
{
    static const char *digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        unsigned n = static_cast<unsigned>(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            n |= static_cast<unsigned>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size())
            n |= bytes[i + 2];
        result += digits[(n >> 18) & 63];
        result += digits[(n >> 12) & 63];
        result += i + 1 < bytes.size() ? digits[(n >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? digits[n & 63] : '=';
    }
    return result;
}
Bytes Decode64(const std::string &text)
{
    if (text.size() % 4 || text.size() > 1400000)
        throw std::runtime_error("Invalid base64 size");
    Bytes output;
    const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < text.size(); i += 4)
    {
        unsigned n = 0;
        int padding = 0;
        for (size_t j = 0; j < 4; ++j)
        {
            char c = text[i + j];
            auto pos = digits.find(c);
            if (c == '=')
            {
                if (j < 2 || i + 4 != text.size())
                    throw std::runtime_error("Invalid padding");
                ++padding;
                n <<= 6;
            }
            else
            {
                if (pos == std::string::npos || padding)
                    throw std::runtime_error("Invalid base64");
                n = (n << 6) | static_cast<unsigned>(pos);
            }
        }
        output.push_back(static_cast<std::uint8_t>(n >> 16));
        if (padding < 2)
            output.push_back(static_cast<std::uint8_t>(n >> 8));
        if (padding < 1)
            output.push_back(static_cast<std::uint8_t>(n));
    }
    return output;
}
ApiServer::ApiServer()
{
    instance_ = Random().substr(0, 32);
}
ApiServer::~ApiServer()
{
    Stop();
}
std::string ApiServer::Token() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return token_;
}
bool ApiServer::Start(Handler handler, std::map<std::string, ApiGrant> grants, std::wstring &error)
{
    Stop();
    try
    {
        token_ = Random();
    }
    catch (...)
    {
        error = L"无法生成 API 密钥";
        return false;
    }
    unsigned port = 0;
    listen_ = Bind(7080, 7095, port);
    if (listen_ == INVALID_SOCKET)
    {
        error = L"7080–7095 端口均不可用";
        return false;
    }
    port_ = port;
    handler_ = std::move(handler);
    grants_ = std::move(grants);
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(discoveryInfoMutex);
        discoveryInstance = instance_;
        discoveryApiPort = port_;
    }
    unsigned discoveryPort =
        0; // Prefer the end of the legacy scan range so serial keeps port 7000.
    for (unsigned p = 7015; p >= 7000; --p)
    {
        discovery_ = Bind(p, p, discoveryPort);
        if (discovery_ != INVALID_SOCKET)
            break;
    }
    accept_ = std::thread(&ApiServer::Accept, this);
    if (discovery_ != INVALID_SOCKET)
        discoveryThread_ = std::thread(&ApiServer::Discover, this);
    return true;
}
void ApiServer::Stop()
{
    running_ = false;
    if (listen_ != INVALID_SOCKET)
    {
        shutdown(listen_, SD_BOTH);
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    if (discovery_ != INVALID_SOCKET)
    {
        shutdown(discovery_, SD_BOTH);
        closesocket(discovery_);
        discovery_ = INVALID_SOCKET;
    }
    if (accept_.joinable())
        accept_.join();
    if (discoveryThread_.joinable())
        discoveryThread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &client : clients_)
            if (!finished_.count(client.first))
                shutdown(client.first, SD_BOTH);
    }
    for (auto &client : clients_)
        if (client.second.joinable())
            client.second.join();
    clients_.clear();
    finished_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token_.clear();
        grants_.clear();
        handler_ = {};
        events_.clear();
    }
    port_ = 0;
    {
        std::lock_guard<std::mutex> lock(discoveryInfoMutex);
        discoveryApiPort = 0;
    }
}
void ApiServer::Accept()
{
    while (running_)
    {
        SOCKET socket = accept(listen_, nullptr, nullptr);
        if (socket == INVALID_SOCKET)
            break;
        DWORD timeout = 3000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
                   sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout),
                   sizeof(timeout));
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto old : finished_)
        {
            auto it = clients_.find(old);
            if (it != clients_.end())
            {
                it->second.join();
                clients_.erase(it);
            }
        }
        finished_.clear();
        if (clients_.size() >= 16)
        {
            closesocket(socket);
            continue;
        }
        clients_.emplace(socket, std::thread([this, socket] {
                             Client(socket);
                             std::lock_guard<std::mutex> lock(mutex_);
                             closesocket(socket);
                             finished_.insert(socket);
                         }));
    }
}
void ApiServer::Discover()
{
    while (running_)
    {
        SOCKET socket = accept(discovery_, nullptr, nullptr);
        if (socket == INVALID_SOCKET)
            break;
        DWORD timeout = 500;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout),
                   sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout),
                   sizeof(timeout));
        std::string command;
        char c = 0;
        auto deadline = GetTickCount64() + 1500;
        while (running_ && GetTickCount64() < deadline && command.size() < 128 &&
               recv(socket, &c, 1, 0) == 1 && c != '\n')
            command += c;
        const auto reply = command == "SERIALCTL/3 DISCOVER" ? ApiDiscoveryReply()
                                                             : std::string("ERR NO_SERIAL\n");
        SendAll(socket, reply.data(), reply.size());
        closesocket(socket);
    }
}
bool ApiServer::Allowed(const std::string &method, const std::string &path, const Json &body)
{
    auto parts = Segments(path);
    if (parts.size() < 3 || parts[0] != "api" || parts[1] != "v1")
        return false;
    if (method == "GET" && parts[2] == "resources")
        return true;
    if (parts.size() < 4)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = parts[3];
    if (parts[2] == "actions")
    {
        auto grant = grants_.find("power-1");
        return method == "GET" && grant != grants_.end() && (grant->second.rights & 1);
    }
    auto it = grants_.find(id);
    if (it == grants_.end())
        return false;
    unsigned right = 1;
    if (method != "GET")
    {
        if (parts[2] == "sessions")
            right = 2;
        else if (parts.size() > 4)
        {
            std::string action = parts.back();
            right = action == "output"                                 ? 4
                    : action == "parameters" || action == "protection" ? 8
                    : action == "task" || action == "stop"             ? 16
                                                                       : 32;
        }
        else
            return false;
    }
    if (!(it->second.rights & right))
        return false;
    if (right == 16 && !(it->second.rights & 4))
        return false;
    if (body.contains("channels"))
    {
        if (!body["channels"].is_array() || body["channels"].empty())
            return false;
        for (const auto &c : body["channels"])
        {
            if (!c.is_number_integer())
                return false;
            int n = c.get<int>();
            if (n < 1 || n > 3 || !(it->second.channels & (1u << (n - 1))))
                return false;
        }
    }
    return true;
}
Json ApiServer::Call(const std::string &method, const std::string &path, const Json &body)
{
    if (!Allowed(method, path, body))
        return {{"error", "Resource or operation not authorized"}, {"httpStatus", 403}};
    if (method == "GET" && path.find("/events") != std::string::npos)
    {
        auto parts = Segments(path);
        return Events(parts[3], body.value("after", std::uint64_t(0)));
    }
    if (method == "POST" && (path == "/api/v1/power-supplies/power-1/stop" ||
                             path == "/api/v1/power-supplies/power-1/disconnect"))
    {
        auto state = handler_("GET", "/api/v1/power-supplies/power-1", Json::object());
        if (state.contains("ownedChannels") && !state["ownedChannels"].empty())
        {
            Json check = {{"channels", state["ownedChannels"]}, {"enabled", false}};
            if (!Allowed("POST", "/api/v1/power-supplies/power-1/channels/output", check))
                return {{"error", "Task owns channels outside this grant"}, {"httpStatus", 403}};
        }
    }
    auto response = handler_(method, path, body);
    if (path == "/api/v1/resources" && response.contains("resources"))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Json allowed = Json::array();
        for (auto resource : response["resources"])
        {
            auto found = grants_.find(resource["id"].get<std::string>());
            if (found != grants_.end())
            {
                resource["rights"] = found->second.rights;
                resource["channelsMask"] = found->second.channels;
                allowed.push_back(resource);
            }
        }
        response["resources"] = allowed;
        response["instance"] = instance_;
    }
    return response;
}
void ApiServer::Client(SOCKET socket)
{
    try
    {
        auto requestDeadline = GetTickCount64() + 6000;
        std::string request;
        std::array<char, 4096> buffer{};
        size_t headerEnd = std::string::npos;
        while (GetTickCount64() < requestDeadline && request.size() < 16384 &&
               (headerEnd = request.find("\r\n\r\n")) == std::string::npos)
        {
            int n = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (n <= 0)
                throw std::runtime_error("Closed");
            request.append(buffer.data(), n);
        }
        if (headerEnd == std::string::npos)
            throw std::runtime_error("Header too large");
        std::istringstream head(request.substr(0, headerEnd));
        std::string method, path, version;
        head >> method >> path >> version;
        if ((method != "GET" && method != "POST") || version != "HTTP/1.1")
            throw std::runtime_error("Invalid HTTP request");
        std::string line;
        std::getline(head, line);
        std::map<std::string, std::string> headers;
        while (std::getline(head, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("Malformed header");
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value[0] == ' ')
                value.erase(0, 1);
            std::string key = Lower(line.substr(0, colon));
            if (headers.count(key))
                throw std::runtime_error("Duplicate header");
            headers[key] = value;
        }
        Json response;
        unsigned status = 200;
        bool authenticated = EqualToken(headers["authorization"], "Bearer " + Token());
        Json body = Json::object();
        if (!authenticated)
        {
            status = 401;
            response = {{"error", "Bearer token required"}};
        }
        else
        {
            if (headers.count("transfer-encoding"))
                throw std::runtime_error("Chunked requests unsupported");
            size_t length = 0;
            if (headers.count("content-length"))
            {
                const auto &text = headers["content-length"];
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("Invalid length");
                length = std::stoull(text);
            }
            if (length > 1024 * 1024)
                throw std::runtime_error("Request body too large");
            std::string payload = request.substr(headerEnd + 4);
            while (payload.size() < length)
            {
                if (GetTickCount64() > requestDeadline)
                    throw std::runtime_error("Request timeout");
                int n = recv(socket, buffer.data(),
                             static_cast<int>(std::min(buffer.size(), length - payload.size())), 0);
                if (n <= 0)
                    throw std::runtime_error("Incomplete body");
                payload.append(buffer.data(), n);
            }
            if (length)
                body = Json::parse(payload.substr(0, length));
            if (!body.is_object())
                throw std::runtime_error("JSON object required");
            auto query = path.find('?');
            if (query != std::string::npos)
            {
                std::string q = path.substr(query + 1);
                path.resize(query);
                if (q.rfind("after=", 0) == 0)
                    body["after"] = std::stoull(q.substr(6));
            }
            if (Lower(headers["upgrade"]) == "websocket")
            {
                auto parts = Segments(path);
                if (method == "GET" && parts.size() == 5 && parts[2] == "sessions" &&
                    parts[4] == "stream" && Allowed(method, path, body) &&
                    headers["sec-websocket-version"] == "13")
                {
                    auto decoded = Decode64(headers["sec-websocket-key"]);
                    if (decoded.size() != 16)
                        throw std::runtime_error("Invalid websocket key");
                    Websocket(socket, parts[3], headers["sec-websocket-key"]);
                    return;
                }
                response = {{"error", "Stream not authorized"}};
                status = 403;
            }
            else
            {
                response = Call(method, path, body);
                status = response.value("httpStatus", response.contains("error") ? 400 : 200);
                response.erase("httpStatus");
            }
        }
        std::string payload = response.dump();
        std::string output =
            "HTTP/1.1 " + std::to_string(status) +
            " Result\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: "
            "no-store\r\nConnection: close\r\nContent-Length: " +
            std::to_string(payload.size()) + "\r\n\r\n" + payload;
        SendAll(socket, output.data(), output.size());
    }
    catch (...)
    {
        const std::string response =
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        SendAll(socket, response.data(), response.size());
    }
}
void ApiServer::Publish(const std::string &resource, const Bytes &bytes, const char *source,
                        const char *kind)
{
    if (!running_ || bytes.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = grants_.find(resource);
    if (it == grants_.end() || !(it->second.rights & 1))
        return;
    for (size_t offset = 0; offset < bytes.size(); offset += 16384)
    {
        Bytes chunk(bytes.begin() + offset, bytes.begin() + std::min(bytes.size(), offset + 16384));
        events_.push_back({{"seq", ++sequence_},
                           {"resource", resource},
                           {"type", kind},
                           {"source", source},
                           {"data", Encode64(chunk)},
                           {"tick", GetTickCount64()}});
    }
    while (events_.size() > 1024)
        events_.pop_front();
}
Json ApiServer::Events(const std::string &resource, std::uint64_t after)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Json events = Json::array();
    bool gap =
        !events_.empty() && after != 0 && after + 1 < events_.front()["seq"].get<std::uint64_t>();
    for (const auto &e : events_)
        if (e["resource"] == resource && e["seq"].get<std::uint64_t>() > after)
            events.push_back(e);
    return {{"events", events}, {"cursor", sequence_}, {"gap", gap}};
}
void ApiServer::Websocket(SOCKET socket, const std::string &resource, const std::string &key)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    Bytes digest(20);
    DWORD length = 20;
    std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return;
    if (!CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash))
    {
        CryptReleaseContext(provider, 0);
        return;
    }
    CryptHashData(hash, reinterpret_cast<const BYTE *>(input.data()),
                  static_cast<DWORD>(input.size()), 0);
    bool ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &length, 0) != FALSE;
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok)
        return;
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                           "Upgrade\r\nSec-WebSocket-Accept: " +
                           Encode64(digest) + "\r\n\r\n";
    if (!SendAll(socket, response.data(), response.size()))
        return;
    std::uint64_t cursor = Events(resource, 0)["cursor"];
    while (running_)
    {
        auto events = Events(resource, cursor);
        if (events["gap"].get<bool>())
        {
            std::string text = "{\"error\":\"Stream gap\"}";
            Frame(socket, 1, Bytes(text.begin(), text.end()));
            break;
        }
        for (const auto &event : events["events"])
        {
            if (event["type"] == "output")
            {
                if (!Frame(socket, 2, Decode64(event["data"])))
                    return;
            }
            else
            {
                std::string text = event.dump();
                if (!Frame(socket, 1, Bytes(text.begin(), text.end())))
                    return;
            }
        }
        cursor = events["cursor"];
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(socket, &reads);
        timeval wait{0, 50000};
        int ready = select(0, &reads, nullptr, nullptr, &wait);
        if (ready < 0)
            break;
        if (!ready)
            continue;
        std::uint8_t head[2]{};
        if (!ReadAll(socket, head, 2))
            break;
        bool fin = (head[0] & 0x80) != 0, mask = (head[1] & 0x80) != 0;
        unsigned opcode = head[0] & 15;
        if (!fin || !mask || (head[0] & 0x70))
            break;
        std::uint64_t size = head[1] & 127;
        if (size == 126)
        {
            std::uint8_t n[2]{};
            if (!ReadAll(socket, n, 2))
                break;
            size = (n[0] << 8) | n[1];
        }
        else if (size == 127)
        {
            std::uint8_t n[8]{};
            if (!ReadAll(socket, n, 8))
                break;
            size = 0;
            for (auto b : n)
                size = (size << 8) | b;
        }
        if (size > 65536 || ((opcode & 8) && size > 125))
            break;
        std::uint8_t masking[4]{};
        if (!ReadAll(socket, masking, 4))
            break;
        Bytes payload(static_cast<size_t>(size));
        if (!ReadAll(socket, payload.data(), payload.size()))
            break;
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= masking[i % 4];
        if (opcode == 8)
        {
            Frame(socket, 8, {});
            break;
        }
        if (opcode == 9)
        {
            if (!Frame(socket, 10, payload))
                break;
            continue;
        }
        if (opcode == 10)
            continue;
        if (opcode != 2)
            break;
        auto result =
            Call("POST", "/api/v1/sessions/" + resource + "/input", {{"data", Encode64(payload)}});
        std::string text = result.dump();
        if (!Frame(socket, 1, Bytes(text.begin(), text.end())))
            break;
    }
}
} // namespace serialctl
