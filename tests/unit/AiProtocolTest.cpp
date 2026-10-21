#include "AiProtocol.h"
#include "CodexProtocol.h"
#include <iostream>
#include <stdexcept>

using namespace serialctl::ai;
void Require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
int main() {
    try {
        JsonLines stream;
        const std::string bytes = u8"{\"method\":\"item/agentMessage/"
                                  u8"delta\",\"params\":{\"delta\":\"串口日志\"}}\r\n{\"id\":7,\"result\":{}}\n";
        std::vector<Json> frames;
        for (char byte : bytes)
            for (auto &frame : stream.Feed(&byte, 1))
                frames.push_back(frame);
        Require(frames.size() == 2 && frames[0]["params"]["delta"] == u8"串口日志",
                "UTF-8 fragmented across SSH reads must survive");
        Require(frames[1]["id"] == 7, "coalesced JSON-RPC response lost");
        bool rejected = false;
        try {
            const std::string oversized(JsonLines::MaximumFrame + 1, 'x');
            stream.Feed(oversized.data(), oversized.size());
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        Require(rejected, "unbounded incomplete frame accepted");

        ChatScroll scroll;
        scroll.Layout(1000, 300, true);
        Require(scroll.offset == 700, "new chat should follow output");
        scroll.Move(-450);
        scroll.Layout(1400, 300, true);
        Require(scroll.offset == 250 && scroll.unread, "streaming reply stole history reading position");
        scroll.Layout(1500, 400, false);
        Require(scroll.offset == 250, "resize stole history reading position");
        scroll.Bottom();
        Require(scroll.offset == 1100 && !scroll.unread, "return-to-latest failed");
        scroll.Layout(1600, 400, true);
        Require(scroll.offset == 1200, "live reader did not follow new output");
        scroll.Move(-99999);
        Require(scroll.offset == 0, "negative scroll offset");
        scroll.Layout(10, 400, false);
        Require(scroll.offset == 0 && !scroll.unread, "short chat scroll state invalid");

        Require(ValidCommand("dmesg | tail -n 80"), "single-line diagnostic command rejected");
        Require(!ValidCommand("reboot\nrm -rf /"), "multiline serial write accepted");
        Require(!ValidCommand(std::string("echo x\0reboot", 13)), "embedded NUL serial write accepted");
        Require(!ValidCommand(std::string(4097, 'x')), "oversized serial write accepted");
        const auto tools = SerialTools();
        Require(tools.size() == 4, "unexpected tool scope");
        for (const auto &tool : tools)
            Require(tool["inputSchema"]["additionalProperties"] == false, "unbounded tool argument schema");
        const auto reply = ToolResult({{"accepted", 5}, {"execution", "unknown"}});
        Require(reply["contentItems"][0]["type"] == "inputText", "app-server dynamic tool result format invalid");
        const auto create = MakeRequest(Operation::NewConversation, 77, {{"modelId", "fixture"}});
        Require(create["method"] == "thread/start" && create["params"]["dynamicTools"].size() == 4 &&
                    create["params"]["model"] == "fixture",
                "Presentation adapter lost fixed role/model/tool configuration");
        const auto delta = DecodeNotification(
            {{"method", "item/agentMessage/delta"},
             {"params", {{"threadId", "t"}, {"turnId", "u"}, {"itemId", "m"}, {"delta", u8"串口"}}}});
        Require(delta["event"] == "textDelta" && delta["conversationId"] == "t" && delta["text"] == u8"串口",
                "Protocol delta was not translated into a presentation event");
        const auto models = DecodeResponse(
            Operation::Models, {{"data", Json::array({{{"id", "m"}, {"supportedReasoningEfforts", Json::array()}}})}});
        Require(models["models"][0]["reasoning"] == false, "Adapter assumed reasoning support without server metadata");
        std::cout
            << "AiProtocol: framing, UTF-8, bounds, scroll anchoring, tool output and command validation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
