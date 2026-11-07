#include "McpHttpServer.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace serialctl::mcp {

namespace {
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
            send(clientSocket, resp.data(), static_cast<int>(resp.size()), 0);
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

        std::string allowedOrigin;
        if (!origin.empty()) {
            std::string lowerOrigin = origin;
            std::transform(lowerOrigin.begin(), lowerOrigin.end(), lowerOrigin.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowerOrigin.rfind("http://localhost", 0) == 0 ||
                lowerOrigin.rfind("https://localhost", 0) == 0 ||
                lowerOrigin.rfind("http://127.0.0.1", 0) == 0 ||
                lowerOrigin.rfind("https://127.0.0.1", 0) == 0) {
                allowedOrigin = origin;
            }
        }

        std::string response;
        if (method == "OPTIONS") {
            response = BuildCorsPreflightResponse(allowedOrigin);
        } else if (method == "GET" && (uri == "/mcp" || uri == "/health" || uri == "/")) {
            std::string healthBody = healthHandler_ ? healthHandler_() : "{\"status\":\"ok\"}";
            response = BuildHttpResponse(200, "OK", "application/json", healthBody, allowedOrigin);
        } else if (method == "POST" && (uri == "/mcp" || uri == "/")) {
            size_t headerLength = (requestData.find("\r\n\r\n") != std::string::npos) ? 4 : 2;
            size_t bodyStart = headerEnd + headerLength;
            std::string body = requestData.substr(bodyStart, contentLength);

            std::string resultJson = requestHandler_ ? requestHandler_(clientIp, body) : "{}";
            response = BuildHttpResponse(200, "OK", "application/json", resultJson, allowedOrigin);
        } else {
            response = BuildHttpResponse(404, "Not Found", "application/json",
                "{\"error\": \"Not Found\", \"message\": \"Use POST /mcp or GET /health\"}", allowedOrigin);
        }

        send(clientSocket, response.data(), static_cast<int>(response.size()), 0);
    }

    closesocket(clientSocket);
}

} // namespace serialctl::mcp
