#pragma once
#include "ApiServer.h"
#include "GatewayClient.h"
#include "NetworkConnect.h"
#include "QueuedConnection.h"
#include "SessionService.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>
namespace test {
using namespace serialctl;
inline void Expect(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Wsa {
    Wsa() {
        WSADATA w{};
        Expect(WSAStartup(MAKEWORD(2, 2), &w) == 0, "Winsock startup");
    }
    ~Wsa() {
        WSACleanup();
    }
};
class Echo : public IConnection {
  public:
    std::atomic_bool connected{false};
    std::atomic<size_t> received{0};
    std::mutex mutex;
    Bytes bytes;
    bool Start(DataCallback data, StatusCallback, std::wstring &) override {
        data_ = std::move(data);
        connected = true;
        return true;
    }
    void Stop() override {
        connected = false;
    }
    bool IsConnected() const override {
        return connected;
    }
    bool Send(const Bytes &b, std::wstring &) override {
        {
            std::lock_guard<std::mutex> l(mutex);
            bytes.insert(bytes.end(), b.begin(), b.end());
        }
        received += b.size();
        if (data_)
            data_(b);
        return true;
    }

  private:
    DataCallback data_;
};
struct Fixture {
    ApiServer server;
    SessionService service;
    std::shared_ptr<QueuedConnection> com3, com5, cmd;
    Echo *rx3 = nullptr, *rx5 = nullptr;
    std::atomic<size_t> powerWrites{0};
    Fixture(unsigned first = 18000, unsigned last = 18015) {
        std::wstring error;
        Expect(server.Start(
                   [&](const std::string &op, const std::string &id, const Json &p) -> Json {
                       if (op == "resources") {
                           auto r = service.Resources();
                           r.push_back({{"id", "power-1"}, {"kind", "power"}, {"connected", false}});
                           return {{"resources", r}};
                       }
                       if (op == "input")
                           return service.Input(id, Decode64(p["data"]), p.value("source", "test"));
                       if (op == "session.get")
                           return service.State(id);
                       if (op == "power.output") {
                           ++powerWrites;
                           return {{"id", "action-1"}, {"state", "queued"}, {"requestId", p["requestId"]}};
                       }
                       if (op == "power.get")
                           return {{"selectedChannels", {1, 3}}, {"connected", false}};
                       if (op == "action.get")
                           return {{"id", p["id"]}, {"state", "completed"}};
                       return {{"error", {{"code", "OPERATION_DENIED"}}}};
                   },
                   error, first, last),
               "start production gateway");
        service.ObserveInput([&](const std::string &id, const Bytes &b, const std::string &source) {
            server.Publish(id, b, source.c_str(), "input");
        });
        com3 = Open("session-3", "COM3", rx3);
        com5 = Open("session-5", "COM5", rx5);
    }
    std::shared_ptr<QueuedConnection> Open(const std::string &id, const std::string &com, Echo *&rx) {
        auto e = std::make_unique<Echo>();
        rx = e.get();
        auto c = std::make_shared<QueuedConnection>(std::move(e));
        service.Register({{"id", id}, {"kind", "serial"}, {"com", com}, {"name", com}, {"baudRate", 115200}}, c);
        server.SetSessions(service.Ids());
        std::wstring error;
        Expect(c->Start([this, id](const Bytes &b) { server.Publish(id, b); }, [](const auto &, bool) {}, error),
               "start controlled serial substitute");
        return c;
    }
    void Remove3() {
        service.Unregister("session-3");
        server.SetSessions(service.Ids());
        com3->Stop();
    }
    ~Fixture() {
        server.Stop();
        com3->Stop();
        com5->Stop();
    }
};
struct Peer {
    SOCKET socket = INVALID_SOCKET;
    std::unique_ptr<ws::Stream> stream;
    Json hello;
    explicit Peer(unsigned port) {
        std::wstring error;
        socket = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(port), error);
        Expect(socket != INVALID_SOCKET, "peer connect");
        auto tail = ws::UpgradeClient(socket, L"127.0.0.1", port);
        stream = std::make_unique<ws::Stream>(socket, true, std::move(tail));
        hello = Receive();
    }
    ~Peer() {
        if (socket != INVALID_SOCKET) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
    }
    Json Receive() {
        auto m = stream->Receive();
        if (m.opcode != 1)
            throw std::runtime_error("Expected JSON text");
        return Json::parse(m.data.begin(), m.data.end());
    }
    Json Envelope(const std::string &op, const std::string &target, const Json &params, const std::string &id = "r") {
        Json j = {{"type", "request"}, {"version", 1}, {"instance", hello["instance"]},
                  {"requestId", id},   {"op", op},     {"params", params}};
        if (!target.empty())
            j["resource"] = target;
        return j;
    }
    void Send(const Json &j) {
        auto s = j.dump();
        Expect(stream->Send(1, Bytes(s.begin(), s.end())), "send request");
    }
    Json Request(const std::string &op, const std::string &target = "", const Json &params = Json::object(),
                 const std::string &id = "r") {
        Send(Envelope(op, target, params, id));
        for (;;) {
            auto j = Receive();
            if (j["type"] == "response")
                return j;
        }
    }
};
inline SOCKET Reserve(unsigned port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char *>(&yes), sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(s, 8)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}
inline void Wait(std::function<bool()> predicate) {
    auto deadline = GetTickCount64() + 5000;
    while (!predicate() && GetTickCount64() < deadline)
        Sleep(1);
    Expect(predicate(), "controlled wait timed out");
}
} // namespace test
