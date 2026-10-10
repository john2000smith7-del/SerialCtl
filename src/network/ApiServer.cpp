#include "ApiServer.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <ws2tcpip.h>
namespace serialctl {
namespace {
Json Failure(const char *code, const std::string &detail) {
    return {{"error", {{"code", code}, {"message", detail}}}};
}
bool JsonDepth(const Bytes &bytes) {
    unsigned depth = 0;
    bool string = false, escape = false;
    for (auto b : bytes) {
        if (string) {
            if (escape)
                escape = false;
            else if (b == '\\')
                escape = true;
            else if (b == '"')
                string = false;
        } else if (b == '"')
            string = true;
        else if (b == '{' || b == '[') {
            if (++depth > 32)
                return false;
        } else if ((b == '}' || b == ']') && depth)
            --depth;
    }
    return true;
}
bool Keys(const Json &j, std::initializer_list<const char *> keys) {
    if (!j.is_object())
        return false;
    for (auto i = j.begin(); i != j.end(); ++i) {
        bool found = false;
        for (auto k : keys)
            found = found || i.key() == k;
        if (!found)
            return false;
    }
    return true;
}
bool Text(const Json &j, const char *key, size_t max = 128) {
    return j.contains(key) && j[key].is_string() && !j[key].get_ref<const std::string &>().empty() &&
           j[key].get_ref<const std::string &>().size() <= max;
}
} // namespace
bool ApiServer::Start(Handler handler, std::wstring &error, unsigned first, unsigned last) {
    Stop();
    if (first == 0 || first > last || last > 65535) {
        error = L"无效端口范围";
        return false;
    }
    try {
        instance_ = ws::RandomHex();
    } catch (...) {
        error = L"无法初始化网络实例";
        return false;
    }
    for (unsigned port = first; port <= last; ++port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            break;
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char *>(&yes), sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(u_short(port));
        if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && listen(s, SOMAXCONN) == 0) {
            listen_ = s;
            port_ = port;
            break;
        }
        closesocket(s);
    }
    if (listen_ == INVALID_SOCKET) {
        error = std::to_wstring(first) + L"–" + std::to_wstring(last) + L" 端口均不可用；本地会话仍可使用";
        return false;
    }
    acceptEvent_ = WSACreateEvent();
    stopEvent_ = WSACreateEvent();
    if (acceptEvent_ == WSA_INVALID_EVENT || stopEvent_ == WSA_INVALID_EVENT ||
        WSAEventSelect(listen_, acceptEvent_, FD_ACCEPT | FD_CLOSE) != 0) {
        error = L"无法建立网络监听事件";
        Stop();
        return false;
    }
    handler_ = std::move(handler);
    running_ = true;
    accept_ = std::thread(&ApiServer::Accept, this);
    return true;
}
void ApiServer::Stop() {
    running_ = false;
    if (stopEvent_ != WSA_INVALID_EVENT)
        WSASetEvent(stopEvent_);
    if (accept_.joinable())
        accept_.join();
    auto s = listen_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET)
        closesocket(s);
    for (auto *event : {&acceptEvent_, &stopEvent_})
        if (*event != WSA_INVALID_EVENT) {
            WSACloseEvent(*event);
            *event = WSA_INVALID_EVENT;
        }
    std::vector<std::shared_ptr<ClientState>> clients;
    {
        std::lock_guard<std::mutex> l(mutex_);
        clients.swap(clients_);
    }
    for (auto &c : clients) {
        Close(c, 1001, "SERVER_STOPPING");
        if (c->reader.joinable())
            CancelSynchronousIo(reinterpret_cast<HANDLE>(c->reader.native_handle()));
    }
    for (auto &c : clients)
        if (c->reader.joinable())
            c->reader.join();
    {
        std::lock_guard<std::mutex> l(mutex_);
        histories_.clear();
        historyBytes_ = 0;
        sessions_.clear();
        handler_ = {};
    }
    port_ = 0;
}
void ApiServer::Trace(const std::shared_ptr<ClientState> &c, const std::string &phase, const std::string &detail) {
    Json entry = {
        {"tick", GetTickCount64()}, {"client", c->id}, {"peer", c->peer}, {"phase", phase}, {"detail", detail}};
    const auto text = entry.dump() + "\n";
    OutputDebugStringA(text.c_str());
    std::lock_guard<std::mutex> l(mutex_);
    diagnostics_.push_back(std::move(entry));
    while (diagnostics_.size() > 512)
        diagnostics_.pop_front();
}
Json ApiServer::Diagnostics() {
    std::lock_guard<std::mutex> l(mutex_);
    return diagnostics_;
}
void ApiServer::Accept() {
    WSAEVENT waits[] = {stopEvent_, acceptEvent_};
    while (running_) {
        auto signaled = WSAWaitForMultipleEvents(2, waits, FALSE, WSA_INFINITE, FALSE);
        if (!running_ || signaled != WSA_WAIT_EVENT_0 + 1)
            break;
        WSANETWORKEVENTS events{};
        if (WSAEnumNetworkEvents(listen_, acceptEvent_, &events) != 0 || (events.lNetworkEvents & FD_CLOSE))
            break;
        for (;;) {
            sockaddr_in a{};
            int size = sizeof(a);
            SOCKET s = accept(listen_, reinterpret_cast<sockaddr *>(&a), &size);
            if (s == INVALID_SOCKET)
                break;
            if (!running_) {
                closesocket(s);
                break;
            }
            // Accepted sockets inherit nonblocking/event properties from the listener.
            WSAEventSelect(s, nullptr, 0);
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            std::vector<std::shared_ptr<ClientState>> retired;
            std::shared_ptr<ClientState> c;
            {
                std::lock_guard<std::mutex> l(mutex_);
                for (auto it = clients_.begin(); it != clients_.end();)
                    if ((*it)->finished) {
                        retired.push_back(*it);
                        it = clients_.erase(it);
                    } else
                        ++it;
                if (clients_.size() < MaxClients) {
                    c = std::make_shared<ClientState>();
                    c->socket = s;
                    c->id = nextClient_++;
                    char peer[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &a.sin_addr, peer, sizeof(peer));
                    c->peer = std::string(peer) + ":" + std::to_string(ntohs(a.sin_port));
                    clients_.push_back(c);
                }
            }
            for (auto &old : retired)
                old->reader.join();
            ws::SocketOptions(s, 1500);
            if (!c) {
                const std::string reply =
                    "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 12\r\n\r\nCLIENT_LIMIT";
                ws::SendAll(s, reply.data(), reply.size());
                ws::FinishSend(s);
                closesocket(s);
                continue;
            }
            c->reader = std::thread(&ApiServer::Client, this, c);
        }
    }
}
void ApiServer::Queue(const std::shared_ptr<ClientState> &c, const Json &j) {
    auto text = j.dump();
    Queue(c, 1, Bytes(text.begin(), text.end()));
}
void ApiServer::Queue(const std::shared_ptr<ClientState> &c, unsigned opcode, const Bytes &data) {
    std::lock_guard<std::mutex> l(c->mutex);
    if (c->closing)
        return;
    if (c->queued + data.size() > MaxQueueBytes || c->queue.size() >= 4096) {
        c->closing = true;
        c->slow = true;
        c->queue.clear();
        c->queued = 0;
        auto reason = ws::ClosePayload(1013, "SLOW_CLIENT: bounded send queue exceeded; reconnect with cursor");
        c->queue.push_back({8, std::move(reason)});
        SetEvent(c->readCancel);
    } else {
        c->queue.push_back({opcode, data});
        c->queued += data.size();
    }
    c->wake.notify_one();
}
void ApiServer::Close(const std::shared_ptr<ClientState> &c, unsigned code, const std::string &reason,
                      bool wakeReader) {
    std::lock_guard<std::mutex> l(c->mutex);
    if (c->closing || c->finished)
        return;
    c->closing = true;
    c->drainOnClose = !wakeReader;
    // Closing cancels queued output explicitly; the client must use its last received cursor.
    c->queue.clear();
    c->queued = 0;
    if (code != 1006)
        c->queue.push_back({8, ws::ClosePayload(code, reason)});
    c->wake.notify_one();
    if (wakeReader)
        SetEvent(c->readCancel);
}
void ApiServer::Send(const std::shared_ptr<ClientState> &c) {
    ws::Stream stream(c->socket, false);
    bool graceful = false;
    try {
        for (;;) {
            ws::Stream::Message m;
            {
                std::unique_lock<std::mutex> l(c->mutex);
                c->wake.wait(l, [&] { return c->closing || !c->queue.empty(); });
                if (c->queue.empty())
                    break;
                m = std::move(c->queue.front());
                c->queue.pop_front();
                c->queued -= std::min(c->queued, m.data.size());
            }
            if (!stream.Send(m.opcode, m.data)) {
                const auto error = WSAGetLastError();
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                    c->slow = true;
                    Trace(c, "close", "SLOW_CLIENT: socket send timeout");
                } else
                    Trace(c, "close", "SEND_DISCONNECTED:" + std::to_string(error));
                break;
            }
            if (m.opcode == 8) {
                Trace(c, "close", std::string(m.data.begin() + 2, m.data.end()));
                {
                    std::lock_guard<std::mutex> lock(c->mutex);
                    graceful = c->drainOnClose;
                }
                break;
            }
        }
    } catch (...) {
        Trace(c, "close", "SEND_FAILED");
    }
    if (graceful)
        ws::FinishSend(c->socket);
    SetEvent(c->readCancel);
    shutdown(c->socket, SD_BOTH);
}
void ApiServer::Client(const std::shared_ptr<ClientState> &c) {
    bool upgraded = false;
    Trace(c, "accepted");
    try {
        Bytes tail;
        auto h = ws::ReadHeader(c->socket, tail);
        if (h.first != "GET /serialctl HTTP/1.1" || ws::Lower(h.fields["upgrade"]) != "websocket" ||
            !ws::Token(h.fields["connection"], "upgrade") || h.fields["sec-websocket-version"] != "13" ||
            !ws::Token(h.fields["sec-websocket-protocol"], ws::Protocol) || h.fields.count("transfer-encoding") ||
            (h.fields.count("content-length") && h.fields["content-length"] != "0"))
            throw ws::Error(1002, "UPGRADE_REQUIRED_SERIALCTL_V1");
        auto response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                        "Upgrade\r\nSec-WebSocket-Protocol: " +
                        std::string(ws::Protocol) +
                        "\r\nSec-WebSocket-Accept: " + ws::AcceptKey(h.fields["sec-websocket-key"]) + "\r\n\r\n";
        if (!ws::SendAll(c->socket, response.data(), response.size()))
            throw ws::Error(1006, "HANDSHAKE_SEND_FAILED");
        upgraded = true;
        ws::SocketOptions(c->socket);
        c->sender = std::thread(&ApiServer::Send, this, c);
        Queue(c, Json{{"type", "hello"},
                      {"version", 1},
                      {"instance", instance_},
                      {"maxClients", MaxClients},
                      {"maxInput", 65536},
                      {"maxMessage", ws::MaxMessage},
                      {"historyBytes", MaxHistoryBytes}});
        Trace(c, "upgraded");
        ws::Stream stream(c->socket, false, std::move(tail), c->readCancel);
        while (running_) {
            auto m = stream.Receive();
            if (!running_ || WaitForSingleObject(c->readCancel, 0) == WAIT_OBJECT_0)
                break;
            if (m.opcode == 8) {
                Close(c, 1000, "PEER_CLOSED", false);
                break;
            }
            if (m.opcode == 9) {
                Queue(c, 10, m.data);
                continue;
            }
            if (m.opcode == 10)
                continue;
            if (m.opcode != 1)
                throw ws::Error(1003, "JSON_TEXT_REQUIRED");
            if (!JsonDepth(m.data))
                throw ws::Error(1007, "JSON_DEPTH_LIMIT");
            auto j = Json::parse(m.data.begin(), m.data.end(), nullptr, false);
            if (j.is_discarded()) {
                Queue(c, Json{{"type", "response"},
                              {"version", 1},
                              {"instance", instance_},
                              {"requestId", ""},
                              {"ok", false},
                              {"error", {{"code", "INVALID_JSON"}, {"message", "JSON object required"}}}});
                continue;
            }
            Request(c, j);
        }
    } catch (const ws::Error &e) {
        Trace(c, "rejected", e.what());
        if (upgraded)
            Close(c, e.code, e.what(), false);
        else {
            const std::string reply = "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\nSec-WebSocket-Protocol: "
                                      "serialctl.v1\r\nConnection: close\r\nContent-Length: 51\r\n\r\nUpgrade "
                                      "required: SerialCtl WebSocket serialctl.v1\n";
            ws::SendAll(c->socket, reply.data(), reply.size());
            ws::FinishSend(c->socket);
        }
    } catch (const std::exception &e) {
        Trace(c, "error", e.what());
        if (upgraded)
            Close(c, 1011, "INTERNAL_ERROR", false);
    }
    if (c->slow)
        Trace(c, "close", "SLOW_CLIENT: bounded send queue exceeded");
    if (upgraded) {
        Close(c, 1001, "SERVER_STOPPING");
        if (c->sender.joinable())
            c->sender.join();
    }
    shutdown(c->socket, SD_BOTH);
    closesocket(c->socket);
    c->finished = true;
}
void ApiServer::SetSessions(const std::vector<std::string> &list) {
    std::lock_guard<std::mutex> l(mutex_);
    std::set<std::string> next(list.begin(), list.end());
    for (auto &c : clients_)
        for (auto it = c->subscriptions.begin(); it != c->subscriptions.end();)
            if (!next.count(*it)) {
                Queue(c, Json{{"type", "resource_gone"},
                              {"version", 1},
                              {"instance", instance_},
                              {"resource", *it},
                              {"error", {{"code", "TARGET_GONE"}, {"message", "Session removed"}}}});
                it = c->subscriptions.erase(it);
            } else
                ++it;
    sessions_ = std::move(next);
    for (auto it = histories_.begin(); it != histories_.end();)
        if (!sessions_.count(it->first)) {
            historyBytes_ -= it->second.bytes;
            it = histories_.erase(it);
        } else
            ++it;
}
void ApiServer::Request(const std::shared_ptr<ClientState> &c, const Json &j) {
    if (!j.is_object()) {
        Queue(c, Json{{"type", "response"},
                      {"ok", false},
                      {"version", 1},
                      {"instance", instance_},
                      {"requestId", ""},
                      {"error", {{"code", "INVALID_REQUEST"}, {"message", "JSON object required"}}}});
        return;
    }
    std::string id = Text(j, "requestId") ? j["requestId"].get<std::string>() : "";
    Json result;
    std::string op = Text(j, "op", 32) ? j["op"].get<std::string>() : "";
    auto resource = j.value("resource", Json());
    std::string target = resource.is_string() ? resource.get<std::string>() : "";
    if (!Keys(j, {"type", "version", "instance", "requestId", "op", "resource", "params"}) ||
        j.value("type", Json()) != "request" || !j.contains("version") || !j["version"].is_number_integer() ||
        j["version"] != 1 || (j.contains("resource") && (!resource.is_string() || target.empty())) || id.empty() ||
        op.empty() || !j.contains("params") || !j["params"].is_object() || target.size() > 128)
        result = Failure("INVALID_REQUEST", "Invalid envelope, requestId, or params");
    else if (j.value("instance", Json()) != instance_)
        result = Failure("INSTANCE_MISMATCH", "Discover and explicitly select the server instance");
    else if (c->responses.count(id)) {
        auto old = c->responses.at(id);
        if (old.first == j) {
            Queue(c, old.second);
            return;
        }
        result = Failure("REQUEST_ID_CONFLICT", "requestId reused with different operation");
    } else
        try {
            const auto &p = j["params"];
            bool session = false;
            {
                std::lock_guard<std::mutex> l(mutex_);
                session = sessions_.count(target) != 0;
            }
            if (op == "resources" && target.empty() && p.empty())
                result = handler_(op, target, p);
            else if (op == "action.get" && target.empty() && Keys(p, {"id"}) && Text(p, "id"))
                result = handler_(op, target, p);
            else if (op == "power.get" && target == "power-1" && p.empty())
                result = handler_(op, target, p);
            else if (op == "power.output" && target == "power-1" && Keys(p, {"enabled"}) && p.contains("enabled") &&
                     p["enabled"].is_boolean()) {
                Json body = p;
                body["requestId"] = id;
                result = handler_(op, target, body);
            } else if (op == "session.get" && p.empty())
                result = session ? handler_(op, target, p)
                                 : Failure("TARGET_NOT_FOUND", "Session is absent or outside the published scope");
            else if (op == "input" && Keys(p, {"data"}) && Text(p, "data", 90000)) {
                auto bytes = Decode64(p["data"]);
                if (bytes.empty() || bytes.size() > 65536)
                    result = Failure("INPUT_SIZE", "Input must contain 1..65536 bytes");
                else if (!session)
                    result = Failure("TARGET_NOT_FOUND", "Session is absent or outside the published scope");
                else {
                    Trace(c, "input",
                          target + " source=network bytes=" + std::to_string(bytes.size()) + " requestId=" + id);
                    Json body = p;
                    body["source"] = "network:" + std::to_string(c->id);
                    result = handler_(op, target, body);
                }
            } else if (op == "subscribe" && Keys(p, {"after"}) &&
                       (!p.contains("after") || p["after"].is_number_unsigned() ||
                        (p["after"].is_number_integer() && p["after"].get<std::int64_t>() >= 0))) {
                std::lock_guard<std::mutex> l(mutex_);
                if (!sessions_.count(target))
                    result = Failure("TARGET_NOT_FOUND", "Cannot subscribe to this session");
                else if (c->subscriptions.size() >= 64 && !c->subscriptions.count(target))
                    result = Failure("SUBSCRIPTION_LIMIT", "At most 64 existing serial/CMD sessions per connection");
                else {
                    auto history = EventsLocked(target, p.value("after", std::uint64_t(0)));
                    if (history["gap"] == true)
                        result = Failure("HISTORY_GAP", history.dump());
                    else {
                        c->subscriptions.insert(target);
                        result = {{"cursor", history["cursor"]}, {"subscribed", target}};
                        Json reply = {{"type", "response"}, {"version", 1}, {"instance", instance_},
                                      {"requestId", id},    {"ok", true},   {"result", result}};
                        Queue(c, reply);
                        for (const auto &event : history["events"])
                            Queue(c, event);
                        CacheResponse(c, id, j, reply);
                        return;
                    }
                }
            } else if (op == "unsubscribe" && p.empty()) {
                std::lock_guard<std::mutex> l(mutex_);
                if (!sessions_.count(target))
                    result = Failure("TARGET_NOT_FOUND", "Cannot unsubscribe from an absent session");
                else {
                    c->subscriptions.erase(target);
                    result = {{"unsubscribed", target}};
                }
            } else if (op == "events" && Keys(p, {"after"}) && p.contains("after") &&
                       (p["after"].is_number_unsigned() ||
                        (p["after"].is_number_integer() && p["after"].get<std::int64_t>() >= 0)))
                result = session ? Events(target, p["after"]) : Failure("TARGET_NOT_FOUND", "Session absent");
            else
                result = Failure("OPERATION_DENIED", "Unknown operation, excess fields, or invalid parameters");
        } catch (const std::exception &e) {
            result = Failure("INVALID_PARAMETERS", e.what());
        }
    Json reply = {{"type", "response"},
                  {"version", 1},
                  {"instance", instance_},
                  {"requestId", id},
                  {"ok", !result.contains("error")}};
    if (result.contains("error")) {
        const auto &error = result["error"];
        reply["error"] = error.is_object() ? error : Json{{"code", "BACKEND_REJECTED"}, {"message", error}};
        if (!reply["error"].contains("message"))
            reply["error"]["message"] = reply["error"].value("code", "Backend error");
        Trace(c, "request_rejected", target + " op=" + op + " requestId=" + id + " " + reply["error"].dump());
    } else
        reply["result"] = result;
    Queue(c, reply);
    if (!id.empty() && !c->responses.count(id))
        CacheResponse(c, id, j, reply);
}
void ApiServer::CacheResponse(const std::shared_ptr<ClientState> &c, const std::string &id, const Json &request,
                              const Json &response) {
    c->responses[id] = {request, response};
    c->responseOrder.push_back(id);
    c->responseBytes += request.dump().size() + response.dump().size();
    while (c->responseOrder.size() > 256 || c->responseBytes > 1024 * 1024) {
        auto old = c->responses.find(c->responseOrder.front());
        if (old != c->responses.end()) {
            c->responseBytes -= old->second.first.dump().size() + old->second.second.dump().size();
            c->responses.erase(old);
        }
        c->responseOrder.pop_front();
    }
}

