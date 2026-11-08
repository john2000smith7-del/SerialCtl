#include "GatewayFixture.h"
#include "GatewayHistoryBoundary.h"
#include "SharedSerialConnection.h"
#include <future>
namespace serialctl {
struct ApiServerTestAccess {
    static std::unique_lock<std::mutex> HoldPublication(ApiServer& server) {
        return std::unique_lock<std::mutex>(server.mutex_);
    }
};
}
using namespace test;
int stage = 0;
int main() {
    Wsa wsa;
    try {
        TestHistoryMessageBoundaries();
        Fixture f;
        {
            const auto before = f.server.Events("session-3", 0)["cursor"].get<std::uint64_t>();
            auto gate = ApiServerTestAccess::HoldPublication(f.server);
            auto producer = std::async(std::launch::async, [&] {
                for (unsigned i = 0; i < 16; ++i)
                    if (!f.server.EnqueuePublish("session-3", Bytes{static_cast<std::uint8_t>(i)},
                                                 i % 2 ? "device" : "gui", i % 2 ? "output" : "input", 936))
                        return false;
                return true;
            });
            struct ReleaseBeforeJoin {
                std::unique_lock<std::mutex>& gate;
                ~ReleaseBeforeJoin() { if (gate.owns_lock()) gate.unlock(); }
            } release{gate};
            Expect(producer.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
                   "publication handoff must not acquire server fan-out mutex");
            Expect(producer.get(), "bounded publication handoff accepts ordinary input/output");
            gate.unlock();
            Wait([&] { return f.server.Events("session-3", before)["cursor"] == before + 16; });
            const auto events = f.server.Events("session-3", before)["events"];
            for (unsigned i = 0; i < 16; ++i)
                Expect(events[i]["seq"] == before + i + 1 && events[i]["codePage"] == 936 &&
                           Decode64(events[i]["data"]) == Bytes{static_cast<std::uint8_t>(i)} &&
                           events[i]["kind"] == (i % 2 ? "output" : "input"),
                       "asynchronous publications preserve bytes, metadata and input/output order");
            const auto cursor = before + 16;
            Expect(!f.server.EnqueuePublish("session-3", Bytes(8 * 1024 * 1024 + 1, 'x')),
                   "publication queue has a hard memory bound");
            Wait([&] { return f.server.Events("session-3", cursor)["gap"] == true; });
            Expect(f.server.Events("session-3", cursor)["events"].empty(),
                   "publication overflow must explicitly invalidate history continuity");
            Expect(f.server.EnqueuePublish("session-3", Bytes{'r'}), "publication resumes after an explicit gap");
            Wait([&] { return f.server.Events("session-3", cursor + 1)["events"].size() == 1; });
        }
        {
            Fixture registry(19040, 19055);
            auto gate = ApiServerTestAccess::HoldPublication(registry.server);
            auto producer = std::async(std::launch::async, [&] {
                return registry.server.EnqueueSessions({"session-3", "session-5", "session-99"}) &&
                       registry.server.EnqueuePublish("session-99", Bytes{'z'});
            });
            struct ReleaseGate {
                std::unique_lock<std::mutex> &lock;
                ~ReleaseGate() { if (lock.owns_lock()) lock.unlock(); }
            } release{gate};
            Expect(producer.wait_for(std::chrono::seconds(1)) == std::future_status::ready && producer.get(),
                   "session registration must not wait for the server fan-out lock");
            gate.unlock();
            Wait([&] { return registry.server.Events("session-99", 0)["events"].size() == 1; });
            Expect(Decode64(registry.server.Events("session-99", 0)["events"][0]["data"]) == Bytes{'z'},
                   "session registration must precede its first queued bytes");
            Expect(registry.server.EnqueueSessions({"session-3", "session-5"}) &&
                   registry.server.EnqueuePublish("session-3", Bytes{'b'}),
                   "queue session removal and ordering barrier");
            Wait([&] { return registry.server.Events("session-3", 0)["events"].size() == 1; });
            Expect(registry.server.Events("session-99", 0)["events"].empty(),
                   "queued session removal must retire its history before later output");
            auto overflowGate = ApiServerTestAccess::HoldPublication(registry.server);
            Expect(registry.server.EnqueueSessions({"session-3", "session-5", "session-99"}) &&
                   !registry.server.EnqueuePublish("session-99", Bytes(8 * 1024 * 1024 + 1, 'x')),
                   "bounded queue must explicitly reject an oversized output");
            overflowGate.unlock();
            Wait([&] { return registry.server.Events("session-99", 0)["gap"] == true; });
            Expect(registry.server.EnqueuePublish("session-99", Bytes{'r'}),
                   "the latest session snapshot must survive a publication gap");
            Wait([&] { return registry.server.Events("session-99", 1)["events"].size() == 1; });
        }
        Peer p(f.server.Port());
        int seq = 0;
        auto request = [&](const std::string &op, const std::string &id, const Json &params = Json::object()) {
            std::cerr << "request-stage=" << ++stage << " op=" << op << std::endl;
            return p.Request(op, id, params, std::to_string(++seq));
        };
        Expect(request("resources", "")["ok"] == true, "one service resource query");
        for (auto op : {"connect", "disconnect", "ssh", "sftp", "power.parameters", "power.mode", "power.scpi",
                        "power.task", "power.reset"})
            Expect(request(op, "power-1")["ok"] == false, "unknown/unauthorized operation rejected");
        for (const auto &body : {Json{{"channels", {2}}, {"enabled", true}}, Json{{"enabled", "false"}},
                                 Json{{"enabled", true}, {"voltage", 1}}, Json{{"enabled", true}, {"requestId", "x"}}})
            Expect(request("power.output", "power-1", body)["ok"] == false, "power parameter prevalidation");
        Expect(f.powerWrites == 0, "rejected request has no backend effects");
        auto j = p.Envelope("input", "session-3", {{"data", "QQ=="}}, "wrong-instance");
        j["instance"] = "wrong";
        p.Send(j);
        Expect(p.Receive()["error"]["code"] == "INSTANCE_MISMATCH", "instance mismatches rejected");
        Expect(request("input", "session-2", {{"data", "QQ=="}})["ok"] == false, "forged session rejected");
        Expect(request("unsubscribe", "session-2")["error"]["code"] == "TARGET_NOT_FOUND",
               "forged unsubscribe target rejected");
        for (const auto &field : {Json{{"version", 1.0}}, Json{{"resource", Json::array()}},
                                  Json{{"params", Json::array()}}, Json{{"source", "gui"}}}) {
            auto invalid = p.Envelope("input", "session-3", {{"data", "QQ=="}}, "invalid-" + std::to_string(++seq));
            invalid.update(field);
            p.Send(invalid);
            Expect(p.Receive()["error"]["code"] == "INVALID_REQUEST", "illegal types or excess fields prevalidated");
        }
        Expect(request("input", "session-3", {{"data", "Q!=="}})["ok"] == false,
               "invalid Base64 rejected before driver");
        Expect(request("input", "session-3", {{"data", Encode64(Bytes(65537, 1))}})["ok"] == false,
               "oversize input rejected");
        Expect(f.rx3->received == 0, "invalid inputs never enter device");
        auto action = request("power.output", "power-1", {{"enabled", true}});
        Expect(action["result"]["state"] == "queued" && f.powerWrites == 1, "queued is distinct from completed action");
        auto duplicate = p.Envelope("input", "session-3", {{"data", Encode64(Bytes{0, 255, 13, 10})}}, "duplicate");
        p.Send(duplicate);
        auto reply = p.Receive();
        p.Send(duplicate);
        Expect(p.Receive() == reply, "same connection requestId returns cached response");
        Wait([&] { return f.rx3->received == 4; });
        Expect(f.rx3->received == 4, "duplicate does not execute twice");
        duplicate["params"]["data"] = "QQ==";
        p.Send(duplicate);
        Expect(p.Receive()["error"]["code"] == "REQUEST_ID_CONFLICT", "conflicting requestId never reexecutes");
        // RFC fragments, control interleaving, coalesced packets and exact binary ingress.
        auto msg =
            p.Envelope("input", "session-5", {{"data", Encode64(Bytes{255, 244, 255, 0, 13, 10})}}, "fragment").dump();
        Bytes a(msg.begin(), msg.begin() + msg.size() / 2), b(msg.begin() + msg.size() / 2, msg.end());
        p.stream->Send(1, a, false);
        p.stream->Send(9, Bytes{1, 2, 3});
        auto pong = p.stream->Receive();
        Expect(pong.opcode == 10 && pong.data == Bytes({1, 2, 3}), "Ping/Pong not input");
        p.stream->Send(0, b);
        Expect(p.Receive()["ok"] == true, "fragmented JSON accepted");
        Wait([&] { return f.rx5->received == 6; });
        {
            std::lock_guard<std::mutex> lock(f.rx5->mutex);
            Expect(f.rx5->bytes == Bytes({255, 244, 255, 0, 13, 10}), "legal FF F4 FF never filtered");
        }
        auto illegal = [&](const Bytes &frame, unsigned expected) {
            std::cerr << "illegal-stage=" << ++stage << " expected=" << expected << " frame=" << Encode64(frame)
                      << std::endl;
            Peer bad(f.server.Port());
            ws::SendAll(bad.socket, frame.data(), frame.size());
            auto close = bad.stream->Receive();
            Expect(close.opcode == 8 && close.data.size() >= 2 &&
                       ((unsigned(close.data[0]) << 8) | close.data[1]) == expected,
                   "illegal RFC frame explicit close");
        };
        illegal(Bytes{0x82, 0x80, 1, 2, 3, 4}, 1003);          // JSON-only application frames
        illegal(Bytes{0x81, 0xfe, 0, 1, 1, 2, 3, 4, 0}, 1002); // canonical length
        illegal(Bytes{0x88, 0x82, 1, 2, 3, 4, std::uint8_t(3 ^ 1), std::uint8_t(0xed ^ 2)},
                1002);                                            // forbidden Close 1005
        illegal(Bytes{0x81, 0}, 1002);                            // client frames must be masked
        illegal(Bytes{0x89, 0xfe, 0, 126}, 1002);                 // control payload too long
        illegal(Bytes{0x80, 0x80, 1, 2, 3, 4}, 1002);             // continuation without message
        illegal(Bytes{0xc1, 0x80, 1, 2, 3, 4}, 1002);             // RSV
        illegal(Bytes{0x81, 0xff, 0, 0, 0, 0, 0, 2, 0, 1}, 1009); // >128 KiB
        illegal(Bytes{0x81, 0x81, 1, 2, 3, 4, 0xff ^ 1}, 1007);   // UTF8
        illegal(Bytes{0x88, 0x81, 1, 2, 3, 4, 0}, 1002);          // invalid close length
        {
            Peer close(f.server.Port());
            close.stream->Send(8, ws::ClosePayload(1000, "done"));
            Expect(close.stream->Receive().opcode == 8, "close handshake");
        }
        // No HTTP REST or serial raw fallback. Old bytes cannot reach a device.
        for (auto text : {"SERIALCTL/1 LIST\n", "SERIALCTL/2 OPEN COM3\n", "SERIALCTL/3 DISCOVER\n",
                          "GET /api/v1/resources HTTP/1.1\r\nHost: localhost\r\n\r\n",
                          "POST /api/v1/sessions/session-3/input HTTP/1.1\r\n\r\n", "telnet\xff\xf4\xff"}) {
            std::wstring error;
            auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), error);
            ws::SocketOptions(s, 3000);
            ws::SendAll(s, text, strlen(text));
            char buffer[512]{};
            auto n = recv(s, buffer, sizeof(buffer), 0);
            Expect(n > 0 && std::string(buffer, n).find("426") != std::string::npos,
                   "legacy gets explicit upgrade error");
            closesocket(s);
        }
        Expect(f.rx3->received == 4 && f.rx5->received == 6, "old/control traffic never device input");
        {
            std::wstring error;
            auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), error);
            ws::SocketOptions(s, 3000);
            const auto header = "GET /serialctl HTTP/1.1\r\nX-Large: " + std::string(9000, 'x') + "\r\n\r\n";
            ws::SendAll(s, header.data(), header.size());
            char response[512]{};
            const int n = recv(s, response, sizeof(response), 0);
            closesocket(s);
            Expect(n > 0 && std::string(response, n).find("426") != std::string::npos,
                   "coalesced terminator must not bypass the 8192-byte header bound");
        }
        {
            // Every fragment arrives inside SO_RCVTIMEO. Only an absolute
            // handshake deadline can reject this never-ending partial header.
            std::wstring error;
            auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), error);
            ws::SocketOptions(s, 3000);
            const std::string partial = "GET /serialctl HTTP/1.1\r\n";
            ws::SendAll(s, partial.data(), partial.size());
            std::atomic_bool done{false};
            std::thread drip([&] {
                while (!done) {
                    Sleep(120);
                    const char fragment[] = "X-Keep: alive\r\n";
                    if (done || send(s, fragment, sizeof(fragment) - 1, 0) <= 0) break;
                }
            });
            const auto start = GetTickCount64();
            char response[512]{};
            const int n = recv(s, response, sizeof(response), 0);
            const auto elapsed = GetTickCount64() - start;
            done = true;
            drip.join();
            closesocket(s);
            Expect(n > 0 && std::string(response, n).find("426") != std::string::npos && elapsed < 2500,
                   "trickled headers must expire at the total handshake deadline");
            std::cout << "Slow header rejected after " << elapsed << " ms\n";
        }
        {
            std::wstring error;
            auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(f.server.Port()), error);
            ws::SocketOptions(s, 3000);
            const std::string header =
                "GET /serialctl HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: "
                "Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: serialctl.v1\r\nSec-WebSocket-Key: "
                "dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            auto payload = p.Envelope("input", "session-5", {{"data", "AA=="}}, "coalesced-first-byte").dump();
            Bytes packet(header.begin(), header.end());
            packet.push_back(0x81);
            if (payload.size() < 126)
                packet.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
            else {
                packet.push_back(0xfe);
                packet.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
                packet.push_back(static_cast<std::uint8_t>(payload.size()));
            }
            packet.insert(packet.end(), {1, 2, 3, 4});
            for (size_t i = 0; i < payload.size(); ++i)
                packet.push_back(static_cast<std::uint8_t>(payload[i] ^ (1 + i % 4)));
            ws::SendAll(s, packet.data(), packet.size());
            Bytes tail;
            Expect(ws::ReadHeader(s, tail).first == "HTTP/1.1 101 Switching Protocols", "coalesced upgrade accepted");
            ws::Stream stream(s, true, std::move(tail));
            Expect(Json::parse(stream.Receive().data)["type"] == "hello", "hello precedes coalesced request reply");
            Expect(Json::parse(stream.Receive().data)["ok"] == true, "first masked request retained after header");
            Wait([&] { return f.rx5->received == 7; });
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        {
            Peer subscription(f.server.Port());
            auto cursor = f.server.Events("session-5", 0)["cursor"].get<std::uint64_t>();
            Expect(subscription.Request("subscribe", "session-5", {{"after", cursor}}, "sub")["ok"] == true,
                   "cursor subscription");
            f.server.Publish("session-5", Bytes{'z'});
            auto event = subscription.Receive();
            Expect(event["seq"] == cursor + 1 && Decode64(event["data"]) == Bytes{'z'}, "ordered resource cursor");
            Expect(subscription.Request("unsubscribe", "session-5", Json::object(), "unsub")["ok"] == true,
                   "unsubscribe acknowledged");
            f.server.Publish("session-5", Bytes{'y'});
            subscription.Send(subscription.Envelope("resources", "", Json::object(), "after-unsub"));
            Expect(subscription.Receive()["type"] == "response", "no new output queued after unsubscribe");
        }
        for (int i = 0; i < 140; ++i) {
            GatewayClient client;
            client.Connect(L"127.0.0.1", f.server.Port());
            Expect(client.Request("resources").contains("resources"), "finished clients recycled");
        }
        for (int i = 0; i < 1100; ++i)
            f.server.Publish("session-3", Bytes{1});
        Expect(f.server.Events("session-3", 1)["gap"] == true, "expired history explicitly reports gap");
        f.Remove3();
        Expect(request("session.get", "session-3")["ok"] == false, "target disappearance");
        std::cerr << "stage=" << ++stage << " stopping first server" << std::endl;
        std::mutex stopMutex;
        std::condition_variable stopSignal;
        bool stopComplete = false;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lock(stopMutex);
            if (!stopSignal.wait_for(lock, std::chrono::seconds(5), [&] { return stopComplete; }))
                std::cerr << "Stop watchdog diagnostics=" << f.server.Diagnostics().dump() << std::endl;
        });
        f.server.Stop();
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopComplete = true;
            stopSignal.notify_one();
        }
        watchdog.join();
        std::cerr << "stage=" << ++stage << " first server stopped" << std::endl;
        {
            std::cerr << "stage=" << ++stage << " capacity fixture" << std::endl;
            Fixture capacity(18900, 18915);
            std::vector<std::unique_ptr<GatewayClient>> held;
            for (size_t i = 0; i < ApiServer::MaxClients; ++i) {
                auto client = std::make_unique<GatewayClient>();
                client->Connect(L"127.0.0.1", capacity.server.Port());
                held.push_back(std::move(client));
                if (held.size() % 16 == 0)
                    std::cerr << "capacity=" << held.size() << std::endl;
            }
            Expect(held.size() == 128, "128 simultaneous connections retained");
            bool refused = false;
            try {
                GatewayClient extra;
                extra.Connect(L"127.0.0.1", capacity.server.Port());
            } catch (const std::exception &e) {
                refused = std::string(e.what()).find("CLIENT_LIMIT") != std::string::npos;
            }
            Expect(refused, "client 129 explicitly refused at capacity");
            std::uint16_t port = 0;
            std::vector<std::wstring> names;
            std::wstring error;
            Expect(!SharedSerialConnection::DiscoverAuto(L"127.0.0.1", port, names, error, nullptr,
                                                         static_cast<std::uint16_t>(capacity.server.Port())) &&
                       error.find(L"CLIENT_LIMIT") != std::wstring::npos,
                   "native discovery preserves explicit capacity refusal");
            held.clear();
            std::cerr << "stage=" << ++stage << " capacity clients cleared" << std::endl;
        }
        {
            std::cerr << "stage=" << ++stage << " testing bind, allowlist, CSWSH origin and metrics" << std::endl;
            Fixture secureFixture(18920, 18935);
            auto &secServer = secureFixture.server;

            // 1. IP Allowlist
            secServer.SetAllowedClients({"192.168.1.100"}); // exclude loopback 127.0.0.1
            {
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                Expect(s != INVALID_SOCKET, "connect to allowlist test server");
                char buf[512]{};
                int n = recv(s, buf, sizeof(buf), 0);
                closesocket(s);
                Expect(n > 0 && std::string(buf, n).find("403 Forbidden") != std::string::npos &&
                       std::string(buf, n).find("CLIENT_NOT_ALLOWED") != std::string::npos,
                       "unlisted IP is rejected with HTTP 403 CLIENT_NOT_ALLOWED");
            }
            secServer.SetAllowedClients({"127.0.0.1"});
            {
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                Expect(s != INVALID_SOCKET, "connect with allowed IP");
                closesocket(s);
            }
            secServer.SetAllowedClients({}); // empty allows all

            // 2. CSWSH Origin validation
            {
                // Disallowed origin
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                ws::SocketOptions(s, 2000);
                std::string req =
                    "GET /serialctl HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: serialctl.v1\r\n"
                    "Origin: https://evil.attacker.com\r\n\r\n";
                ws::SendAll(s, req.data(), req.size());
                char buf[512]{};
                int n = recv(s, buf, sizeof(buf), 0);
                closesocket(s);
                Expect(n > 0 && std::string(buf, n).find("403 Forbidden") != std::string::npos &&
                       std::string(buf, n).find("ORIGIN_NOT_ALLOWED") != std::string::npos,
                       "untrusted Origin rejected with HTTP 403 ORIGIN_NOT_ALLOWED");
            }
            {
                // Disallowed subdomain spoof origin (https://localhost.evil.com)
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                ws::SocketOptions(s, 2000);
                std::string req =
                    "GET /serialctl HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: serialctl.v1\r\n"
                    "Origin: https://localhost.evil.com\r\n\r\n";
                ws::SendAll(s, req.data(), req.size());
                char buf[512]{};
                int n = recv(s, buf, sizeof(buf), 0);
                closesocket(s);
                Expect(n > 0 && std::string(buf, n).find("403 Forbidden") != std::string::npos &&
                       std::string(buf, n).find("ORIGIN_NOT_ALLOWED") != std::string::npos,
                       "spoofed localhost.evil.com Origin rejected with HTTP 403 ORIGIN_NOT_ALLOWED");
            }
            {
                // Allowed origin (localhost)
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                ws::SocketOptions(s, 2000);
                std::string req =
                    "GET /serialctl HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: serialctl.v1\r\n"
                    "Origin: http://localhost:8080\r\n\r\n";
                ws::SendAll(s, req.data(), req.size());
                char buf[512]{};
                int n = recv(s, buf, sizeof(buf), 0);
                closesocket(s);
                Expect(n > 0 && std::string(buf, n).find("101 Switching Protocols") != std::string::npos,
                       "localhost Origin accepted");
            }
            {
                // Allowed origin (127.0.0.1)
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                ws::SocketOptions(s, 2000);
                std::string req =
                    "GET /serialctl HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: serialctl.v1\r\n"
                    "Origin: http://127.0.0.1:3000\r\n\r\n";
                ws::SendAll(s, req.data(), req.size());
                char buf[512]{};
                int n = recv(s, buf, sizeof(buf), 0);
                closesocket(s);
                Expect(n > 0 && std::string(buf, n).find("101 Switching Protocols") != std::string::npos,
                       "127.0.0.1 Origin accepted");
            }

            for (const auto& authority : {"rebound.example", "127.0.0.1:0", "localhost/other"}) {
                std::wstring error;
                auto s = ConnectTcpSocket(L"127.0.0.1", static_cast<std::uint16_t>(secServer.Port()), error);
                Expect(s != INVALID_SOCKET, "connect for browser Host rejection");
                ws::SocketOptions(s, 2000);
                const std::string upgradeRequest = std::string("GET /serialctl HTTP/1.1\r\nHost: ") + authority +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: serialctl.v1\r\nOrigin: http://localhost:8080\r\n\r\n";
                Expect(ws::SendAll(s, upgradeRequest.data(), upgradeRequest.size()), "send browser Host probe");
                char response[512]{};
                const int count = recv(s, response, sizeof(response), 0);
                closesocket(s);
                Expect(count > 0 && std::string(response, count).find("403 Forbidden") != std::string::npos,
                       "browser handshake must reject untrusted or invalid Host before upgrade");
            }
            // 3. Metrics() and O(1) History Eviction Fairness across 18 sessions (> 16 MiB global cap)
            auto metrics0 = secServer.Metrics();
            Expect(metrics0.contains("publishedEvents") && metrics0.contains("historyTrims") &&
                   metrics0.contains("publicationHighWaterMark") && metrics0.contains("historyBytes") &&
                   metrics0.contains("historyEvents") && metrics0.contains("rejectedClients") &&
                   metrics0.contains("rejectedOrigins"),
                   "Metrics contains all required observability keys");
            Expect(metrics0["rejectedClients"].get<uint64_t>() >= 1 &&
                   metrics0["rejectedOrigins"].get<uint64_t>() >= 2,
                   "Metrics tracks rejected client IPs and origins");

            std::vector<std::string> manySessions = {"session-3", "session-5"};
            for (int sIdx = 10; sIdx < 28; ++sIdx)
                manySessions.push_back("session-" + std::to_string(sIdx));
            secServer.SetSessions(manySessions);

            // Publish ~950 KB to each of 18 hot sessions (total ~17.1 MiB > 16 MiB global limit)
            for (int sIdx = 10; sIdx < 28; ++sIdx) {
                const std::string resName = "session-" + std::to_string(sIdx);
                for (int i = 0; i < 60; ++i)
                    secServer.Publish(resName, Bytes(12000, 'H'));
            }
            // Publish moderately to cold session-5
            for (int i = 0; i < 20; ++i) {
                secServer.Publish("session-5", Bytes(200, 'B'));
            }

            auto metrics1 = secServer.Metrics();
            Expect(metrics1["publishedEvents"].get<uint64_t>() >= 1100, "metrics tracks published events");
            Expect(metrics1["historyTrims"].get<uint64_t>() > 0, "metrics tracks history trims");
            Expect(metrics1["historyBytes"].get<size_t>() <= 16 * 1024 * 1024, "history bytes bounded by 16MB");
            auto events5 = secServer.Events("session-5", 0);
            Expect(!events5["gap"].get<bool>(), "smaller cold session not trimmed when 16MB largest-first fairness applies");

            // 4. WebSocket stream multi-continuation fragmentation & interleaved control (Issue #22)
            {
                Peer fragPeer(secServer.Port());
                std::string reqBody = fragPeer.Envelope("resources", "", Json::object(), "multi-frag").dump();
                size_t part1 = reqBody.size() / 3;
                size_t part2 = reqBody.size() / 3;
                Bytes b1(reqBody.begin(), reqBody.begin() + part1);
                Bytes b2(reqBody.begin() + part1, reqBody.begin() + part1 + part2);
                Bytes b3(reqBody.begin() + part1 + part2, reqBody.end());

                fragPeer.stream->Send(1, b1, false); // Opcode 1, FIN=0
                fragPeer.stream->Send(9, Bytes{'h', 'i'}); // Interleaved Ping
                auto pongResp = fragPeer.stream->Receive();
                Expect(pongResp.opcode == 10 && pongResp.data == Bytes({'h', 'i'}), "Ping processed during multi-continuation");
                fragPeer.stream->Send(0, b2, false); // Opcode 0, FIN=0
                fragPeer.stream->Send(0, b3, true);  // Opcode 0, FIN=1
                auto res = fragPeer.Receive();
                Expect(res["type"] == "response" && res["requestId"] == "multi-frag" && res["ok"] == true &&
                       res["result"].contains("resources"),
                       "multi-continuation frame reassembly succeeds");

                // Oversized continuation exceeding MaxMessage triggers 1009
                fragPeer.stream->Send(1, Bytes(60000, 'x'), false);
                fragPeer.stream->Send(0, Bytes(75000, 'y'), true); // total 135000 > 128KB
                auto closeMsg = fragPeer.stream->Receive();
                Expect(closeMsg.opcode == 8, "oversized continuation stream closed with code 1009");
                unsigned code = (static_cast<unsigned>(closeMsg.data[0]) << 8) | closeMsg.data[1];
                Expect(code == 1009, "close code is 1009 MESSAGE_TOO_BIG");
            }
        }
        std::cout << "Unified gateway scope, RFC6455, legacy rejection, byte integrity, replay and lifecycle passed\n";
    } catch (const std::exception &e) {
        std::cerr << "stage=" << stage << " " << e.what() << '\n';
        return 1;
    }
    return 0;
}
