#include "GatewayFixture.h"
using namespace test;
int stage = 0;
int main() {
    Wsa wsa;
    try {
        Fixture f;
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
            held.clear();
            std::cerr << "stage=" << ++stage << " capacity clients cleared" << std::endl;
        }
        std::cout << "Unified gateway scope, RFC6455, legacy rejection, byte integrity, replay and lifecycle passed\n";
    } catch (const std::exception &e) {
        std::cerr << "stage=" << stage << " " << e.what() << '\n';
        return 1;
    }
    return 0;
}