void ApiServer::Publish(const std::string &target, const Bytes &data, const char *source, const char *kind) {
    if (data.empty())
        return;
    std::lock_guard<std::mutex> l(mutex_);
    if (!sessions_.count(target))
        return;
    auto &h = histories_[target];
    for (size_t offset = 0; offset < data.size(); offset += 16384) {
        Bytes b(data.begin() + offset, data.begin() + std::min(data.size(), offset + 16384));
        Json event = {{"type", "event"},    {"version", 1},        {"instance", instance_},
                      {"resource", target}, {"seq", ++h.seq},      {"kind", kind},
                      {"source", source},   {"data", Encode64(b)}, {"tick", GetTickCount64()}};
        auto serialized = event.dump();
        Bytes payload(serialized.begin(), serialized.end());
        auto bytes = payload.size();
        h.events.push_back(event);
        h.bytes += bytes;
        historyBytes_ += bytes;
        for (auto &c : clients_)
            if (c->subscriptions.count(target))
                Queue(c, 1, payload);
        while (h.bytes > MaxHistoryBytes || h.events.size() > 1024) {
            auto n = h.events.front().dump().size();
            h.bytes -= n;
            historyBytes_ -= n;
            h.events.pop_front();
        }
        while (historyBytes_ > 16 * 1024 * 1024) {
            bool removed = false;
            for (auto &old : histories_)
                if (!old.second.events.empty()) {
                    auto n = old.second.events.front().dump().size();
                    old.second.bytes -= n;
                    historyBytes_ -= n;
                    old.second.events.pop_front();
                    removed = true;
                    break;
                }
            if (!removed)
                break;
        }
    }
}
Json ApiServer::EventsLocked(const std::string &target, std::uint64_t after) {
    auto it = histories_.find(target);
    if (it == histories_.end())
        return {{"events", Json::array()}, {"cursor", 0}, {"gap", after != 0}};
    auto &h = it->second;
    bool gap =
        after > h.seq || (h.events.empty() ? after < h.seq : after + 1 < h.events.front()["seq"].get<std::uint64_t>());
    Json events = Json::array();
    if (!gap)
        for (auto &e : h.events)
            if (e["seq"].get<std::uint64_t>() > after)
                events.push_back(e);
    // History retrieval is bounded by the message limit; subscription streams individual frames.
    return {{"events", events}, {"cursor", h.seq}, {"gap", gap}};
}
Json ApiServer::Events(const std::string &target, std::uint64_t after) {
    std::lock_guard<std::mutex> l(mutex_);
    auto result = EventsLocked(target, after);
    if (result.dump().size() > ws::MaxMessage)
        return Failure("HISTORY_TOO_LARGE", "Use subscribe for bounded streamed history");
    return result;
}
} // namespace serialctl
