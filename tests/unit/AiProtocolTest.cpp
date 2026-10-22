#include "AiProtocol.h"
#include "AiHistory.h"
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
        Require(tools.size() == 7, "terminal and bounded power tools missing");
        Require(tools[5]["inputSchema"]["properties"].contains("enabled") &&
                    !tools[5]["inputSchema"]["properties"].contains("channels"), "Model must not override GUI channel selection");
        for (const auto &tool : tools)
            Require(tool["inputSchema"]["additionalProperties"] == false, "unbounded tool argument schema");
        const auto reply = ToolResult({{"accepted", 5}, {"execution", "unknown"}});
        Require(reply["contentItems"][0]["type"] == "inputText", "app-server dynamic tool result format invalid");
        const auto create = MakeRequest(Operation::NewConversation, 77, {{"defaultModel", "fixture"}, {"cwd", "/srv/project with spaces"}, {"modelId", "obsolete-model"}});
        Require(create["method"] == "thread/start" && create["params"]["dynamicTools"].size() == 7 &&
                    create["params"]["model"] == "fixture",
                "Presentation adapter lost fixed role/model/tool configuration");
        Require(create["params"]["config"]["web_search"] == "disabled" && create["params"]["approvalPolicy"] == "never",
                "Thread configuration must use the server enum and direct serial operation policy");
        Require(create["params"]["sandbox"] == "danger-full-access" &&
                    create["params"]["config"]["features.shell_tool"] == true &&
                    create["params"]["cwd"] == "/srv/project with spaces",
                "Server native tools or JSON project directory missing");
        Require(!MakeRequest(Operation::NewConversation, 79, {{"modelId", "obsolete"}})["params"].contains("model"),
                "Old user model preference must not override server default");
        const auto task = MakeRequest(Operation::StartTask, 80,
            {{"conversationId", "t"}, {"text", "inspect"}, {"defaultModel", "server-default"}, {"cwd", "/srv/code"}});
        Require(task["params"]["model"] == "server-default" && task["params"]["cwd"] == "/srv/code" &&
            task["params"]["sandboxPolicy"]["type"] == "dangerFullAccess", "Turn lost server tool policy");
        Json commandItem = {{"id", "cmd-1"}, {"type", "commandExecution"}, {"command", "make test"},
            {"cwd", "/srv/code"}, {"status", "completed"}, {"exitCode", 2}, {"aggregatedOutput", "ACTUAL_FAILED_TEST"}};
        const auto commandEvent = DecodeNotification({{"method", "item/completed"},
            {"params", {{"threadId", "t"}, {"turnId", "u"}, {"item", commandItem}}}});
        Require(commandEvent["event"] == "serverTool" && commandEvent["completed"] == true &&
            commandEvent["messageId"] == "server:cmd-1" &&
            commandEvent["text"].get<std::string>().find("ACTUAL_FAILED_TEST") != std::string::npos &&
            commandEvent["text"].get<std::string>().find(u8"退出码 2") != std::string::npos,
            "Native command result lost actual output or exit status");
        const auto serverDelta = DecodeNotification({{"method", "item/commandExecution/outputDelta"},
            {"params", {{"threadId", "t"}, {"turnId", "u"}, {"itemId", "cmd-1"}, {"delta", "stream"}}}});
        Require(serverDelta["event"] == "serverToolDelta" && serverDelta["messageId"] == "server:cmd-1" &&
            serverDelta["text"] == "stream", "Native command output delta not adapted");
        const Json fileItem = {{"id", "patch-1"}, {"type", "fileChange"}, {"status", "completed"},
            {"changes", {{{"path", "/srv/code/main.c"}, {"diff", "-old\n+fixed"}, {"kind", {{"type", "update"}}}}}}};
        const auto restored = DecodeResponse(Operation::RestoreConversation,
            {{"thread", {{"id", "t"}, {"turns", {{{"items", {commandItem, fileItem}}}}}}}});
        Require(restored["messages"].size() == 2 &&
            restored["messages"][1]["text"].get<std::string>().find("/srv/code/main.c") != std::string::npos &&
            restored["messages"][1]["text"].get<std::string>().find("+fixed") != std::string::npos,
            "Restored native tool evidence missing");
        Require(DecodeNotification({{"id", 90}, {"method", "item/fileChange/requestApproval"},
            {"params", {{"threadId", "t"}, {"turnId", "u"}}}})["event"] == "serverApproval",
            "Native approval request not associated with active turn");
        const auto resumed = MakeRequest(Operation::RestoreConversation, 78, {{"conversationId", "t"}});
        Require(resumed["params"]["config"]["web_search"] == "disabled" &&
                    resumed["params"]["approvalPolicy"] == "never",
                "Resumed threads must retain the current serial policy");
        Require(DecodeResponse(Operation::NewConversation, {{"thread", {{"id", "nested-id"}}}})["conversationId"] ==
                    "nested-id",
                "Thread ID must come from result.thread.id");
        Require(DecodeNotification({{"method", "remoteControl/status/changed"}, {"params", {{"status", "disabled"}}}})
                    .is_null(),
                "Unrelated notification must not be treated as a response");
        const auto complete = DecodeNotification(
            {{"method", "item/completed"},
             {"params",
              {{"threadId", "t"},
               {"turnId", "u"},
               {"item", {{"type", "agentMessage"}, {"id", "m"}, {"text", u8"完整回复"}, {"phase", nullptr}}}}}});
        Require(complete["event"] == "textComplete" && complete["final"] == true && complete["taskId"] == "u",
                "Gateway final agentMessage without turn/completed must be recognized");
        Require(DecodeNotification(
                    {{"method", "item/completed"},
                     {"params", {{"item", {{"type", "agentMessage"}, {"phase", "commentary"}}}}}})["final"] == false,
                "Intermediate commentary must not finish a task");
        Require(ExplainServiceError("Missing environment variable: `CONTAINER_APIKEY`.").find("~/.profile") !=
                    std::string::npos,
                "Missing server environment must have an actionable diagnosis");
        const auto delta = DecodeNotification(
            {{"method", "item/agentMessage/delta"},
             {"params", {{"threadId", "t"}, {"turnId", "u"}, {"itemId", "m"}, {"delta", u8"串口"}}}});
        Require(delta["event"] == "textDelta" && delta["conversationId"] == "t" && delta["text"] == u8"串口",
                "Protocol delta was not translated into a presentation event");
        const auto models = DecodeResponse(
            Operation::Models, {{"data", Json::array({{{"id", "m"}, {"supportedReasoningEfforts", Json::array()}}})}});
        Require(models["models"][0]["reasoning"] == false, "Adapter assumed reasoning support without server metadata");
        // Exercise persisted history across successive conversations. The old
        // initializer-list insert wrote pair arrays, then value() threw on them.
        auto history = RememberHistory(Json::array(), "first", "hello", "server-a/user");
        history = Json::parse(history.dump());
        Require(history.size() == 1 && history[0].is_object(), "History record must persist as one object");
        history = RememberHistory(history, "second", "again", "server-a/user");
        Require(history.size() == 2 && history[0]["id"] == "second",
                "Second conversation could not read persisted history");
        history = RememberHistory(history, "first", "updated", "server-b/user");
        Require(history.size() == 3, "Same ID on another SSH profile must retain its history");
        history = RememberHistory(history, "first", "renamed", "server-a/user");
        Require(history.size() == 3 && history[0]["title"] == "renamed", "History replacement lost its profile scope");
        const Json legacy = Json::array({Json::array({"id", "old"}), Json::array({"title", "old title"}),
                                         Json::array({"profile", "server-a/user"})});
        auto migrated = NormalizeHistory(legacy);
        Require(migrated.size() == 1 && migrated[0]["id"] == "old", "V1.0.9 pair-array history was not recovered");
        migrated = RememberHistory(migrated, "new", "new title", "server-a/user");
        Require(migrated.size() == 2 && migrated[1].is_object(), "Migrated history failed on next conversation");
        Json damaged = legacy;
        damaged.push_back(nullptr);
        damaged.push_back(7);
        damaged.push_back({{"id", 1}, {"title", "bad"}, {"profile", "server-a/user"}});
        damaged.push_back(Json::array({"id", "incomplete"}));
        Require(NormalizeHistory(damaged) == NormalizeHistory(legacy),
                "Damaged history rows must not block valid history");
        Require(NormalizeHistory(Json()).empty(), "Invalid history root must be ignored");
        for (int i = 0; i < 110; ++i)
            migrated = RememberHistory(migrated, std::to_string(i), "title", "server-a/user");
        Require(migrated.size() == 100 && migrated[0]["id"] == "109",
                "History must retain only the latest 100 records");
        std::cout
            << "AiProtocol: framing, UTF-8, bounds, scroll anchoring, tool output and command validation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
