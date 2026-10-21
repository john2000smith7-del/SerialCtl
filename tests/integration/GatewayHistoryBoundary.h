#pragma once
#include "GatewayFixture.h"

namespace test {
inline Json UnboundedHistoryReply(ApiServer &server, const std::string &id, const Json &history) {
    return {{"type", "response"}, {"version", 1}, {"instance", server.Instance()},
            {"requestId", id}, {"ok", true}, {"result", history}};
}

// Use public Publish, actual Base64 and ordinary <=16 KiB output events.
// Only the last source's 0..3 padding bytes tune the exact envelope size.
inline Json PrepareHistoryBoundary(Fixture &fixture, size_t wireBytes, const std::string &id) {
    constexpr auto target = "session-5";
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        fixture.server.SetSessions({});
        fixture.server.SetSessions(fixture.service.Ids());
        for (unsigned n = 1; n <= 5; ++n)
            fixture.server.Publish(target, Bytes(16384, static_cast<unsigned char>(n)));
        const std::string source = "设备";
        auto history = fixture.server.Events(target, 0);
        history["events"].push_back({{"type", "event"}, {"version", 1}, {"instance", fixture.server.Instance()},
            {"resource", target}, {"seq", 6}, {"kind", "output"}, {"source", source},
            {"data", Encode64(Bytes{6})}, {"tick", GetTickCount64()}});
        history["cursor"] = 6;
        auto baseBytes = UnboundedHistoryReply(fixture.server, id, history).dump().size();
        Expect(baseBytes <= wireBytes, "history boundary has payload room");
        auto room = wireBytes - baseBytes;
        const auto rawBytes = 1 + 3 * (room / 4);
        Expect(rawBytes <= 16384, "history boundary uses legal output chunks");
        fixture.server.Publish(target, Bytes(rawBytes, 6), (source + std::string(room % 4, 'x')).c_str());
        history = fixture.server.Events(target, 0);
        if (UnboundedHistoryReply(fixture.server, id, history).dump().size() == wireBytes) return history;
        // A GetTickCount64 digit boundary can change JSON overhead. Rebuild
        // rather than relaxing the exact-byte assertion or sleeping for timing.
    }
    throw std::runtime_error("Could not arrange exact history envelope size");
}

inline void TestHistoryMessageBoundaries() {
    Fixture fixture(18500, 18515);
    for (size_t idBytes : {size_t(1), size_t(128)}) {
        const std::string id(idBytes, 'r');
        for (size_t wireBytes : {ws::MaxMessage - 1, ws::MaxMessage, ws::MaxMessage + 1}) {
            auto expected = PrepareHistoryBoundary(fixture, wireBytes, id);
            Peer peer(fixture.server.Port());
            auto reply = peer.Request("events", "session-5", {{"after", 0}}, id);
            Expect(reply.dump().size() <= ws::MaxMessage, "server emits bounded complete response");
            if (wireBytes <= ws::MaxMessage)
                Expect(reply.at("ok") == true && reply.at("result") == expected,
                       "near-limit native history is complete and unchanged");
            else
                Expect(reply.at("ok") == false && reply.at("error").at("code") == "HISTORY_TOO_LARGE",
                       "native client receives history size business error");
            Expect(peer.Request("events", "session-5", {{"after", 0}}, id) == reply,
                   "dedup caches exactly the bounded response");
            Expect(peer.Request("events", "session-5", {{"after", 1}}, id).at("error").at("code") ==
                       "REQUEST_ID_CONFLICT", "conflicting near-limit request is rejected");
            Expect(peer.Request("resources", "", Json::object(), "follow").at("ok") == true,
                   "same healthy connection remains usable after history result/error");
            Expect(peer.Request("subscribe", "session-5", {{"after", 0}}, "stream").at("ok") == true,
                   "streamed history remains available on same connection");
            for (const auto &event : expected.at("events"))
                Expect(peer.Receive() == event, "every streamed history event preserved in order");
            fixture.server.Publish("session-5", Bytes{'z'});
            Expect(Decode64(peer.Receive().at("data")) == Bytes{'z'}, "live subscription still works");
            std::cout << "native_id_bytes=" << idBytes << " candidate_wire_bytes=" << wireBytes
                      << " emitted_wire_bytes=" << reply.dump().size()
                      << " ok=" << reply.at("ok") << " continued_query_and_subscribe=pass\n";
        }
    }
}
} // namespace test
