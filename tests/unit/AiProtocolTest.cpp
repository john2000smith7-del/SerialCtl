#include "AiProtocol.h"
#include "AiHistory.h"
#include "CodexProtocol.h"
#include "AiReplyMarkup.h"
#include "Workspace.h"
#include "WorkspaceActivity.h"
#include "WorkspaceInstructions.h"
#include <algorithm>
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
        Require(tools.size() == 15, "terminal, local boot and bounded power recovery tools missing");
        const auto allTools = AllDynamicTools();
        Require(allTools.size() == 15 + serialctl::mcp::McpProtocol::ToolDefinitions().size(),
                "AI dynamic tools must expose all MCP capabilities");
        Require(ResolveMcpMethodFromDynamicTool("mcp_session_connect_serial") == "session.connect_serial",
                "MCP dynamic tool name mapping broken");
        const auto outputTool = std::find_if(tools.begin(), tools.end(), [](const Json &t) { return t.at("name") == "serialctl_power_output"; });
        Require(outputTool != tools.end() && (*outputTool)["inputSchema"]["properties"].contains("enabled") &&
                    !(*outputTool)["inputSchema"]["properties"].contains("channels"), "Model must not override GUI channel selection");
        for (const auto &tool : allTools)
            Require(tool["inputSchema"]["additionalProperties"] == false, "unbounded tool argument schema");
        const auto reply = ToolResult({{"accepted", 5}, {"execution", "unknown"}});
        Require(reply["contentItems"][0]["type"] == "inputText", "app-server dynamic tool result format invalid");
        const auto create = MakeRequest(Operation::NewConversation, 77, {{"defaultModel", "fixture"}, {"cwd", "/srv/project with spaces"}, {"modelId", "obsolete-model"}});
        Require(create["method"] == "thread/start" && create["params"]["dynamicTools"].size() == allTools.size() &&
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
        const auto imageTurn = MakeRequest(Operation::StartTask, 81,
            {{"conversationId", "t"}, {"text", "inspect image"}, {"images", {"/tmp/serialctl-ai.A123456789/file-1.png"}}});
        Require(imageTurn["params"]["input"].size() == 2 &&
                imageTurn["params"]["input"][1] == Json{{"type", "localImage"}, {"path", "/tmp/serialctl-ai.A123456789/file-1.png"}},
                "Turn did not carry the uploaded image as a visual input");
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
        const auto turnDiff = DecodeNotification({{"method", "turn/diff/updated"},
            {"params", {{"threadId", "remote"}, {"turnId", "turn-1"}, {"diff",
                "diff --git a/board.txt b/board.txt\n--- a/board.txt\n+++ b/board.txt\n@@ -1 +1 @@\n-old\n+new\n"
                "diff --git a/info b/info\n--- /dev/null\n+++ \"b/\\344\\270\\255.txt\"\n@@ -0,0 +1 @@\n+info\n"
                "diff --git a/removed b/removed\n--- a/removed\n+++ /dev/null\n@@ -1 +0,0 @@\n-previous\n"}}}});
        const auto &patches = turnDiff.at("fileChanges");
        Require(turnDiff.at("event") == "turnDiff" && turnDiff.at("taskId") == "turn-1" && patches.size() == 3 &&
                patches[0].at("path") == "board.txt" && patches[0].at("removedLines") == 1 &&
                patches[1].at("path") == u8"中.txt" && patches[1].at("action") == "create" &&
                patches[2].at("action") == "delete", "remote aggregate diff keeps file paths, actions and line counts");
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
        Require(commandEvent["failed"] == true, "Nonzero command exit status was not retained for process display");
        const auto compact = MakeRequest(Operation::CompactConversation, 82, {{"conversationId", "t"}});
        Require(compact["method"] == "thread/compact/start" && compact["params"] == Json({{"threadId", "t"}}),
                "Context cleanup must use the native server thread API");
        Require(
            DecodeNotification({{"method", "item/completed"},
                                {"params", {{"threadId", "t"}, {"item", {{"type", "contextCompaction"}}}}}})["event"] ==
                "contextCompacted",
            "Current context compaction completion was ignored");
        const auto grouped = DecodeResponse(
            Operation::RestoreConversation,
            {{"thread",
              {{"id", "t"},
               {"turns",
                {{{"id", "turn-a"},
                  {"status", "completed"},
                  {"items",
                   {{{"type", "agentMessage"}, {"id", "interim"}, {"text", "Progress"}, {"phase", "commentary"}},
                    {{"type", "agentMessage"}, {"id", "final"}, {"text", "Answer"}, {"phase", "final_answer"}}}}}}}}}});
        Require(grouped["messages"][0]["group"] == "turn-a" && grouped["messages"][0]["process"] == true &&
                    grouped["messages"][1]["process"] == false,
                "History lost process/final-answer boundaries");
        const auto recovered = DecodeResponse(Operation::RestoreConversation,
            {{"thread", {{"id", "t"}, {"turns", {{{"id", "recovered-turn"}, {"status", "completed"},
                {"items", {commandItem, {{"type", "agentMessage"}, {"id", "answer"}, {"text", "Diagnosis"}}}}}}}}}});
        Require(recovered["messages"][0]["status"] == "completed" &&
                    recovered["messages"][0]["failedStep"] == true &&
                    recovered["messages"][1]["failedStep"] == true,
                "A failed diagnostic command must not overwrite the server's completed turn status");
        const auto resumed = MakeRequest(Operation::RestoreConversation, 78, {{"conversationId", "t"}});
        Require(resumed["params"]["config"]["web_search"] == "disabled" &&
                    resumed["params"]["approvalPolicy"] == "never",
                "Resumed threads must retain the current serial policy");
        const auto oldWorkspace = MakeRequest(Operation::RestoreConversation,79,{{"conversationId","t"},{"cwd","/other/project"}});
        Require(!oldWorkspace["params"].contains("cwd"),"Restoring history must not replace its original server workspace");
        const auto originalPath = DecodeResponse(Operation::RestoreConversation,{{"thread",{{"id","t"},{"cwd","/srv/original"}}}});
        Require(originalPath["cwd"]=="/srv/original","Thread's actual cwd must survive the adapter");
        const auto strippedHistory = DecodeResponse(Operation::RestoreConversation,
            {{"thread", {{"id", "t"}, {"turns", {{{"id", "u1"}, {"items", {
                {{"type", "userMessage"}, {"id", "m1"}, {"content", {{{"type", "text"},
                    {"text", u8"你好，给我一个表格测试案例以及C语言排序算法案例\n\n[SerialCtl associated terminal session: ; label: 请选择终端…; selected session read/write enabled; no per-command confirmation. Power tools independently address the connected IT6332A GUI-selected channels; call serialctl_power_get first and only switch output when the user asks.]"}}}}}
            }}}}}}}});
        Require(strippedHistory["messages"].size() == 1 &&
                strippedHistory["messages"][0]["text"] == u8"你好，给我一个表格测试案例以及C语言排序算法案例",
                "Restored userMessage must strip injected SerialCtl session context");
        Require(commandEvent["text"].get<std::string>().find("```text\nACTUAL_FAILED_TEST\n```") != std::string::npos,
                "Command output should be fenced in a text code block");
        const auto listPath=MakeRequest(Operation::ReadDirectory,80,{{"path","/srv/code with spaces"}});
        Require(listPath["method"]=="fs/readDirectory" && listPath["params"]["path"]=="/srv/code with spaces","Directory API must use the exact absolute path");
        const auto nativeFind=MakeRequest(Operation::DirectoryFallback,81,{{"path","/srv/$(literal) code"}});
        Require(nativeFind["params"]["command"][2]=="/srv/$(literal) code" && nativeFind["params"]["timeoutMs"]==3000 &&
            nativeFind["params"]["outputBytesCap"]==65536,"Fallback must pass native argv with timeout/bounds, never interpret shell metacharacters");
        const auto directories=WorkspaceDirectories("/srv",Json::array({{{"fileName","code"},{"isDirectory",true}},
            {{"fileName","../escape"},{"isDirectory",true}},{{"fileName","file.c"},{"isDirectory",false}},
            {{"fileName","line\nbreak"},{"isDirectory",true}},{{"fileName","code"},{"isDirectory",true}}}));
        Require(directories==Json::array({"/srv/code"}),"Directory dropdown must filter unsafe names, files and duplicate entries");
        Require(WorkspaceParent("/srv/code/")=="/srv" && WorkspaceParent("/")=="/" && !ValidWorkspacePath("relative") &&
            !ValidWorkspacePath(std::string("/a\0b",4)),"Workspace paths must remain absolute and bounded");
        Require(WorkspaceProfileMatches("host:22/linux/project:/srv/old","host:22/linux") &&
            StoredWorkspace({{"profile","host:22/linux/project:/srv/old"}})=="/srv/old" &&
            !WorkspaceProfileMatches("other:22/linux","host:22/linux"),"Legacy workspace history must migrate without crossing servers");
        Require(WorkspaceTargetIdentity({{"kind","cmd"},{"instance","old"},{"id","session-1"}})!=
            WorkspaceTargetIdentity({{"kind","cmd"},{"instance","new"},{"id","session-1"}}),"Reused CMD IDs from another process must not restore the wrong terminal");
        const auto finalStart=DecodeNotification({{"method","item/started"},{"params",{{"item",{{"type","agentMessage"},{"id","final"},{"phase","final_answer"}}}}}});
        Require(finalStart["event"]=="textStarted" && finalStart["final"]==true,"Explicit final-answer streaming must remain separate from collapsed process");
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

        // Issue #56: Session permission persistence & full-access protocol validation
        const auto newConvReq = MakeRequest(Operation::NewConversation, 101, {{"cwd", "/srv/test"}});
        Require(newConvReq["params"]["sandbox"] == "danger-full-access" &&
                newConvReq["params"]["approvalPolicy"] == "never",
                "NewConversation must default to danger-full-access and approvalPolicy never");
        const auto resumeReq = MakeRequest(Operation::RestoreConversation, 102, {{"conversationId", "t-1"}});
        Require(resumeReq["params"]["sandbox"] == "danger-full-access" &&
                resumeReq["params"]["approvalPolicy"] == "never",
                "RestoreConversation must default to danger-full-access and approvalPolicy never");
        const auto startTaskReq = MakeRequest(Operation::StartTask, 103, {{"conversationId", "t-1"}, {"text", "hello"}});
        Require(startTaskReq["params"]["sandboxPolicy"]["type"] == "dangerFullAccess" &&
                startTaskReq["params"]["approvalPolicy"] == "never",
                "StartTask must specify dangerFullAccess and approvalPolicy never");
        const auto runCmdReq = MakeRequest(Operation::RunCommand, 104, {{"command", Json::array({"ls", "-la"})}, {"cwd", "/srv"}});
        Require(runCmdReq["method"] == "command/exec" &&
                runCmdReq["params"]["command"][0] == "ls" &&
                runCmdReq["params"]["sandbox"] == "danger-full-access" &&
                runCmdReq["params"]["approvalPolicy"] == "never",
                "RunCommand must enforce command/exec with danger-full-access");

        // Decode effective permissions from responses
        const auto decodedPerm = DecodeResponse(Operation::NewConversation, {
            {"thread", {{"id", "thread-perm"}, {"effectiveSandbox", "danger-full-access"}, {"effectiveApprovalPolicy", "never"}}}});
        Require(decodedPerm["effectiveSandbox"] == "danger-full-access" &&
                decodedPerm["effectiveApprovalPolicy"] == "never",
                "Effective permissions must be decoded from thread response");
        const auto appServerPerm = DecodeResponse(Operation::NewConversation, {
            {"thread", {{"id", "thread-native"}}}, {"sandbox", {{"type", "dangerFullAccess"}}},
            {"approvalPolicy", "never"}, {"activePermissionProfile", nullptr}});
        Require(appServerPerm["effectiveSandbox"] == "dangerFullAccess" && appServerPerm["effectiveApprovalPolicy"] == "never",
                "App Server thread/start returns a sandbox policy object");
        const auto restrictedPerm = DecodeResponse(Operation::RestoreConversation, {
            {"thread", {{"id", "thread-native"}}}, {"sandbox", {{"type", "readOnly"}, {"networkAccess", false}}},
            {"approvalPolicy", "never"}});
        Require(restrictedPerm["effectiveSandbox"] == "readOnly",
                "App Server thread/resume can return an effective read-only sandbox object");
        Require(ExtractSandbox({{"sandbox", "danger-full-access"}, {"effectiveSandbox", "workspace-write"}}) == "workspace-write",
                "An explicit effective restriction takes precedence over the legacy value");

        // Issue #57: WorkspaceInstructions pure functions validation
        const std::string sampleAgentsMd =
            "# My Project\n\n"
            "This is user content before.\n\n"
            "<!-- SerialCtl workspace practices BEGIN -->\n"
            "- Always run cmake with -B build\n"
            "- Avoid deleting cache\n"
            "<!-- SerialCtl workspace practices END -->\n\n"
            "## Footer\n"
            "User content after.\n";

        auto extracted = ExtractManagedBlock(sampleAgentsMd);
        Require(extracted.hasManagedBlock &&
                extracted.managedContent.find("Always run cmake") != std::string::npos &&
                extracted.managedContent.find("Avoid deleting cache") != std::string::npos,
                "ExtractManagedBlock failed to extract existing block");

        auto entries = ParsePracticeEntries(extracted.managedContent);
        Require(entries.size() == 2 && entries[0] == "Always run cmake with -B build",
                "ParsePracticeEntries failed to parse lines");

        // Sanitization tests
        Require(SanitizePracticeEntry("password=secret123").empty(), "Sanitize must reject password=");
        Require(SanitizePracticeEntry("my api_key is sk-1234567890").empty(), "Sanitize must reject api_key");
        Require(SanitizePracticeEntry("-----BEGIN RSA PRIVATE KEY-----").empty(), "Sanitize must reject private keys");
        Require(SanitizePracticeEntry("Authorization: Bearer token123").empty(), "Sanitize must reject Bearer token");
        Require(SanitizePracticeEntry("Always check git status before commit") == "Always check git status before commit",
                "Valid entry should pass sanitization");
        Require(SanitizePracticeEntry("- Use `ninja` build tool") == "Use ninja build tool",
                "Sanitize should strip leading dash and backticks");
        Require(SanitizePracticeEntry(std::string(400, 'a')).size() == 300,
                "Sanitize should truncate to 300 characters");

        // Merge tests
        std::string merged = MergeManagedBlock(sampleAgentsMd, {"Always check git status before commit"});
        Require(merged.find("Always check git status before commit") != std::string::npos &&
                merged.find("Always run cmake with -B build") != std::string::npos &&
                merged.find("This is user content before.") != std::string::npos &&
                merged.find("User content after.") != std::string::npos,
                "MergeManagedBlock must preserve outside content and prepend new entry");
        Require(merged.size() <= 8192, "MergeManagedBlock must respect 8 KiB capacity");

        // Context prefix test
        std::string prefix = BuildTurnContextPrefix(sampleAgentsMd);
        Require(prefix.find("【工作空间经验 (AGENTS.md)】") != std::string::npos &&
                prefix.find("Always run cmake") != std::string::npos,
                "BuildTurnContextPrefix must construct prompt prefix");

        // Base64 test
        Require(Base64Encode("hello world") == "aGVsbG8gd29ybGQ=", "Base64Encode mismatch");
        Require(Base64Encode("") == "", "Base64Encode empty mismatch");

        // Antigravity ACP protocol translation tests
        const auto acpNew = MakeAcpRequest(Operation::NewConversation, 201, {{"cwd", "/srv/app"}}, 7080);
        Require(acpNew["method"] == "session/new" &&
                acpNew["params"]["cwd"] == "/srv/app" &&
                acpNew["params"]["mcpServers"][0]["url"] == "http://127.0.0.1:7080/mcp",
                "ACP session/new must carry cwd and serialctl MCP HTTP URL");
        const auto acpNewResp = DecodeAcpResponse(
            Operation::NewConversation,
            {{"sessionId", "acp-s1"}, {"models", {{"currentModelId", "gemini-4-argon-medium"}}}},
            "", "/srv/app");
        Require(acpNewResp["conversationId"] == "acp-s1" &&
                acpNewResp["defaultModel"] == "gemini-4-argon-medium" &&
                acpNewResp["cwd"] == "/srv/app",
                "DecodeAcpResponse must map sessionId and currentModelId");
        const auto acpPrompt = MakeAcpRequest(Operation::StartTask, 202, {{"conversationId", "acp-s1"}, {"text", u8"检查串口"}});
        Require(acpPrompt["method"] == "session/prompt" &&
                acpPrompt["params"]["sessionId"] == "acp-s1" &&
                acpPrompt["params"]["prompt"][0]["text"] == u8"检查串口",
                "ACP session/prompt must map conversationId and text");
        const auto acpCancel = MakeAcpRequest(Operation::StopTask, 203, {{"conversationId", "acp-s1"}});
        Require(acpCancel["method"] == "session/cancel" && !acpCancel.contains("id"),
                "ACP session/cancel must be a JSON-RPC notification without id");
        std::string acpAccum;
        bool acpStarted = false;
        const auto chunkEvents = DecodeAcpNotification(
            Json{{"method", "session/update"},
                 {"params",
                  {{"sessionId", "acp-s1"},
                   {"update",
                    {{"sessionUpdate", "agent_message_chunk"},
                     {"content", {{"type", "text"}, {"text", u8"你好"}}}}}}}},
            "acp-s1", "acp-turn-1", "acp-msg-1", acpAccum, acpStarted);
        Require(acpStarted && acpAccum == u8"你好" && chunkEvents.size() == 2 &&
                chunkEvents[0]["event"] == "textStarted" && chunkEvents[1]["event"] == "textDelta" &&
                chunkEvents[1]["text"] == u8"你好",
                "ACP agent_message_chunk must emit textStarted and textDelta");
        const auto thoughtEvents = DecodeAcpNotification(
            Json{{"method", "session/update"},
                 {"params",
                  {{"sessionId", "acp-s1"},
                   {"update",
                    {{"sessionUpdate", "agent_thought_chunk"},
                     {"content", {{"type", "text"}, {"text", "thinking"}}}}}}}},
            "acp-s1", "acp-turn-1", "acp-msg-1", acpAccum, acpStarted);
        Require(thoughtEvents.size() == 1 && thoughtEvents[0]["event"] == "thinkingDelta" &&
                thoughtEvents[0]["text"] == "thinking",
                "ACP agent_thought_chunk must emit thinkingDelta");
        const auto toolEvents = DecodeAcpNotification(
            Json{{"method", "session/update"},
                 {"params",
                  {{"sessionId", "acp-s1"},
                   {"update",
                    {{"sessionUpdate", "tool_call_update"},
                     {"toolCallId", "tc-1"},
                     {"title", "Run ls"},
                     {"status", "completed"},
                     {"rawInput", {{"command", "ls -la"}}},
                     {"content", Json::array({{{"type", "content"}, {"content", {{"type", "text"}, {"text", "ok"}}}}})}}}}}},
            "acp-s1", "acp-turn-1", "acp-msg-1", acpAccum, acpStarted);
        Require(toolEvents.size() == 1 && toolEvents[0]["event"] == "serverTool" &&
                toolEvents[0]["completed"] == true &&
                toolEvents[0]["text"].get<std::string>().find("ls -la") != std::string::npos,
                "ACP tool_call_update must emit serverTool with command and output");
        const auto codexThought = DecodeNotification(
            {{"method", "item/reasoning/textDelta"},
             {"params", {{"threadId", "t"}, {"turnId", "u"}, {"itemId", "r1"}, {"delta", "step 1"}}}});
        Require(codexThought["event"] == "thinkingDelta" && codexThought["text"] == "step 1",
                "Codex item/reasoning/textDelta must emit thinkingDelta");
        const auto quoteBlocks = markup::Parse(L"> 第一行 **加粗**\n> 第二行 `code`\n普通行");
        Require(quoteBlocks.size() == 2 && quoteBlocks[0].kind == markup::Kind::Quote &&
                quoteBlocks[0].lines.size() == 2 && quoteBlocks[0].lines[0].value == L"第一行 加粗",
                "Markdown blockquote '>' must parse as Kind::Quote");

        // Phase 2 Markdown/GFM/HTML parser regression coverage
        const auto brSteps = markup::Parse(
            L"1. 执行系统重启并监听启动流程：<br>reboot<br>"
            L"2. 发送前导空格与  stop 打断序列。<br>"
            L"3. 配置参数：<br>setenv ipaddr 192.168.1.50 && setenv serverip 192.168.1.100<br>"
            L"tftpboot 0x82000000 u-boot-custom.bin<br>"
            L"sf probe 0:0 && sf write 0x82000000 0x0 ${filesize}");
        Require(brSteps.size() == 1 && brSteps[0].kind == markup::Kind::List &&
                brSteps[0].lines.size() == 7 &&
                brSteps[0].lines[0].value == L"1. 执行系统重启并监听启动流程：" &&
                brSteps[0].lines[1].value == L"reboot" && brSteps[0].lineMeta[1].continuation &&
                brSteps[0].lines[2].value == L"2. 发送前导空格与  stop 打断序列。" && !brSteps[0].lineMeta[2].continuation &&
                brSteps[0].lines[6].value.find(L"${filesize}") != std::wstring::npos &&
                brSteps[0].lines[6].value.find(L"<br>") == std::wstring::npos,
                "Ordered list items split by <br> must retain numbering, hanging continuations and shell ${filesize}");

        const auto inlineRich = markup::Parse(
            L"*斜体* _也斜_ ***粗斜*** ~~删除~~ <u>下划</u> <mark>高亮</mark> H<sub>2</sub>O 2<sup>10</sup> "
            L"<kbd>Ctrl</kbd> \\#转义 &copy; &amp; CONFIG_ARM64_VA_BITS_48 dma_alloc_coherent "
            L"[文档](https://developers.google.com) <https://kernel.org> ![图标](https://example.com/a.png)");
        Require(inlineRich.size() == 1 && inlineRich[0].lines.size() == 1, "Inline sample must form one prose line");
        const auto &rich = inlineRich[0].lines[0];
        Require(rich.value.find(L"CONFIG_ARM64_VA_BITS_48") != std::wstring::npos &&
                rich.value.find(L"dma_alloc_coherent") != std::wstring::npos &&
                rich.value.find(L"#转义") != std::wstring::npos &&
                rich.value.find(L"\\#") == std::wstring::npos &&
                rich.value.find(L"© &") != std::wstring::npos &&
                rich.value.find(L"[图片: 图标]") != std::wstring::npos &&
                rich.value.find(L"<kbd>") == std::wstring::npos &&
                rich.links.size() == 3 && rich.links[0].url == L"https://developers.google.com" &&
                rich.links[1].url == L"https://kernel.org",
                "Inline formatting, escapes, entities, snake_case immunity and links must parse cleanly");
        auto hasFlag = [&](std::wstring_view needle, unsigned flag) {
            const size_t pos = rich.value.find(needle);
            if (pos == std::wstring::npos) return false;
            return std::any_of(rich.spans.begin(), rich.spans.end(), [&](const markup::Span &s) {
                return pos >= s.start && pos + needle.size() <= s.start + s.length && (s.flags & flag) == flag;
            });
        };
        Require(hasFlag(L"斜体", markup::ItalicFlag) &&
                hasFlag(L"也斜", markup::ItalicFlag) &&
                hasFlag(L"粗斜", markup::BoldFlag | markup::ItalicFlag) &&
                hasFlag(L"删除", markup::StrikeFlag) &&
                hasFlag(L"下划", markup::UnderlineFlag) &&
                hasFlag(L"高亮", markup::MarkFlag) &&
                hasFlag(L"2", markup::SubFlag) &&
                hasFlag(L"10", markup::SupFlag) &&
                hasFlag(L"Ctrl", markup::KbdFlag),
                "Expected rich inline style flags for italic, bold-italic, strike, underline, mark, sub, sup and kbd");
        Require(std::none_of(rich.spans.begin(), rich.spans.end(), [&](const markup::Span &s) {
            return rich.value.substr(s.start, s.length).find(L"ARM64") != std::wstring::npos;
        }), "Snake_case underscore identifiers must never be italicized");

        const auto multiDoc = markup::Parse(
            L"# H1\n###### H6\n"
            L"软换行第一句。\n软换行第二句。\n\n"
            L"硬换行第一句。<br>\n使用 `<br>` 与双空格  \n第三句。\n\n"
            L"* 一级\n  * 二级\n    * 三级\n- [x] 已完成\n- [ ] 待完成\n\n"
            L"> 单层\n> > 二层\n> - 引用条目\n\n"
            L"> [!WARNING]\n> 注意检查电压\n\n"
            L"<details><summary>展开日志</summary>\n\n- 细节条目\n\n</details>\n\n"
            L"<table style=\"width:100%\"><thead><tr><th>编号</th><th>说明</th></tr></thead>"
            L"<tbody><tr><td><b>BUG-1</b><br><span style=\"color: gray;\">[高优先级]</span></td>"
            L"<td><code>&lt;rid&gt;</code></td></tr></tbody></table>\n\n"
            L"$$\ny = x^2\n$$\n$5 和 $10 与 $PATH/$USER 及 $O(n^2)$");
        Require(multiDoc.size() >= 10 &&
                multiDoc[0].kind == markup::Kind::Heading && multiDoc[0].headingLevel == 1 &&
                multiDoc[1].kind == markup::Kind::Heading && multiDoc[1].headingLevel == 6 &&
                multiDoc[2].kind == markup::Kind::Prose && multiDoc[2].lines.size() == 1 &&
                multiDoc[2].lines[0].value == L"软换行第一句。软换行第二句。" &&
                multiDoc[3].kind == markup::Kind::Prose && multiDoc[3].lines.size() == 3 &&
                multiDoc[3].lines[1].value.find(L"<br>") != std::wstring::npos &&
                multiDoc[4].kind == markup::Kind::List && multiDoc[4].lineMeta.size() == 5 &&
                multiDoc[4].lineMeta[0].indent == 0 && multiDoc[4].lineMeta[1].indent == 1 &&
                multiDoc[4].lineMeta[2].indent == 2 &&
                multiDoc[4].lineMeta[3].isTask && multiDoc[4].lineMeta[3].checked &&
                multiDoc[4].lineMeta[4].isTask && !multiDoc[4].lineMeta[4].checked &&
                multiDoc[5].kind == markup::Kind::Quote && multiDoc[5].lineMeta[1].quoteDepth == 2 &&
                multiDoc[5].lineMeta[2].isList &&
                multiDoc[6].kind == markup::Kind::Quote && multiDoc[6].alert == markup::AlertKind::Warning &&
                multiDoc[7].kind == markup::Kind::Details && multiDoc[7].summary.value == L"展开日志" &&
                !multiDoc[7].detailsOpen && multiDoc[7].children.size() == 1 &&
                multiDoc[8].kind == markup::Kind::Table && multiDoc[8].rows.size() == 2 &&
                multiDoc[8].rows[1][0].value == L"BUG-1\n[高优先级]" &&
                multiDoc[8].rows[1][1].value == L"<rid>" &&
                multiDoc[9].kind == markup::Kind::Math &&
                multiDoc[10].kind == markup::Kind::Prose &&
                multiDoc[10].lines[0].value.find(L"$5 和 $10") != std::wstring::npos &&
                multiDoc[10].lines[0].value.find(L"$PATH/$USER") != std::wstring::npos,
                "Multi-level headings, lists, alerts, details, HTML tables, math and shell safety must parse cleanly");

        const auto diffCode = markup::Parse(L"```diff\n@@ -1 +1 @@\n+added\n-removed\n```");
        Require(diffCode.size() == 1 && diffCode[0].kind == markup::Kind::Code &&
                diffCode[0].lines[1].spans[0].ink == markup::Ink::String &&
                diffCode[0].lines[2].spans[0].ink == markup::Ink::Number,
                "Diff syntax highlighting must distinguish added and removed lines");

        const auto hasSpan = [](const markup::Text& line, const std::wstring& needle,
                                unsigned requiredFlags, unsigned forbiddenFlags = 0) {
            for (const auto& span : line.spans) {
                if (span.start + span.length > line.value.size()) continue;
                const auto text = std::wstring_view(line.value).substr(span.start, span.length);
                if (text.find(needle) != std::wstring_view::npos &&
                    (span.flags & requiredFlags) == requiredFlags &&
                    (span.flags & forbiddenFlags) == 0) {
                    return true;
                }
            }
            return false;
        };

        const auto texDoc = markup::Parse(
            L"### 四、对应 $P_3$ 边界\n\n"
            L"判别函数 $C(x) \\in \\{0, 1\\}$ 与 $C(x_{<k})$，对比 $G_{\\text{embryo}}(e_k)$、"
            L"$\\text{my\\_id} + \\frac{a+b}{2} + \\sqrt{x}$ 和 $\\mathbf{\\text{先有蛋}}$。\n\n"
            L"$$\n"
            L"\\text{广义蛋先于鸡} \\;\\land\\; \\text{受精} \\implies \\mathbf{\\text{先有蛋}}\n"
            L"$$\n\n"
            L"$$\n"
            L"\\begin{aligned}\n"
            L"a &= b \\\\\n"
            L"c \\ll d\n"
            L"\\end{aligned}\n"
            L"$$\n\n"
            L"价格 $5 与 $PATH/${HOME} 保持原样，复杂度 $O(n^2)$。");
        Require(texDoc.size() == 5, "Expected 5 blocks in TeX math test document");
        Require(texDoc[0].kind == markup::Kind::Heading && texDoc[0].headingLevel == 3,
                "Expected H3 heading with inline math");
        Require(hasSpan(texDoc[0].lines[0], L"P", markup::ItalicFlag | markup::MathFlag),
                "Heading math variable P must preserve ItalicFlag | MathFlag");
        Require(hasSpan(texDoc[0].lines[0], L"3", markup::SubFlag | markup::MathFlag),
                "Heading math subscript 3 must preserve SubFlag | MathFlag");
        Require(texDoc[1].kind == markup::Kind::Prose &&
                texDoc[1].lines[0].value.find(L'\\') == std::wstring::npos,
                "Inline math must not leave raw TeX backslashes");
        Require(texDoc[1].lines[0].value.find(L"C(x) ∈ {0, 1}") != std::wstring::npos,
                "Inline set membership and escaped braces must translate cleanly");
        Require(texDoc[1].lines[0].value.find(L"my_id + (a+b)/2 + √(x)") != std::wstring::npos,
                "Inline text underscore escape, fraction and sqrt must translate cleanly");
        Require(hasSpan(texDoc[1].lines[0], L"embryo", markup::SubFlag) &&
                hasSpan(texDoc[1].lines[0], L"k", markup::SubFlag) &&
                hasSpan(texDoc[1].lines[0], L"<k", markup::SubFlag),
                "Nested \\text subscripts and symbol subscripts must mark SubFlag");
        Require(hasSpan(texDoc[1].lines[0], L"先有蛋", markup::BoldFlag, markup::ItalicFlag),
                "Nested \\mathbf{\\text{...}} must be bold without italicizing plain text");
        Require(texDoc[2].kind == markup::Kind::Math && texDoc[2].lines.size() == 1 &&
                texDoc[2].lines[0].value.find(L"∧") != std::wstring::npos &&
                texDoc[2].lines[0].value.find(L"⇒") != std::wstring::npos &&
                texDoc[2].lines[0].value.find(L'\\') == std::wstring::npos &&
                hasSpan(texDoc[2].lines[0], L"先有蛋", markup::BoldFlag, markup::ItalicFlag),
                "Single-line display math block must translate logical symbols and bold text");
        Require(texDoc[3].kind == markup::Kind::Math && texDoc[3].lines.size() == 2 &&
                texDoc[3].lines[0].value.find(L'&') == std::wstring::npos &&
                texDoc[3].lines[0].value.find(L"begin") == std::wstring::npos &&
                texDoc[3].lines[1].value.find(L"≪") != std::wstring::npos,
                "Aligned display math must strip \\begin/\\end/& and split \\\\ lines");
        Require(texDoc[4].kind == markup::Kind::Prose &&
                texDoc[4].lines[0].value.find(L"$5 与 $PATH/${HOME}") != std::wstring::npos &&
                hasSpan(texDoc[4].lines[0], L"2", markup::SupFlag | markup::MathFlag),
                "Shell variables and currency must remain literal alongside inline superscript math");

        // Model & reasoning effort switching tests (Codex + Antigravity ACP)
        const auto codexModels = DecodeResponse(
            Operation::Models,
            {{"data", Json::array({{{"id", "gpt-5.6-sol"},
                                     {"displayName", "GPT-5.6-Sol"},
                                     {"defaultReasoningEffort", "low"},
                                     {"supportedReasoningEfforts",
                                      Json::array({{{"reasoningEffort", "low"}},
                                                   {{"reasoningEffort", "high"}}})}}})}});
        Require(codexModels["models"][0]["defaultEffort"] == "low" &&
                codexModels["models"][0]["efforts"].size() == 2 &&
                codexModels["models"][0]["efforts"][1] == "high",
                "Codex model/list must decode supportedReasoningEfforts");
        const auto effortTurn = MakeRequest(
            Operation::StartTask, 204,
            {{"conversationId", "t-1"}, {"text", "hi"}, {"defaultModel", "gpt-5.6-sol"}, {"effort", "high"}});
        Require(effortTurn["params"]["model"] == "gpt-5.6-sol" &&
                effortTurn["params"]["effort"] == "high",
                "Codex turn/start must pass selected model and effort");
        const auto acpSetCfg = MakeAcpRequest(
            Operation::SetConfig, 205,
            {{"conversationId", "acp-s1"}, {"configId", "thought_level"}, {"value", "high"}});
        Require(acpSetCfg["method"] == "session/set_config_option" &&
                acpSetCfg["params"]["configId"] == "thought_level" &&
                acpSetCfg["params"]["value"] == "high",
                "ACP SetConfig must build session/set_config_option");
        const auto acpCfgResp = DecodeAcpResponse(
            Operation::SetConfig,
            {{"configOptions",
              Json::array({{{"id", "model"},
                            {"currentValue", "gemini-3.8-flash"},
                            {"options", Json::array({{{"value", "gemini-3.8-flash"}, {"name", "Gemini 3.8 Flash"}}})}},
                           {{"id", "thought_level"},
                            {"currentValue", "high"},
                            {"options", Json::array({{{"value", "low"}, {"name", "Low"}},
                                                     {{"value", "high"}, {"name", "High"}}})}}})}});
        Require(acpCfgResp["defaultModel"] == "gemini-3.8-flash" &&
                acpCfgResp["defaultEffort"] == "high" &&
                acpCfgResp["models"].size() == 1 &&
                acpCfgResp["efforts"].size() == 2,
                "ACP SetConfig must decode updated model and thought_level options");

        // Decoupled 2x2 workspace tools, ACP remote MCP URL, and prompt context stripping
        Require(ResolveMcpMethodFromDynamicTool("mcp_workspace_info") == "workspace.info" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_list") == "workspace.list" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_search_text") == "workspace.search_text" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_read_file") == "workspace.read_file" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_write_file") == "workspace.write_file" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_edit_file") == "workspace.edit_file" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_move") == "workspace.move" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_delete") == "workspace.delete" &&
                    ResolveMcpMethodFromDynamicTool("mcp_workspace_exec") == "workspace.exec",
                "Workspace MCP dynamic tool mapping incomplete");
        const auto acpTunnel = MakeAcpRequest(Operation::NewConversation, 206, {{"cwd", "/tmp"}}, 17081);
        Require(acpTunnel["method"] == "session/new" &&
                    acpTunnel["params"]["cwd"] == "/tmp" &&
                    !acpTunnel["params"]["mcpServers"].empty() &&
                    acpTunnel["params"]["mcpServers"][0]["url"] == "http://127.0.0.1:17081/mcp",
                "ACP session/new must carry SSH -R forwarded loopback MCP URL");
        const std::string promptWithBothContexts =
            u8"请检查工程\n\n[SerialCtl associated terminal session: id=session-7]\n[SerialCtl active workspace: mode=local root=D:\\proj]";
        Require(StripInjectedSessionContext(promptWithBothContexts) == u8"请检查工程",
                "StripInjectedSessionContext must strip both terminal and workspace injected blocks");
        const std::string restoreDev =
            MakeRequest(Operation::RestoreConversation, 207, {{"conversationId", "t"}})["params"]["developerInstructions"]
                .get<std::string>();
        Require(restoreDev.find("mcp_workspace_*") != std::string::npos &&
                    std::string(EngineerInstructions()).find("mcp_workspace_exec") != std::string::npos &&
                    std::string(EngineerInstructions()).find("workspace.*") != std::string::npos,
                "Instructions must document built-in Windows local workspace tools");

        // Local read-only workspace binding round-trip
        const auto binding = ParseStoredWorkspaceBinding(
            Json{{"workspaceIsLocal", true},
                 {"localReadOnly", true},
                 {"localWorkspace", "D:\\Users\\demo\\proj"},
                 {"remoteWorkspace", "/home/linux/proj"},
                 {"remoteSshSessionId", "ssh-9"}});
        Require(binding.hasExplicitMode && binding.isLocal && binding.localReadOnly &&
                    binding.localPath == "D:\\Users\\demo\\proj" &&
                    binding.remotePath == "/home/linux/proj" &&
                    binding.remoteSessionId == "ssh-9",
                "ParseStoredWorkspaceBinding must restore localReadOnly and workspace fields");

        // Inline-formatted tool markup vs literal pipe preservation vs full thinking markdown
        {
            const auto toolBlocks = markup::Parse(
                L"**Run mcp_serialctl_workspace.list** · 已完成\n\n```json\n{\"depth\": 3}\n```\n\n**输出**\n```text\nsrc/\n```",
                false, true);
            Require(toolBlocks.size() == 6 &&
                        toolBlocks[0].kind == markup::Kind::Prose &&
                        !toolBlocks[0].lines.empty() &&
                        toolBlocks[0].lines[0].value == L"Run mcp_serialctl_workspace.list · 已完成" &&
                        toolBlocks[0].lines[0].spans.size() == 1 &&
                        toolBlocks[0].lines[0].spans[0].ink == markup::Ink::Bold &&
                        toolBlocks[2].kind == markup::Kind::Code &&
                        toolBlocks[4].kind == markup::Kind::Prose &&
                        !toolBlocks[4].lines.empty() &&
                        toolBlocks[4].lines[0].value == L"输出" &&
                        toolBlocks[4].lines[0].spans.size() == 1 &&
                        toolBlocks[4].lines[0].spans[0].ink == markup::Ink::Bold &&
                        toolBlocks[5].kind == markup::Kind::Code,
                    "Tool inline markup must style bold markers without splitting mcp_serialctl_workspace underscores");
            Require(markup::Parse(L"| 日志 | 内容 |\n| --- | --- |", false, true).front().kind == markup::Kind::Prose,
                    "Inline-formatted tool mode must still leave tables as literal text");
            const auto thinkBlocks = markup::Parse(L"### 检查目录\n- `list` 目录", true);
            Require(thinkBlocks.size() == 2 && thinkBlocks[0].kind == markup::Kind::Heading &&
                        thinkBlocks[1].kind == markup::Kind::List,
                    "Thinking blocks rendered with proseMarkup=true must parse headings and lists");
        }

        // Stateful ACP stream: partial tool update retains title/rawInput and extracts rawOutput/diff/locations/plan,
        // and interleaved thought -> message -> tool -> thought -> final keeps chronological IDs
        {
            AcpStreamState acp;
            acp.conversationId = "acp-conv-1";
            acp.taskId = "turn-1";

            auto ev1 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "agent_thought_chunk"},
                                         {"content", {{"type", "text"}, {"text", "First thought"}}}}}}}},
                acp);
            auto ev2 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "agent_message_chunk"},
                                         {"content", {{"type", "text"}, {"text", "Checking tree..."}}}}}}}},
                acp);
            auto ev3 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "tool_call"},
                                         {"toolCallId", "call-1"},
                                         {"title", "Run mcp_serialctl_workspace.list"},
                                         {"status", "in_progress"},
                                         {"rawInput", {{"depth", 3}}}}}}}},
                acp);
            Require(ev1.size() == 1 && ev1[0]["event"] == "thinkingDelta" &&
                        ev1[0]["messageId"] == "thinking:turn-1",
                    "First ACP thought chunk must start thinking:turn-1");
            Require(ev2.size() == 2 && ev2[0]["event"] == "textStarted" &&
                        ev2[1]["event"] == "textDelta",
                    "Pre-tool ACP commentary must start first message bubble");
            Require(ev3.size() == 2 && ev3[0]["event"] == "textComplete" && ev3[0]["final"] == false &&
                        ev3[1]["event"] == "serverTool" && ev3[1]["completed"] == false,
                    "Tool call following ACP commentary must seal commentary as final=false and emit serverTool");

            auto ev4 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "tool_call_update"},
                                         {"toolCallId", "call-1"},
                                         {"status", "completed"},
                                         {"locations", Json::array({{{"path", "src/main.cpp"}, {"line", 10}}})},
                                         {"content", Json::array({{{"type", "diff"}, {"path", "src/main.cpp"}, {"oldText", "a"}, {"newText", "b"}}})},
                                         {"rawOutput", {{"entries", Json::array({"README.md", "src/"})}}}}}}}},
                acp);
            Require(ev4.size() == 1 && ev4[0]["event"] == "serverTool" && ev4[0]["completed"] == true &&
                        ev4[0]["text"].get<std::string>().find("Run mcp_serialctl_workspace.list") != std::string::npos &&
                        ev4[0]["text"].get<std::string>().find("\"depth\": 3") != std::string::npos &&
                        ev4[0]["text"].get<std::string>().find("src/main.cpp") != std::string::npos &&
                        ev4[0]["text"].get<std::string>().find("README.md") != std::string::npos &&
                        ev4[0]["technical"].get<std::string>().find("rawOutput") != std::string::npos,
                    "Partial ACP tool_call_update must preserve title/rawInput and render locations/diff/rawOutput + technical JSON");

            auto ev5 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "plan"},
                                         {"entries", Json::array({{{"content", "Inspect files"}, {"status", "completed"}}})}}}}}},
                acp);
            Require(ev5.size() == 1 && ev5[0]["event"] == "serverTool" &&
                        ev5[0]["text"].get<std::string>().find("Inspect files") != std::string::npos,
                    "ACP plan update must surface checklist item");

            auto ev6 = DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-conv-1"},
                             {"update", {{"sessionUpdate", "agent_thought_chunk"},
                                         {"content", {{"type", "text"}, {"text", "Second thought"}}}}}}}},
                acp);
            Require(ev6.size() == 1 && ev6[0]["messageId"] == "thinking:turn-1:2",
                    "Thought after tool execution must start a distinct chronological thinking bubble");
        }

        // ACP session/load request & replay assembly + _meta.antigravity.systemPrompt
        {
            const auto acpLoadReq = MakeAcpRequest(
                Operation::RestoreConversation, 301,
                {{"conversationId", "acp-hist-1"}, {"cwd", "/home/linux/proj"}, {"loadSession", true}}, 17081);
            Require(acpLoadReq["method"] == "session/load" &&
                        acpLoadReq["params"]["sessionId"] == "acp-hist-1" &&
                        acpLoadReq["params"]["cwd"] == "/home/linux/proj" &&
                        !acpLoadReq["params"]["mcpServers"].empty() &&
                        !acpLoadReq["params"]["_meta"]["antigravity"]["systemPrompt"]["append"].get<std::string>().empty(),
                    "ACP session/load must carry sessionId, cwd, mcpServers and systemPrompt");

            const auto acpResumeReq = MakeAcpRequest(
                Operation::RestoreConversation, 302,
                {{"conversationId", "acp-hist-1"}, {"cwd", "/home/linux/proj"}, {"loadSession", false}}, 17081);
            Require(acpResumeReq["method"] == "session/resume" &&
                        acpResumeReq["params"]["sessionId"] == "acp-hist-1" &&
                        !acpResumeReq["params"]["_meta"]["antigravity"]["systemPrompt"]["append"].get<std::string>().empty(),
                    "ACP session/resume fallback must include sessionId and systemPrompt");

            AcpStreamState replay;
            replay.replayingHistory = true;
            replay.replayConversationId = "acp-hist-1";
            DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-hist-1"},
                             {"update", {{"sessionUpdate", "user_message_chunk"},
                                         {"content", {{"type", "text"}, {"text", u8"请分析\n\n[SerialCtl active workspace: mode=local root=D:\\proj]"}}}}}}}},
                replay);
            DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-hist-1"},
                             {"update", {{"sessionUpdate", "agent_thought_chunk"},
                                         {"content", {{"type", "text"}, {"text", "Thinking in history"}}}}}}}},
                replay);
            DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-hist-1"},
                             {"update", {{"sessionUpdate", "agent_message_chunk"},
                                         {"content", {{"type", "text"}, {"text", "Let me list files"}}}}}}}},
                replay);
            DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-hist-1"},
                             {"update", {{"sessionUpdate", "tool_call"},
                                         {"toolCallId", "hist-tool-1"},
                                         {"title", "mcp_workspace_list"},
                                         {"status", "completed"},
                                         {"rawOutput", {{"entries", Json::array({"a.txt"})}}}}}}}},
                replay);
            DecodeAcpNotification(
                {{"method", "session/update"},
                 {"params", {{"sessionId", "acp-hist-1"},
                             {"update", {{"sessionUpdate", "agent_message_chunk"},
                                         {"content", {{"type", "text"}, {"text", "Done listing"}}}}}}}},
                replay);
            FinalizeAcpReplay(replay);
            Require(replay.replayedMessages.size() == 5 &&
                        replay.replayedMessages[0]["role"] == u8"我" &&
                        replay.replayedMessages[0]["text"] == u8"请分析" &&
                        replay.replayedMessages[1]["role"] == u8"思考" &&
                        replay.replayedMessages[2]["role"] == "AI" &&
                        replay.replayedMessages[2]["process"] == true &&
                        replay.replayedMessages[3]["role"] == u8"工具" &&
                        replay.replayedMessages[3]["text"].get<std::string>().find("a.txt") != std::string::npos &&
                        replay.replayedMessages[3].contains("technical") &&
                        replay.replayedMessages[4]["role"] == "AI" &&
                        replay.replayedMessages[4]["process"] == false,
                    "ACP session/load replay must reconstruct user/thought/process-commentary/tool/final-answer order");
        }

        // Codex RestoreConversation mcpToolCall + summaryPartAdded notification separator
        {
            const auto codexRestored = DecodeResponse(
                Operation::RestoreConversation,
                {{"thread",
                  {{"id", "codex-t1"},
                   {"turns",
                    Json::array({{{"id", "turn-a"},
                                  {"status", "completed"},
                                  {"items",
                                   Json::array({{{"type", "userMessage"},
                                                 {"id", "u1"},
                                                 {"content", Json::array({{{"type", "text"}, {"text", "check"}}})}},
                                                {{"type", "reasoning"},
                                                 {"id", "r1"},
                                                 {"summary", Json::array({"Step 1", "Step 2"})}},
                                                {{"type", "mcpToolCall"},
                                                 {"id", "m1"},
                                                 {"server", "serialctl"},
                                                 {"tool", "workspace_list"},
                                                 {"status", "completed"},
                                                 {"arguments", {{"depth", 2}}},
                                                 {"result", {{"content", Json::array({{{"type", "text"}, {"text", "src/"}}})}}}},
                                                {{"type", "agentMessage"}, {"id", "a1"}, {"text", "OK"}}})}}})}}}});
            Require(codexRestored["messages"].size() == 4 &&
                        codexRestored["messages"][1]["text"] == "Step 1\n\nStep 2" &&
                        codexRestored["messages"][2]["role"] == u8"工具" &&
                        codexRestored["messages"][2]["text"].get<std::string>().find("workspace_list") != std::string::npos &&
                        codexRestored["messages"][2]["text"].get<std::string>().find("src/") != std::string::npos,
                    "Codex RestoreConversation must decode reasoning sections and mcpToolCall items");

            const auto firstPartEv = DecodeNotification({{"method", "item/reasoning/summaryPartAdded"},
                                                         {"params", {{"threadId", "t1"}, {"turnId", "k1"}, {"itemId", "r1"}, {"summaryIndex", 0}}}});
            Require(firstPartEv.value("event", std::string()) == "taskActivity",
                    "item/reasoning/summaryPartAdded summaryIndex=0 must not prepend leading blank lines");
            const auto sepEv = DecodeNotification({{"method", "item/reasoning/summaryPartAdded"},
                                                   {"params", {{"threadId", "t1"}, {"turnId", "k1"}, {"itemId", "r1"}, {"summaryIndex", 1}}}});
            Require(sepEv.value("event", std::string()) == "thinkingDelta" &&
                        sepEv.value("separatorOnly", false) == true &&
                        sepEv.value("text", std::string()) == "\n\n",
                    "item/reasoning/summaryPartAdded must emit double-newline separatorOnly thinkingDelta");
        }

        // WorkspaceActivity formatting, newline normalization, and unified diff helpers
        {
            Require(NormalizeLineEndingsForTerminal("a\nb\r\nc\rd") == "a\r\nb\r\nc\rd",
                    "NormalizeLineEndingsForTerminal must normalize LF to CRLF while preserving CRLF and CR");

            const std::string runningMd = FormatCompactExecMarkdown(
                "Run workspace.exec", "cmake --build build", "ex-1", false, false, 0, false, false, 0);
            Require(runningMd.find("```bash\ncmake --build build\n```") != std::string::npos &&
                        runningMd.find("serialctl-exec://ex-1") != std::string::npos,
                    "FormatCompactExecMarkdown must wrap command in fenced bash block and keep serialctl-exec deep link");

            const std::string doneMd = FormatCompactExecMarkdown(
                "Run workspace.exec", "cmake --build build", "ex-1", true, false, 0, false, false, 1250);
            Require(doneMd.find("serialctl-exec://ex-1") != std::string::npos &&
                        doneMd != runningMd,
                    "FormatCompactExecMarkdown completed format must differ from running format and keep link");

            FileChangeRecord rec;
            rec.changeId = "chg-9";
            rec.action = "edit";
            rec.path = "src/foo.cpp";
            rec.addedLines = 4;
            rec.removedLines = 2;
            const std::string linkMd = FormatFileChangeLinkMarkdown(rec);
            const std::string cardMd = FormatCompactFileChangeCardMarkdown(rec);
            Require(linkMd.find("src/foo.cpp") != std::string::npos &&
                        linkMd.find("+4 / -2") != std::string::npos &&
                        linkMd.find("serialctl-change://chg-9") != std::string::npos &&
                        cardMd.find("serialctl-change://chg-9") != std::string::npos,
                    "File change formatters must emit clickable serialctl-change markdown links with stats");
        }

        std::cout
            << "AiProtocol: framing, UTF-8, bounds, scroll anchoring, tool output and command validation passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
