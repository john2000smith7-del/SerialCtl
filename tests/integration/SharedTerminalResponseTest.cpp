#include "GatewayFixture.h"
#include "SharedSerialConnection.h"
#include "TerminalModel.h"
#include <algorithm>

using namespace test;

namespace {
// A controlled COM substitute: RX is injected explicitly and TX is recorded,
// rather than echoed back as another device query. The gateway, FIFO and
// desktop client are production implementations.
class RecordingSerial final : public IConnection {
  public:
    bool Start(DataCallback data, StatusCallback, std::wstring &) override {
        output = std::move(data);
        connected = true;
        return true;
    }
    void Stop() override { connected = false; }
    bool IsConnected() const override { return connected; }
    bool Send(const Bytes &data, std::wstring &) override {
        std::lock_guard<std::mutex> lock(mutex);
        written.insert(written.end(), data.begin(), data.end());
        return true;
    }
    Bytes Written() {
        std::lock_guard<std::mutex> lock(mutex);
        return written;
    }
    DataCallback output;
  private:
    std::atomic_bool connected{false};
    std::mutex mutex;
    Bytes written;
};

struct Viewer {
    TerminalModel model;
    SharedSerialConnection connection;
    std::atomic<size_t> received{0};
    std::atomic_bool failed{false};
    Viewer(unsigned port, int columns, int rows)
        : model(columns, rows), connection(L"127.0.0.1", static_cast<std::uint16_t>(port), L"COM3") {
        model.SetResponseRole(TerminalModel::ResponseRole::Viewer);
    }
    ~Viewer() { connection.Stop(); }
    void Start() {
        received = 0;
        std::wstring error;
        Expect(connection.Start([this](const Bytes &bytes) {
            const auto result = model.Feed(std::wstring(bytes.begin(), bytes.end()), L"");
            // Same synthesized-response dispatch as the desktop. Do not just
            // discard responses in this test: a lost Viewer role must fail TX.
            if (!result.response.empty()) {
                std::wstring ignored;
                if (!connection.Send(Bytes(result.response.begin(), result.response.end()), ignored))
                    failed = true;
            }
            received += bytes.size();
        }, [this](const auto &, bool error) { if (error) failed = true; }, error), "start shared viewer");
    }
    void Barrier(RecordingSerial &serial, Bytes &expected, unsigned char marker) {
        const Bytes input{0xf1, marker, 0};
        expected.insert(expected.end(), input.begin(), input.end());
        std::wstring error;
        Expect(connection.Send(input, error), "manual input remains allowed for viewers");
        // This marker follows any automatic reply on this client's socket and
        // the device FIFO. Its presence makes the no-extra-TX check ordered.
        Wait([&] {
            auto tx = serial.Written();
            return std::search(tx.begin(), tx.end(), input.begin(), input.end()) != tx.end();
        });
        Expect(serial.Written() == expected && !failed, "viewer live/replay adds zero automatic TX");
    }
};

void Run() {
    const std::string query = "\x1b[5n\x1b[6n\x1b[c\x1b[>c\x1b[18t\x1b[19t";
    const Bytes rx(query.begin(), query.end());
    const std::string reply = "\x1b[0n\x1b[1;1R\x1b[?1;2c\x1b[>0;136;0c\x1b[8;24;80t\x1b[8;24;80t";
    const Bytes ownerReply(reply.begin(), reply.end());
    TerminalModel owner(80, 24);
    SessionService service;
    ApiServer server;
    auto recording = std::make_unique<RecordingSerial>();
    auto *serial = recording.get();
    auto local = std::make_shared<QueuedConnection>(std::move(recording));
    service.Register({{"id", "session-3"}, {"kind", "serial"}, {"name", "COM3"}, {"com", "COM3"}}, local);
    std::wstring error;
    Expect(server.Start([&](const std::string &op, const std::string &id, const Json &params) -> Json {
        if (op == "resources") return {{"resources", service.Resources()}};
        if (op == "session.get") return service.State(id);
        if (op == "input") return service.Input(id, Decode64(params.at("data")), "network");
        return {{"error", {{"code", "OPERATION_DENIED"}}}};
    }, error, 18400, 18415), "start production gateway");
    server.SetSessions(service.Ids());
    Expect(local->Start([&](const Bytes &bytes) {
        server.Publish("session-3", bytes);
        const auto result = owner.Feed(std::wstring(bytes.begin(), bytes.end()), L"");
        if (!result.response.empty())
            Expect(!service.Input("session-3", Bytes(result.response.begin(), result.response.end()),
                                  "terminal-response").contains("error"), "owner reply admitted");
    }, [](const auto &, bool) {}, error), "start controlled COM owner");

    // Zero remote viewers: exactly the local owner's geometry is returned.
    serial->output(rx);
    Wait([&] { return serial->Written().size() >= ownerReply.size(); });
    Bytes expected = ownerReply;
    Expect(serial->Written() == expected, "zero-viewer query has one owner reply");
    std::cout << "zero_viewers_owner_reply_bytes=" << ownerReply.size() << '\n';

    GatewayClient ai1, ai2;
    for (auto *ai : {&ai1, &ai2}) {
        ai->Connect(L"127.0.0.1", server.Port());
        ai->Request("subscribe", "session-3", {{"after", 0}});
    }
    auto receiveQuery = [&](GatewayClient &ai) {
        for (;;) {
            const auto event = ai.Receive();
            if (event.value("kind", "") == "output") {
                Expect(Decode64(event.at("data")) == rx, "AI receives query bytes unchanged");
                return;
            }
        }
    };
    receiveQuery(ai1);
    receiveQuery(ai2);
    Viewer first(server.Port(), 100, 30), second(server.Port(), 120, 40);
    first.Start();
    Wait([&] { return first.received >= rx.size(); });
    first.Barrier(*serial, expected, 1); // One viewer, historical query only.
    std::cout << "one_viewer_history_extra_reply_bytes=0\n";
    second.Start();
    Wait([&] { return second.received >= rx.size(); });
    second.Barrier(*serial, expected, 2);

    serial->output(rx); // Two desktop viewers and two AI subscribers, live query.
    expected.insert(expected.end(), ownerReply.begin(), ownerReply.end());
    Wait([&] { return first.received >= 2 * rx.size() && second.received >= 2 * rx.size(); });
    Wait([&] { return serial->Written().size() >= expected.size(); });
    receiveQuery(ai1);
    receiveQuery(ai2);
    first.Barrier(*serial, expected, 3);
    second.Barrier(*serial, expected, 4);
    std::cout << "two_viewers_two_AI_live_owner_reply_bytes=" << ownerReply.size()
              << " extra_reply_bytes=0\n";

    first.connection.Stop();
    first.model.Resize(132, 50);
    first.model.Clear();
    first.model.Reset();
    first.Start();
    Wait([&] { return first.received >= 2 * rx.size(); });
    first.Barrier(*serial, expected, 5); // Reconnect replays both old queries.
    std::cout << "resized_reset_reconnect_two_historical_queries_extra_reply_bytes=0\n";

    Bytes binary;
    for (unsigned value = 0; value < 256; ++value) binary.push_back(static_cast<unsigned char>(value));
    binary.insert(binary.end(), rx.begin(), rx.end()); // Manual protocol bytes are legal input.
    for (auto *ai : {&ai1, &ai2}) {
        ai->Request("input", "session-3", {{"data", Encode64(binary)}});
        expected.insert(expected.end(), binary.begin(), binary.end());
        Wait([&] { return serial->Written().size() >= expected.size(); });
        Expect(serial->Written() == expected, "AI 0-255, CRLF, FF and query bytes preserved");
    }
    for (auto *viewer : {&first, &second}) {
        Expect(viewer->connection.Send(binary, error), "viewer manual binary input admitted");
        expected.insert(expected.end(), binary.begin(), binary.end());
        Wait([&] { return serial->Written().size() >= expected.size(); });
        Expect(serial->Written() == expected, "viewer manual binary input preserved");
    }
    first.connection.Stop();
    second.connection.Stop();
    local->Stop();
    server.Stop();
    std::cout << "PASS: zero/one/two shared viewers plus two AI; owner-only live TX; replay/reconnect zero TX; "
                 "0-255 and explicit manual query bytes preserved. Controlled COM, no GUI or physical UART.\n";
}
} // namespace

int main() {
    Wsa wsa;
    try { Run(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
