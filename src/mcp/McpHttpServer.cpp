#include "McpHttpServer.h"
#include "json.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace serialctl::mcp {

namespace {
bool SendHttpAll(SOCKET socket, const std::string& response) {
    size_t written = 0;
    while (written < response.size()) {
        const size_t remaining = response.size() - written;
        const int count = send(socket, response.data() + written,
                               static_cast<int>(std::min<size_t>(remaining, 64 * 1024)), 0);
        if (count <= 0) return false;
        written += static_cast<size_t>(count);
    }
    return true;
}

bool ValidHostPort(const std::string& authority, bool localOnly) {
    if (authority.empty() || authority.size() > 255 ||
        authority.find_first_of("/?#@ \t\r\n") != std::string::npos) return false;
    const auto colon = authority.find(':');
    if (colon != std::string::npos) {
        if (authority.find(':', colon + 1) != std::string::npos) return false;
        int port = 0;
        const std::string digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return false;
        for (unsigned char c : digits) {
            if (!std::isdigit(c)) return false;
            port = port * 10 + (c - '0');
        }
        if (port == 0 || port > 65535) return false;
    }
    std::string host = authority.substr(0, colon);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (host == "localhost" || host == "127.0.0.1") return true;
    if (localOnly) return false;
    in_addr ip{};
    return inet_pton(AF_INET, host.c_str(), &ip) == 1;
}

bool AllowedBrowserOrigin(const std::string& origin) {
    std::string lower = origin;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.rfind("http://", 0) == 0) return ValidHostPort(lower.substr(7), true);
    if (lower.rfind("https://", 0) == 0) return ValidHostPort(lower.substr(8), true);
    return false;
}

std::string BuildHttpResponse(int statusCode, const std::string& statusText,
                             const std::string& contentType, const std::string& body,
                             const std::string& allowedOrigin = "") {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n"
       << "Content-Type: " << contentType << "\r\n"
       << "Content-Length: " << body.size() << "\r\n";
    if (!allowedOrigin.empty()) {
        ss << "Access-Control-Allow-Origin: " << allowedOrigin << "\r\n"
           << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
           << "Access-Control-Allow-Headers: Content-Type, Authorization, X-MCP-Recursion-Depth\r\n";
    }
    ss << "Connection: close\r\n\r\n"
       << body;
    return ss.str();
}

std::string BuildCorsPreflightResponse(const std::string& allowedOrigin) {
    if (allowedOrigin.empty()) {
        return "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    std::ostringstream ss;
    ss << "HTTP/1.1 204 No Content\r\n"
       << "Access-Control-Allow-Origin: " << allowedOrigin << "\r\n"
       << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type, Authorization, X-MCP-Recursion-Depth\r\n"
       << "Access-Control-Max-Age: 86400\r\n"
       << "Content-Length: 0\r\n"
       << "Connection: close\r\n\r\n";
    return ss.str();
}
}

McpHttpServer::McpHttpServer() = default;

McpHttpServer::~McpHttpServer() {
    Stop();
}

bool McpHttpServer::Start(const std::string& bindIp, uint16_t port,
                          RequestHandler requestHandler, HealthHandler healthHandler,
                          std::wstring& error) {
    if (running_.load()) return true;

    bindIp_ = bindIp.empty() ? "0.0.0.0" : bindIp;
    port_ = port ? port : 7080;
    requestHandler_ = std::move(requestHandler);
    healthHandler_ = std::move(healthHandler);

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        error = L"Failed to create TCP socket: " + std::to_wstring(WSAGetLastError());
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port_);
    if (bindIp_ == "0.0.0.0") {
        sin.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bindIp_.c_str(), &sin.sin_addr) <= 0) {
            error = L"Invalid bind IP address: " + std::wstring(bindIp_.begin(), bindIp_.end());
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
            return false;
        }
    }

    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == SOCKET_ERROR) {
        error = L"Failed to bind MCP server to " + std::wstring(bindIp_.begin(), bindIp_.end()) +
                L":" + std::to_wstring(port_) + L", error: " + std::to_wstring(WSAGetLastError());
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        error = L"Failed to listen on socket, error: " + std::to_wstring(WSAGetLastError());
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    running_.store(true);
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void McpHttpServer::Stop() {
    if (!running_.exchange(false)) return;

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    std::vector<ClientThread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        threadsToJoin = std::move(clientThreads_);
    }
    for (auto& ct : threadsToJoin) {
        if (ct.thread.joinable()) ct.thread.join();
    }
}

