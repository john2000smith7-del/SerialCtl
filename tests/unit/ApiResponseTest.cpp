#include "ApiResponse.h"
#include <iostream>
#include <stdexcept>

using Json = nlohmann::json;
namespace {
void Expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void Run() {
    constexpr size_t limit = 128 * 1024;
    const std::string instance(32, 'a');
    for (const auto &id : {std::string("r"), std::string(128, 'r'), std::string(128, '"')}) {
        for (int delta : {-1, 0, 1}) {
            Json result = {{"events", {{{"source", "串口设备"}, {"data", "AAAA"}}}}, {"cursor", 1}, {"gap", false}};
            auto empty = serialctl::MakeApiResponse(instance, id, result, "events", limit).dump().size();
            result["events"][0]["source"] = "串口设备" + std::string(limit + delta - empty, 'x');
            const Json unbounded = {{"type", "response"}, {"version", 1}, {"instance", instance},
                                    {"requestId", id}, {"ok", true}, {"result", result}};
            Expect(unbounded.dump().size() == limit + delta, "exact UTF-8 wire size arranged");
            auto reply = serialctl::MakeApiResponse(instance, id, result, "events", limit);
            Expect(reply.dump().size() <= limit && reply.at("requestId") == id, "complete bounded envelope and ID");
            if (delta <= 0)
                Expect(reply.at("ok") == true && reply.at("result") == result, "at/below limit preserves all history");
            else
                Expect(reply.at("ok") == false && reply.at("error").at("code") == "HISTORY_TOO_LARGE" &&
                       !reply.contains("result"), "oversize is a small error, never truncated JSON/history");
            std::cout << "id_bytes=" << id.size() << " escaped=" << (id.front() == '"')
                      << " candidate_wire_bytes=" << unbounded.dump().size()
                      << " emitted_wire_bytes=" << reply.dump().size() << " ok=" << reply.at("ok") << '\n';
        }
    }
    const auto tooBig = serialctl::MakeApiResponse(instance, "r", {{"text", std::string(limit, 'x')}}, "resources", limit);
    Expect(tooBig.at("error").at("code") == "RESPONSE_TOO_LARGE", "other response types use generic bounded error");
    auto hugeError = serialctl::MakeApiResponse(instance, "r", {{"error", {{"code", "IO"},
                        {"message", std::string(limit, 'x')}}}}, "input", limit);
    Expect(hugeError.dump().size() <= limit && hugeError.at("error").at("code") == "RESPONSE_TOO_LARGE",
           "backend error detail cannot overflow complete envelope");
    auto error = serialctl::MakeApiResponse(instance, "r", {{"error", {{"code", "HISTORY_GAP"}}}}, "events", limit);
    Expect(error.at("error").at("message") == "HISTORY_GAP", "ordinary gap semantics retained");
    std::cout << "PASS: exact limit-1/limit/limit+1; ID 1/128/escaped; UTF-8; ordinary/oversize error envelopes.\n";
}
} // namespace
int main() {
    try { Run(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