void McpHttpServer::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket_, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (!running_.load()) break;
            Sleep(10);
            continue;
        }

        char ipStr[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string clientIp = ipStr;

        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
        // Clean up done threads
        for (auto it = clientThreads_.begin(); it != clientThreads_.end();) {
            if (it->done && it->done->load()) {
                if (it->thread.joinable()) it->thread.join();
                it = clientThreads_.erase(it);
            } else {
                ++it;
            }
        }

        if (clientThreads_.size() >= kMaxConcurrentHttpClients) {
            std::string resp = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            SendHttpAll(clientSocket, resp);
            closesocket(clientSocket);
            continue;
        }

        auto doneFlag = std::make_shared<std::atomic<bool>>(false);
        clientThreads_.push_back({
            std::thread([this, clientSocket, clientIp, doneFlag] {
                HandleClient(clientSocket, clientIp);
                doneFlag->store(true);
            }),
            doneFlag
        });
    }
}

void McpHttpServer::HandleClient(SOCKET clientSocket, std::string clientIp) {
    // Set 5-second socket timeout
    DWORD timeout = 5000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string requestData;
    char buffer[4096];
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;
    bool hasContentLength = false;
    std::string origin;
    std::string host;
    std::string contentType;
    std::string protocolVersion;

    while (running_.load()) {
        int bytes = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        requestData.append(buffer, bytes);

        if (headerEnd == std::string::npos) {
            headerEnd = requestData.find("\r\n\r\n");
            if (headerEnd == std::string::npos) {
                headerEnd = requestData.find("\n\n");
            }

            if (headerEnd != std::string::npos) {
                // Parse headers
                std::string headers = requestData.substr(0, headerEnd);
                std::istringstream stream(headers);
                std::string line;
                while (std::getline(stream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::string lowerLine = line;
                    std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (lowerLine.rfind("content-length:", 0) == 0) {
                        try {
                            contentLength = std::stoul(line.substr(15));
                            hasContentLength = true;
                        } catch (...) {}
                    } else if (lowerLine.rfind("origin:", 0) == 0) {
                        origin = line.substr(7);
                        while (!origin.empty() && origin.front() == ' ') origin.erase(origin.begin());
                    } else if (lowerLine.rfind("host:", 0) == 0) {
                        host = line.substr(5);
                        while (!host.empty() && host.front() == ' ') host.erase(host.begin());
                    } else if (lowerLine.rfind("content-type:", 0) == 0) {
                        contentType = lowerLine.substr(13);
                        while (!contentType.empty() && contentType.front() == ' ') contentType.erase(contentType.begin());
                    } else if (lowerLine.rfind("mcp-protocol-version:", 0) == 0) {
                        protocolVersion = line.substr(21);
                        while (!protocolVersion.empty() && protocolVersion.front() == ' ') protocolVersion.erase(protocolVersion.begin());
                        while (!protocolVersion.empty() && protocolVersion.back() == ' ') protocolVersion.pop_back();
                    }
                }
            }
        }

        if (headerEnd != std::string::npos) {
            size_t headerLength = (requestData.find("\r\n\r\n") != std::string::npos) ? 4 : 2;
            size_t bodyStart = headerEnd + headerLength;
            size_t bodyReceived = requestData.size() - bodyStart;
            if (!hasContentLength || bodyReceived >= contentLength) {
                break;
            }
        }

        if (requestData.size() > 16 * 1024 * 1024) { // 16 MB max request bound
            break;
        }
    }

    if (!requestData.empty() && headerEnd != std::string::npos) {
        std::string requestLine;
        {
            std::istringstream stream(requestData.substr(0, headerEnd));
            std::getline(stream, requestLine);
            if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
        }

        std::string method, uri, version;
        {
            std::istringstream stream(requestLine);
            stream >> method >> uri >> version;
        }

        // A CORS header alone is not access control. Reject hostile browser
        // origins and DNS-rebinding Host values BEFORE dispatching any tool.
        if ((!origin.empty() && !AllowedBrowserOrigin(origin)) ||
            !ValidHostPort(host, false)) {
            const std::string rejected =
                "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            SendHttpAll(clientSocket, rejected);
            closesocket(clientSocket);
            return;
        }
        const std::string allowedOrigin = origin;

        // Stateless MCP Streamable HTTP (2025-03-26): each request is one POST;
        // server-to-client SSE and sessions are intentionally not advertised.
        std::string response;
        if (!protocolVersion.empty() && protocolVersion != "2025-03-26") {
            response = BuildHttpResponse(400, "Bad Request", "application/json",
                "{\"error\":\"Unsupported MCP-Protocol-Version\"}", allowedOrigin);
        } else if (method == "OPTIONS") {
            response = BuildCorsPreflightResponse(allowedOrigin);
        } else if (method == "GET" && uri == "/mcp") {
            response = BuildHttpResponse(405, "Method Not Allowed", "application/json",
                "{\"error\":\"SSE stream not supported by this stateless MCP endpoint\"}", allowedOrigin);
        } else if (method == "GET" && (uri == "/health" || uri == "/")) {
            std::string healthBody = healthHandler_ ? healthHandler_() : "{\"status\":\"ok\"}";
            response = BuildHttpResponse(200, "OK", "application/json", healthBody, allowedOrigin);
        } else if (method == "POST" && (uri == "/mcp" || uri == "/")) {
            const size_t headerLength = (requestData.find("\r\n\r\n") != std::string::npos) ? 4 : 2;
            const size_t bodyStart = headerEnd + headerLength;
            if (!hasContentLength || contentLength > 16 * 1024 * 1024 ||
                requestData.size() - bodyStart < contentLength ||
                contentType.rfind("application/json", 0) != 0) {
                response = BuildHttpResponse(400, "Bad Request", "application/json",
                    "{\"error\":\"Complete application/json request and Content-Length required\"}", allowedOrigin);
            } else {
                const std::string body = requestData.substr(bodyStart, contentLength);
                const auto request = nlohmann::json::parse(body, nullptr, false);
                if (!request.is_object() || !request.contains("jsonrpc") ||
                    !request["jsonrpc"].is_string() || request["jsonrpc"] != "2.0" ||
                    !request.contains("method") || !request["method"].is_string()) {
                    response = BuildHttpResponse(400, "Bad Request", "application/json",
                        "{\"error\":\"Expected a single JSON-RPC 2.0 request or notification\"}", allowedOrigin);
                } else if (!request.contains("id")) {
                    const auto messageMethod = request["method"].get<std::string>();
                    if (messageMethod.rfind("notifications/", 0) == 0) {
                        // Notifications must never produce a JSON-RPC response.
                        response = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    } else {
                        response = BuildHttpResponse(400, "Bad Request", "application/json",
                            "{\"error\":\"Request id missing\"}", allowedOrigin);
                    }
                } else {
                    const std::string resultJson = requestHandler_ ? requestHandler_(clientIp, body) : "{}";
                    response = BuildHttpResponse(200, "OK", "application/json", resultJson, allowedOrigin);
                }
            }
        } else if (method == "DELETE" && uri == "/mcp") {
            response = BuildHttpResponse(405, "Method Not Allowed", "application/json",
                "{\"error\":\"Session deletion not supported by stateless transport\"}", allowedOrigin);
        } else {
            response = BuildHttpResponse(404, "Not Found", "application/json",
                "{\"error\":\"Not Found\",\"message\":\"Use POST /mcp or GET /health\"}", allowedOrigin);
        }

        SendHttpAll(clientSocket, response);
    }

    closesocket(clientSocket);
}

} // namespace serialctl::mcp
