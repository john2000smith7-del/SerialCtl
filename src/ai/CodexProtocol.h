#pragma once
#include "AiProtocol.h"
#include "WorkspaceActivity.h"
#include <map>

namespace serialctl::ai {
// UI-facing operations. Only this adapter knows the App Server method names,
// parameter casing, experimental fields and persisted thread item shapes.
enum class Operation {
    Models,
    Defaults,
    NewConversation,
    StartTask,
    StopTask,
    RestoreConversation,
    CompactConversation,
    ServerHome,
    ReadDirectory,
    DirectoryFallback,
    RunCommand,
    SetConfig
};
inline Json MakeRequest(Operation operation, std::uint64_t token, const Json &data) {
    Json params = Json::object();
    const char *method = "";
    const Json policy = {{"features.shell_tool", true}, {"features.multi_agent", false}, {"web_search", "disabled"}};
    switch (operation) {
    case Operation::RunCommand:
        method = "command/exec";
        params = {{"command", data.at("command")},
                  {"timeoutMs", data.value("timeoutMs", 5000)},
                  {"outputBytesCap", data.value("outputBytesCap", 65536)},
                  {"sandbox", "danger-full-access"},
                  {"sandboxPolicy", {{"type", "dangerFullAccess"}}},
                  {"approvalPolicy", "never"}};
        if (data.contains("cwd")) params["cwd"] = data["cwd"];
        break;
    case Operation::ServerHome:
        method = "command/exec";
        params = {{"command", Json::array({"printenv", "HOME"})}, {"timeoutMs", 3000}, {"outputBytesCap", 4096}};
        break;
    case Operation::ReadDirectory:
        method = "fs/readDirectory";
        params = {{"path", data.at("path")}};
        break;
    case Operation::DirectoryFallback:
        method = "command/exec";
        params = {{"command", Json::array({"find", "-L", data.at("path"), "-mindepth", "1", "-maxdepth", "1", "-type", "d", "-print0"})},
            {"timeoutMs", 3000}, {"outputBytesCap", 65536}};
        break;
    case Operation::Models:
        method = "model/list";
        params = {{"limit", 100}};
        if (data.contains("cursor"))
            params["cursor"] = data["cursor"];
        break;
    case Operation::Defaults:
        method = "config/read";
        params = {{"includeLayers", false}};
        break;
    case Operation::NewConversation:
        method = "thread/start";
        params = {
            {"baseInstructions", EngineerInstructions()},
            {"developerInstructions", u8"可使用 Codex 原生工具读取、修改服务器项目代码并执行服务器命令，权限限于 SSH 登录用户。Windows 内的终端、电源及本程序其他功能可通过 serialctl_ 和 mcp_* 动态工具直接操作；显示实际路径、命令、输出和文件修改结果，不混淆服务器与板端。"},
            {"sandbox", "danger-full-access"},
            {"sandboxPolicy", {{"type", "dangerFullAccess"}}},
            {"approvalPolicy", "never"},
            {"dynamicTools", AllDynamicTools()},
            {"config", policy}};
        break;
    case Operation::StartTask:
        method = "turn/start";
        params = {{"threadId", data.at("conversationId")},
                  {"approvalPolicy", "never"},
                  {"input", Json::array({{{"type", "text"}, {"text", data.at("text")}}})}};
        if (data.contains("images"))
            for (const auto &path : data.at("images"))
                params["input"].push_back({{"type", "localImage"}, {"path", path}});
        params["sandboxPolicy"] = {{"type", "dangerFullAccess"}};
        break;
    case Operation::StopTask:
        method = "turn/interrupt";
        params = {{"threadId", data.at("conversationId")}, {"turnId", data.at("taskId")}};
        break;
    case Operation::CompactConversation:
        method = "thread/compact/start";
        params = {{"threadId", data.at("conversationId")}};
        break;
    case Operation::RestoreConversation:
        method = "thread/resume";
        params = {{"threadId", data.at("conversationId")},
                  {"sandbox", "danger-full-access"},
                  {"sandboxPolicy", {{"type", "dangerFullAccess"}}},
                  {"approvalPolicy", "never"},
                  {"baseInstructions", EngineerInstructions()},
                  {"developerInstructions", u8"工程项目可位于远端服务器或 Windows 本地工作空间；当本回合注入声明当前激活为 Windows 本地工作空间时，一律通过内置 mcp_workspace_* 工具操作本地目录，不要假定本机安装有 git/bash/rg/python；远程目录才使用原生文件及命令工具。终端、电源及本程序功能使用已注册的 serialctl_ 与 mcp_* 工具。"},
                  {"dynamicTools", AllDynamicTools()},
                  {"config", policy}};
        break;
    case Operation::SetConfig:
        return Json();
    }
    if (operation == Operation::NewConversation || operation == Operation::RestoreConversation || operation == Operation::StartTask) {
        // Restoring a thread retains its own workspace, not the latest global default.
        if (operation != Operation::RestoreConversation && !data.value("cwd", std::string()).empty()) params["cwd"] = data["cwd"];
        // This value comes from config/read, never a user model selector or old registry preference.
        if (!data.value("defaultModel", std::string()).empty()) params["model"] = data["defaultModel"];
        if (!data.value("effort", std::string()).empty()) {
            if (operation == Operation::StartTask)
                params["effort"] = data["effort"];
            else
                params["config"]["model_reasoning_effort"] = data["effort"];
        }
    }
    return {{"id", token}, {"method", method}, {"params", params}};
}
inline std::string TranslateItemStatus(const std::string &status) {
    if (status == "completed") return u8"已完成";
    if (status == "failed") return u8"失败";
    if (status == "inProgress" || status == "in_progress") return u8"执行中";
    if (status == "declined") return u8"已拒绝";
    if (status == "interrupted") return u8"已停止";
    return status.empty() ? std::string(u8"未知") : status;
}

inline std::string StripInjectedSessionContext(std::string text) {
    bool strippedAny = true;
    while (strippedAny) {
        strippedAny = false;
        for (std::string_view marker :
             {std::string_view("[SerialCtl associated terminal session:"),
              std::string_view("[SerialCtl active workspace:")}) {
            const size_t pos = text.rfind(marker);
            if (pos == std::string::npos)
                continue;
            const size_t close = text.find(']', pos);
            if (close == std::string::npos)
                continue;
            bool onlyTrailingWhitespace = true;
            for (size_t i = close + 1; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                    onlyTrailingWhitespace = false;
                    break;
                }
            }
            if (!onlyTrailingWhitespace)
                continue;
            if (pos == 0 || text[pos - 1] == '\n') {
                size_t cut = pos;
                while (cut > 0 && (text[cut - 1] == '\n' || text[cut - 1] == '\r'))
                    --cut;
                text.resize(cut);
                strippedAny = true;
                break;
            }
        }
    }
    return text;
}

inline void TruncateUtf8Bytes(std::string &text, size_t maxBytes, const std::string &suffix) {
    if (text.size() <= maxBytes) return;
    size_t end = maxBytes;
    size_t start = end;
    while (start > 0 && end - start < 3 && (static_cast<unsigned char>(text[start - 1]) & 0xc0) == 0x80) --start;
    if (start > 0) {
        const unsigned char lead = static_cast<unsigned char>(text[start - 1]);
        const size_t expected = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (lead >= 0xc0 && end - (start - 1) < expected) end = start - 1;
    }
    text.resize(end);
    text += suffix;
}

inline void TrimTrailingLineBreaks(std::string &text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
}

inline std::string FormatStructuredOrTextFence(const std::string &heading, std::string body, size_t maxBytes = 16384) {
    TrimTrailingLineBreaks(body);
    if (body.empty()) return {};
    std::string lang = "text";
    size_t firstNonWs = 0;
    while (firstNonWs < body.size() &&
           (body[firstNonWs] == ' ' || body[firstNonWs] == '\t' || body[firstNonWs] == '\r' || body[firstNonWs] == '\n'))
        ++firstNonWs;
    if (firstNonWs < body.size() && (body[firstNonWs] == '{' || body[firstNonWs] == '[')) {
        auto parsed = Json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
            body = parsed.dump(2);
            lang = "json";
        }
    }
    TruncateUtf8Bytes(body, maxBytes, u8"\n[输出已截断]");
    return "\n\n" + heading + "\n\n```" + lang + "\n" + body + "\n```";
}

inline std::string ExtractMcpResultText(const Json &val) {
    if (val.is_null()) return {};
    if (val.is_string()) return val.get<std::string>();
    if (!val.is_object()) return val.is_array() && !val.empty() ? val.dump(2) : std::string();
    std::string out;
    if (val.contains("content") && val["content"].is_array()) {
        for (const auto &c : val["content"]) {
            if (c.is_string()) {
                if (!out.empty()) out += "\n";
                out += c.get<std::string>();
            } else if (c.is_object()) {
                if (c.contains("text") && c["text"].is_string()) {
                    if (!out.empty()) out += "\n";
                    out += c["text"].get<std::string>();
                } else if (c.contains("content") && c["content"].is_object() && c["content"].contains("text") &&
                           c["content"]["text"].is_string()) {
                    if (!out.empty()) out += "\n";
                    out += c["content"]["text"].get<std::string>();
                } else if (c.contains("resource") && c["resource"].is_object()) {
                    const auto &res = c["resource"];
                    const auto uri = res.value("uri", std::string());
                    const auto rtext = res.value("text", std::string());
                    if (!uri.empty() || !rtext.empty()) {
                        if (!out.empty()) out += "\n";
                        if (!uri.empty()) out += "[资源: " + uri + "]\n";
                        out += rtext;
                    }
                }
            }
        }
    }
    if (!out.empty()) return out;
    if (val.contains("structuredContent") && !val["structuredContent"].is_null())
        return val["structuredContent"].dump(2);
    for (const char *key : {"output", "text", "message"}) {
        if (val.contains(key) && val[key].is_string() && !val[key].get_ref<const std::string &>().empty())
            return val[key].get<std::string>();
    }
    if (val.contains("stdout") || val.contains("stderr") || val.contains("exitCode")) {
        if (val.contains("exitCode") && val["exitCode"].is_number_integer())
            out += u8"退出码 " + val["exitCode"].dump();
        const auto so = val.value("stdout", std::string());
        const auto se = val.value("stderr", std::string());
        if (!so.empty()) { if (!out.empty()) out += "\n"; out += so; }
        if (!se.empty()) { if (!out.empty()) out += "\n"; out += "[stderr]\n" + se; }
        if (!out.empty()) return out;
    }
    return val.empty() ? std::string() : val.dump(2);
}

inline std::string NativeItemText(const Json& item, bool completed) {
    std::string text;
    const auto type = item.value("type", std::string());
    if (type == "commandExecution") {
        const auto cwd = item.value("cwd", std::string());
        text = u8"**服务器命令**" + (cwd.empty() ? std::string() : " · `" + cwd + "`") +
               "\n\n```bash\n" + item.value("command", std::string()) + "\n```\n\n";
        text += completed ? u8"状态：" + TranslateItemStatus(item.value("status", std::string())) : u8"状态：执行中";
        if (item.contains("exitCode") && item["exitCode"].is_number_integer())
            text += u8" · 退出码 " + item["exitCode"].dump();
        if (item.contains("aggregatedOutput") && item["aggregatedOutput"].is_string()) {
            auto output = item["aggregatedOutput"].get<std::string>();
            TrimTrailingLineBreaks(output);
            if (!output.empty())
                text += u8"\n\n**输出**\n\n```text\n" + output + "\n```";
        }
    } else if (type == "mcpToolCall" || type == "dynamicToolCall") {
        const auto server = item.value("server", std::string());
        const auto tool = item.value("tool", item.value("name", std::string(u8"工具调用")));
        const auto status = completed ? item.value("status", std::string("completed"))
                                      : item.value("status", std::string("inProgress"));
        const std::string title = server.empty() ? tool : (server + " · " + tool);
        text = "**" + title + "** · " + TranslateItemStatus(status);
        if (item.contains("arguments") && !item["arguments"].is_null()) {
            const auto &args = item["arguments"];
            if (args.is_string() && !args.get<std::string>().empty())
                text += "\n\n```text\n" + args.get<std::string>() + "\n```";
            else if (args.is_object() && !args.empty())
                text += "\n\n```json\n" + args.dump(2) + "\n```";
        }
        if (item.contains("result") && !item["result"].is_null())
            text += FormatStructuredOrTextFence(u8"**输出**", ExtractMcpResultText(item["result"]));
        if (item.contains("error") && !item["error"].is_null()) {
            const auto errStr = item["error"].is_string() ? item["error"].get<std::string>() : item["error"].dump(2);
            text += FormatStructuredOrTextFence(u8"**错误**", errStr, 4096);
        }
    } else if (type == "webSearch") {
        const auto query = item.value("query", item.value("action", Json::object()).value("query", std::string()));
        const auto status = completed ? item.value("status", std::string("completed"))
                                      : item.value("status", std::string("inProgress"));
        text = std::string(u8"**网页检索** · ") + TranslateItemStatus(status);
        if (!query.empty()) text += "\n\n```text\n" + query + "\n```";
        if (item.contains("result") && !item["result"].is_null())
            text += FormatStructuredOrTextFence(u8"**输出**", ExtractMcpResultText(item["result"]));
    } else {
        text = u8"**服务器文件修改** · " + TranslateItemStatus(item.value("status", std::string()));
        for (const auto& change : item.value("changes", Json::array())) {
            auto diff = change.value("diff", std::string());
            TrimTrailingLineBreaks(diff);
            text += "\n\n文件：`" + change.value("path", std::string()) + "`\n\n```diff\n" + diff + "\n```";
        }
    }
    TruncateUtf8Bytes(text, 65536, u8"\n```\n[服务器工具输出已截断]");
    return text;
}
inline std::string ExtractSandbox(const Json &obj) {
    if (!obj.is_object()) return {};
    for (const char *key : {"effectiveSandbox", "sandbox", "sandboxPolicy"}) {
        const auto policy = obj.find(key);
        if (policy == obj.end()) continue;
        if (policy->is_string()) return policy->get<std::string>();
        if (policy->is_object()) {
            const auto type = policy->find("type");
            if (type != policy->end() && type->is_string()) return type->get<std::string>();
        }
    }
    for (const char *sub : {"thread", "turn", "config"}) {
        if (obj.contains(sub) && obj[sub].is_object()) {
            auto s = ExtractSandbox(obj[sub]);
            if (!s.empty()) return s;
        }
    }
    return {};
}

inline std::string ExtractApprovalPolicy(const Json &obj) {
    if (!obj.is_object()) return {};
    if (obj.contains("approvalPolicy") && obj["approvalPolicy"].is_string()) return obj["approvalPolicy"].get<std::string>();
    if (obj.contains("effectiveApprovalPolicy") && obj["effectiveApprovalPolicy"].is_string()) return obj["effectiveApprovalPolicy"].get<std::string>();
    for (const char *sub : {"thread", "turn", "config"}) {
        if (obj.contains(sub) && obj[sub].is_object()) {
            auto p = ExtractApprovalPolicy(obj[sub]);
            if (!p.empty()) return p;
        }
    }
    return {};
}

inline Json DecodeResponse(Operation operation, const Json &result) {
    Json decoded = Json::object();
    switch (operation) {
    case Operation::ServerHome:
    case Operation::ReadDirectory:
    case Operation::DirectoryFallback:
    case Operation::RunCommand:
        return result;
    case Operation::NewConversation:
        decoded = {{"conversationId", result.at("thread").at("id")},
            {"cwd", result.at("thread").contains("cwd") && result.at("thread")["cwd"].is_string() ? result.at("thread")["cwd"] : Json("")}};
        break;
    case Operation::StartTask:
        decoded = {{"taskId", result.at("turn").at("id")}};
        break;
    case Operation::StopTask:
    case Operation::CompactConversation:
        return Json::object();
    case Operation::Defaults: {
        const auto config = result.value("config", Json::object());
        const auto effort = config.contains("model_reasoning_effort") && config["model_reasoning_effort"].is_string()
                                ? config["model_reasoning_effort"]
                                : (config.contains("reasoningEffort") && config["reasoningEffort"].is_string()
                                       ? config["reasoningEffort"]
                                       : Json(""));
        decoded = {{"defaultModel", config.contains("model") && config["model"].is_string() ? config["model"] : Json("")},
                   {"defaultEffort", effort}};
        break;
    }
    case Operation::Models: {
        Json models = Json::array();
        for (const auto &model : result.value("data", Json::array())) {
            auto id = model.value("model", model.value("id", std::string()));
            if (id.empty())
                continue;
            Json efforts = Json::array();
            for (const auto &r : model.value("supportedReasoningEfforts", Json::array())) {
                if (r.is_object() && r.contains("reasoningEffort") && r["reasoningEffort"].is_string())
                    efforts.push_back(r["reasoningEffort"]);
                else if (r.is_string())
                    efforts.push_back(r);
            }
            models.push_back({{"id", id},
                              {"label", model.value("displayName", id)},
                              {"reasoning", !efforts.empty()},
                              {"defaultEffort", model.value("defaultReasoningEffort", std::string())},
                              {"efforts", efforts}});
        }
        return {{"models", models}, {"nextCursor", result.value("nextCursor", Json())}};
    }
    case Operation::RestoreConversation: {
        const auto &thread = result.at("thread");
        Json messages = Json::array();
        for (const auto &turn : thread.value("turns", Json::array())) {
            const auto first = messages.size();
            for (const auto &item : turn.value("items", Json::array())) {
                const auto type = item.value("type", std::string());
                if (type == "agentMessage")
                    messages.push_back({{"role", "AI"},
                                        {"text", item.value("text", std::string())},
                                        {"messageId", item.value("id", std::string())},
                                        {"process", item.value("phase", Json()) == "commentary"}});
                else if (type == "commandExecution" || type == "fileChange" ||
                         type == "mcpToolCall" || type == "dynamicToolCall" || type == "webSearch")
                    messages.push_back({{"role", u8"工具"}, {"text", NativeItemText(item, true)},
                        {"messageId", "server:" + item.value("id", std::string())}});
                else if (type == "reasoning") {
                    std::string text;
                    for (const auto &s : item.value("summary", Json::array()))
                        if (s.is_string() && !s.get_ref<const std::string &>().empty()) {
                            if (!text.empty()) text += "\n\n";
                            text += s.get<std::string>();
                        }
                    for (const auto &c : item.value("content", Json::array()))
                        if (c.is_string() && !c.get_ref<const std::string &>().empty()) {
                            if (!text.empty()) text += "\n\n";
                            text += c.get<std::string>();
                        }
                    if (!text.empty())
                        messages.push_back({{"role", u8"思考"},
                                            {"text", std::move(text)},
                                            {"messageId", "thinking:" + item.value("id", std::string())},
                                            {"process", true}});
                } else if (type == "userMessage") {
                    std::string text;
                    for (const auto &part : item.value("content", Json::array()))
                        if (part.value("type", "") == "text")
                            text += part.value("text", std::string());
                    messages.push_back({{"role", u8"我"},
                                        {"text", StripInjectedSessionContext(std::move(text))},
                                        {"messageId", item.value("id", std::string())}});
                }
            }
            bool failedStep = false;
            for (const auto &source : turn.value("items", Json::array()))
                if (source.value("status", std::string()) == "failed" ||
                    (source.contains("exitCode") && source["exitCode"].is_number_integer() &&
                     source["exitCode"].get<int>() != 0))
                    failedStep = true;
            for (size_t i = first; i < messages.size(); ++i) {
                messages[i]["group"] = turn.value("id", std::string());
                // A failed diagnostic command is different from a failed turn.
                messages[i]["status"] = turn.value("status", std::string());
                messages[i]["failedStep"] = failedStep;
            }
        }
        decoded = {{"conversationId", thread.at("id")}, {"messages", messages},
            {"cwd", thread.contains("cwd") && thread["cwd"].is_string() ? thread["cwd"] : Json("")}};
        break;
    }
    default:
        return Json::object();
    }
    if (operation == Operation::Defaults || operation == Operation::NewConversation ||
        operation == Operation::RestoreConversation || operation == Operation::StartTask) {
        auto sbox = ExtractSandbox(result);
        if (!sbox.empty()) decoded["effectiveSandbox"] = sbox;
        auto apprv = ExtractApprovalPolicy(result);
        if (!apprv.empty()) decoded["effectiveApprovalPolicy"] = apprv;
    }
    return decoded;
}
inline Json DecodeNotification(const Json &wire) {
    if (wire.contains("event"))
        return wire; // transport state from CodexClient
    const auto method = wire.value("method", std::string());
    // Ignore unrelated notifications before inspecting their arbitrary params.
    // Presence/order of these notifications does not consume a request ID.
    if (!wire.contains("id") && method != "item/agentMessage/delta" && method != "item/completed" &&
        method != "item/started" && method != "item/reasoning/textDelta" &&
        method != "item/reasoning/summaryTextDelta" && method != "item/reasoning/summaryPartAdded" &&
        method != "turn/started" && method != "turn/completed" &&
        method != "item/commandExecution/outputDelta" && method != "thread/compacted" && method != "error")
        return Json();
    const auto p = wire.value("params", Json::object());
    if (wire.contains("id")) {
        if (method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval")
            return {{"event", "serverApproval"}, {"requestId", wire.at("id")},
                {"conversationId", p.value("threadId", std::string())}, {"taskId", p.value("turnId", std::string())}};
        if (method != "item/tool/call")
            return {{"event", "unsupportedRequest"},
                    {"requestId", wire.at("id")},
                    {"approval",
                     method == "item/commandExecution/requestApproval" || method == "item/fileChange/requestApproval"}};
        return {{"event", "serialToolRequest"},
                {"requestId", wire.at("id")},
                {"conversationId", p.value("threadId", std::string())},
                {"taskId", p.value("turnId", std::string())},
                {"callId", p.value("callId", wire.at("id").dump())},
                {"tool", p.value("tool", std::string())},
                {"arguments", p.value("arguments", Json::object())}};
    }
    Json event = {{"conversationId", p.value("threadId", std::string())}, {"taskId", p.value("turnId", std::string())}};
    const auto itemType = p.value("item", Json::object()).value("type", std::string());
    if ((method == "item/started" || method == "item/completed") &&
        (itemType == "commandExecution" || itemType == "fileChange" ||
         itemType == "mcpToolCall" || itemType == "dynamicToolCall" || itemType == "webSearch")) {
        event["event"] = "serverTool";
        event["messageId"] = "server:" + p["item"].value("id", std::string());
        event["completed"] = method == "item/completed";
        event["failed"] = p["item"].value("status", std::string()) == "failed" ||
                          (p["item"].contains("exitCode") && p["item"]["exitCode"].is_number_integer() &&
                           p["item"]["exitCode"].get<int>() != 0) ||
                          (p["item"].contains("error") && !p["item"]["error"].is_null());
        event["appendStream"] = itemType == "commandExecution" &&
            (!p["item"].contains("aggregatedOutput") || !p["item"]["aggregatedOutput"].is_string());
        event["text"] = NativeItemText(p["item"], method == "item/completed");
        if (itemType == "commandExecution") {
            event["execKind"] = "codex";
            event["execId"] = p["item"].value("id", std::string());
            event["command"] = p["item"].value("command", std::string());
            event["cwd"] = p["item"].value("cwd", std::string());
            if (p["item"].contains("exitCode")) event["exitCode"] = p["item"]["exitCode"];
            if (p["item"].contains("durationMs")) event["durationMs"] = p["item"]["durationMs"];
            if (p["item"].contains("aggregatedOutput") && p["item"]["aggregatedOutput"].is_string())
                event["execOutput"] = p["item"]["aggregatedOutput"].get<std::string>();
        }
        if (itemType == "fileChange" && method == "item/completed") {
            Json fc = Json::array();
            for (const auto &change : p["item"].value("changes", Json::array())) {
                if (!change.is_object()) continue;
                const auto path = change.value("path", std::string());
                const auto diff = change.value("diff", std::string());
                std::string act = "edit";
                if (change.contains("kind") && change["kind"].is_object()) {
                    const auto ktype = change["kind"].value("type", std::string());
                    if (ktype == "add" || ktype == "create") act = "create";
                    else if (ktype == "delete") act = "delete";
                }
                size_t added = 0, removed = 0;
                CountDiffChanges(diff, added, removed);
                fc.push_back({{"path", path}, {"action", act}, {"diff", diff},
                              {"addedLines", added}, {"removedLines", removed}});
            }
            if (!fc.empty()) event["fileChanges"] = std::move(fc);
        }
        if (itemType == "mcpToolCall" || itemType == "dynamicToolCall" || itemType == "webSearch") {
            auto rawTech = p["item"].dump(2);
            TruncateUtf8Bytes(rawTech, 32768, u8"\n[技术详情已截断]");
            event["technical"] = std::move(rawTech);
        }
    } else if (method == "item/commandExecution/outputDelta") {
        event["event"] = "serverToolDelta";
        event["messageId"] = "server:" + p.value("itemId", std::string());
        event["execId"] = p.value("itemId", std::string());
        event["execDelta"] = true;
        event["text"] = p.value("delta", std::string());
    } else if (method == "item/started" && itemType == "agentMessage") {
        event["event"] = "textStarted";
        event["messageId"] = p["item"].value("id", std::string());
        event["final"] = p["item"].value("phase", Json()) == "final_answer";
    } else if (method == "item/agentMessage/delta") {
        event["event"] = "textDelta";
        event["messageId"] = p.value("itemId", std::string());
        event["text"] = p.value("delta", std::string());
    } else if (method == "item/completed" && itemType == "agentMessage") {
        const auto &item = p["item"];
        event["event"] = "textComplete";
        event["messageId"] = item.value("id", std::string());
        event["text"] = item.value("text", std::string());
        event["final"] = !item.contains("phase") || item["phase"].is_null() || item["phase"] == "final_answer";
    } else if (method == "item/completed" && itemType == "contextCompaction") {
        event["event"] = "contextCompacted";
    } else if (method == "item/reasoning/summaryPartAdded") {
        if (p.value("summaryIndex", 0) > 0) {
            event["event"] = "thinkingDelta";
            event["messageId"] = "thinking:" + p.value("itemId", std::string("reasoning"));
            event["text"] = "\n\n";
            event["separatorOnly"] = true;
        } else {
            event["event"] = "taskActivity";
        }
    } else if (method == "item/reasoning/textDelta" || method == "item/reasoning/summaryTextDelta") {
        const auto delta = p.value("delta", std::string());
        if (delta.empty()) {
            event["event"] = "taskActivity";
        } else {
            event["event"] = "thinkingDelta";
            event["messageId"] = "thinking:" + p.value("itemId", std::string("reasoning"));
            event["text"] = delta;
        }
    } else if (method == "item/started") {
        event["event"] = "taskActivity";
    } else if (method == "turn/started") {
        event["event"] = "taskStarted";
        event["taskId"] = p.at("turn").value("id", std::string());
    } else if (method == "turn/completed") {
        const auto turn = p.value("turn", Json::object());
        event["event"] = "taskFinished";
        event["taskId"] = turn.value("id", std::string());
        event["status"] = turn.value("status", std::string());
        const auto error = turn.value("error", Json::object());
        event["message"] = error.is_object() ? error.value("message", std::string()) : std::string();
    } else if (method == "thread/compacted") {
        event["event"] = "contextCompacted";
    } else if (method == "error") {
        event["event"] = "serviceError";
        const auto error = p.value("error", Json::object());
        event["message"] =
            error.is_object() ? error.value("message", std::string("Service error")) : std::string("Service error");
        event["willRetry"] = p.value("willRetry", false);
    } else
        return Json();
    return event;
}

inline Json MakeAcpRequest(Operation operation, std::uint64_t token, const Json &data, unsigned mcpPort = 7080) {
    const unsigned port = mcpPort ? mcpPort : 7080;
    const Json mcpServers = Json::array({{{"type", "http"},
                                          {"name", "serialctl"},
                                          {"url", "http://127.0.0.1:" + std::to_string(port) + "/mcp"},
                                          {"headers", Json::array()}}});
    const Json acpMeta = {{"antigravity",
                           {{"systemPrompt",
                             {{"append", std::string(EngineerInstructions()) + "\n" +
                                             std::string(u8"可使用 Antigravity 工具读取、修改项目代码并执行命令。"
                                                            u8"Windows 内的终端、电源及本程序其他功能可通过 serialctl MCP 工具直接操作；"
                                                            u8"显示实际路径、命令、输出和文件修改结果，不混淆服务器与板端。")}}}}}};
    const std::string cwd = data.value("cwd", std::string()).empty() ? std::string("/home/linux/workspace")
                                                                     : data.at("cwd").get<std::string>();
    switch (operation) {
    case Operation::NewConversation:
        return {{"jsonrpc", "2.0"},
                {"id", token},
                {"method", "session/new"},
                {"params",
                 {{"cwd", cwd},
                  {"mcpServers", mcpServers},
                  {"_meta", acpMeta}}}};
    case Operation::RestoreConversation:
        return {{"jsonrpc", "2.0"},
                {"id", token},
                {"method", data.value("loadSession", false) ? "session/load" : "session/resume"},
                {"params",
                 {{"sessionId", data.at("conversationId")},
                  {"cwd", cwd},
                  {"mcpServers", mcpServers},
                  {"_meta", acpMeta}}}};
    case Operation::StartTask: {
        Json prompt = Json::array({{{"type", "text"}, {"text", data.at("text")}}});
        if (data.contains("images") && data["images"].is_array() && !data["images"].empty()) {
            std::string note = "\n[Attached image paths:";
            for (const auto &img : data["images"])
                if (img.is_string()) note += " " + img.get<std::string>();
            note += "]";
            prompt[0]["text"] = prompt[0]["text"].get<std::string>() + note;
        }
        return {{"jsonrpc", "2.0"},
                {"id", token},
                {"method", "session/prompt"},
                {"params", {{"sessionId", data.at("conversationId")}, {"prompt", prompt}}}};
    }
    case Operation::StopTask:
        return {{"jsonrpc", "2.0"},
                {"method", "session/cancel"},
                {"params", {{"sessionId", data.at("conversationId")}}}};
    case Operation::SetConfig:
        return {{"jsonrpc", "2.0"},
                {"id", token},
                {"method", "session/set_config_option"},
                {"params",
                 {{"sessionId", data.at("conversationId")},
                  {"configId", data.at("configId")},
                  {"value", data.at("value")}}}};
    default:
        return Json();
    }
}

inline void ExtractAcpConfig(const Json &result, Json &decoded) {
    Json models = Json::array();
    Json efforts = Json::array();
    if (result.contains("configOptions") && result["configOptions"].is_array()) {
        for (const auto &opt : result["configOptions"]) {
            if (!opt.is_object()) continue;
            const auto id = opt.value("id", std::string());
            if (id == "model") {
                if (opt.contains("currentValue") && opt["currentValue"].is_string())
                    decoded["defaultModel"] = opt["currentValue"];
                for (const auto &item : opt.value("options", Json::array())) {
                    if (!item.is_object()) continue;
                    const auto val = item.value("value", std::string());
                    if (val.empty()) continue;
                    models.push_back({{"id", val}, {"label", item.value("name", val)}, {"reasoning", true}});
                }
            } else if (id == "thought_level") {
                if (opt.contains("currentValue") && opt["currentValue"].is_string())
                    decoded["defaultEffort"] = opt["currentValue"];
                for (const auto &item : opt.value("options", Json::array())) {
                    if (!item.is_object()) continue;
                    const auto val = item.value("value", std::string());
                    if (!val.empty()) efforts.push_back(val);
                }
            }
        }
    }
    if (result.contains("models") && result["models"].is_object()) {
        const auto &m = result["models"];
        if ((!decoded.contains("defaultModel") || decoded["defaultModel"].get<std::string>().empty()) &&
            m.contains("currentModelId") && m["currentModelId"].is_string())
            decoded["defaultModel"] = m["currentModelId"];
        if (models.empty()) {
            for (const auto &item : m.value("availableModels", Json::array())) {
                if (!item.is_object()) continue;
                const auto mid = item.value("modelId", std::string());
                if (mid.empty()) continue;
                models.push_back({{"id", mid}, {"label", item.value("name", mid)}, {"reasoning", true}});
            }
        }
    }
    if (!models.empty()) decoded["models"] = std::move(models);
    if (!efforts.empty()) decoded["efforts"] = std::move(efforts);
}

inline Json DecodeAcpResponse(Operation operation, const Json &result, const std::string &conversationIdHint = {},
                              const std::string &cwdHint = {}, const Json &replayedMessages = Json::array()) {
    switch (operation) {
    case Operation::NewConversation: {
        Json decoded = {{"conversationId", result.at("sessionId")}, {"cwd", cwdHint}};
        ExtractAcpConfig(result, decoded);
        return decoded;
    }
    case Operation::RestoreConversation: {
        const auto convId = result.contains("sessionId") && result["sessionId"].is_string()
                                  ? result["sessionId"].get<std::string>()
                                  : conversationIdHint;
        const auto resolvedCwd =
            result.contains("cwd") && result["cwd"].is_string() && !result["cwd"].get_ref<const std::string &>().empty()
                ? result["cwd"].get<std::string>()
                : cwdHint;
        Json decoded = {{"conversationId", convId},
                        {"messages", replayedMessages.is_array() ? replayedMessages : Json::array()},
                        {"cwd", resolvedCwd}};
        ExtractAcpConfig(result, decoded);
        return decoded;
    }
    case Operation::SetConfig: {
        Json decoded = Json::object();
        ExtractAcpConfig(result, decoded);
        return decoded;
    }
    default:
        return Json::object();
    }
}

struct AcpToolState {
    std::string title, kind, status = "in_progress";
    Json locations = Json::array();
    Json rawInput, rawOutput;
    Json content = Json::array();
};

struct AcpStreamState {
    enum class Phase { None, User, Thought, Message, Tool };
    std::string conversationId, taskId;
    std::string currentThoughtId, currentMessageId, accumulatedText;
    bool textStarted = false;
    Phase lastPhase = Phase::None;
    unsigned thoughtSeq = 0, messageSeq = 0;
    std::uint64_t activePromptToken = 0;
    std::map<std::string, AcpToolState> tools;

    // History replay accumulator used while session/load streams session/update notifications.
    bool replayingHistory = false;
    std::string replayConversationId;
    unsigned replayTurnIndex = 0;
    Phase replayLastPhase = Phase::None;
    std::map<std::string, size_t> replayToolMessageIndex;
    std::map<std::string, bool> replayGroupFailed;
    Json replayedMessages = Json::array();
};

inline std::vector<std::string> SplitLinesCopy(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        size_t len = nl - start;
        if (len > 0 && text[start + len - 1] == '\r') --len;
        lines.push_back(text.substr(start, len));
        start = nl + 1;
        if (start == text.size()) break;
    }
    return lines;
}

inline std::string FormatAcpDiffBlock(const std::string &path, const std::string &oldText, const std::string &newText) {
    const auto oldLines = SplitLinesCopy(oldText);
    const auto newLines = SplitLinesCopy(newText);
    size_t prefix = 0;
    while (prefix < oldLines.size() && prefix < newLines.size() && oldLines[prefix] == newLines[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix + prefix < oldLines.size() && suffix + prefix < newLines.size() &&
           oldLines[oldLines.size() - 1 - suffix] == newLines[newLines.size() - 1 - suffix])
        ++suffix;
    std::string diff;
    const size_t ctxStart = prefix > 2 ? prefix - 2 : 0;
    for (size_t i = ctxStart; i < prefix; ++i) diff += " " + oldLines[i] + "\n";
    for (size_t i = prefix; i + suffix < oldLines.size(); ++i) diff += "-" + oldLines[i] + "\n";
    for (size_t i = prefix; i + suffix < newLines.size(); ++i) diff += "+" + newLines[i] + "\n";
    const size_t ctxEnd = std::min(oldLines.size(), oldLines.size() - suffix + 2);
    for (size_t i = oldLines.size() - suffix; i < ctxEnd; ++i) diff += " " + oldLines[i] + "\n";
    TrimTrailingLineBreaks(diff);
    if (diff.empty()) diff = u8"(文件内容无差异)";
    TruncateUtf8Bytes(diff, 12288, u8"\n[diff 已截断]");
    std::string block = "\n\n";
    if (!path.empty()) block += u8"文件：`" + path + "`\n\n";
    block += "```diff\n" + diff + "\n```";
    return block;
}

inline void MergeAcpToolUpdate(AcpToolState &st, const Json &update) {
    const auto nextTitle = update.contains("title") && update["title"].is_string()
                               ? update["title"].get<std::string>()
                               : (update.contains("toolName") && update["toolName"].is_string()
                                      ? update["toolName"].get<std::string>()
                                      : std::string());
    if (!nextTitle.empty()) st.title = nextTitle;
    if (update.contains("kind") && update["kind"].is_string() && !update["kind"].get_ref<const std::string &>().empty())
        st.kind = update["kind"].get<std::string>();
    if (update.contains("status") && update["status"].is_string() &&
        !update["status"].get_ref<const std::string &>().empty())
        st.status = update["status"].get<std::string>();
    if (update.contains("locations") && update["locations"].is_array() && !update["locations"].empty())
        st.locations = update["locations"];
    if (update.contains("rawInput") && !update["rawInput"].is_null()) {
        if (!update["rawInput"].is_object() || !update["rawInput"].empty() || st.rawInput.is_null())
            st.rawInput = update["rawInput"];
    }
    if (update.contains("rawOutput") && !update["rawOutput"].is_null())
        st.rawOutput = update["rawOutput"];
    if (update.contains("content") && update["content"].is_array() && !update["content"].empty())
        st.content = update["content"];
}

inline std::string RenderAcpToolState(const std::string &toolId, const AcpToolState &st, bool &outCompleted,
                                      bool &outFailed, std::string &outTechnical) {
    const auto status = st.status.empty() ? std::string("in_progress") : st.status;
    outCompleted = status == "completed" || status == "failed" || status == "cancelled";
    outFailed = status == "failed" || (st.rawOutput.is_object() && st.rawOutput.value("isError", false));
    const auto title = st.title.empty() ? std::string(u8"工具调用") : st.title;
    std::string text = "**" + title + "** · " + TranslateItemStatus(status);

    if (st.locations.is_array() && !st.locations.empty()) {
        std::string locs;
        for (const auto &loc : st.locations) {
            if (!loc.is_object()) continue;
            const auto path = loc.value("path", std::string());
            if (path.empty() || title.find(path) != std::string::npos) continue;
            std::string entry = "`" + path;
            if (loc.contains("line") && loc["line"].is_number_integer())
                entry += ":" + std::to_string(loc["line"].get<int>());
            entry += "`";
            if (!locs.empty()) locs += " · ";
            locs += entry;
        }
        if (!locs.empty()) text += u8"\n\n位置：" + locs;
    }

    if (!st.rawInput.is_null()) {
        const auto &in = st.rawInput;
        if (in.is_string() && !in.get<std::string>().empty())
            text += "\n\n```text\n" + in.get<std::string>() + "\n```";
        else if (in.is_object() && in.contains("command") && in["command"].is_string())
            text += "\n\n```bash\n" + in["command"].get<std::string>() + "\n```";
        else if (in.is_object() && !in.empty())
            text += "\n\n```json\n" + in.dump(2) + "\n```";
    }

    std::string contentText;
    std::string extraSections;
    for (const auto &c : st.content) {
        if (c.is_string()) {
            contentText += c.get<std::string>();
            continue;
        }
        if (!c.is_object()) continue;
        const auto ctype = c.value("type", std::string());
        if (ctype == "diff") {
            extraSections += FormatAcpDiffBlock(c.value("path", std::string()), c.value("oldText", std::string()),
                                                c.value("newText", std::string()));
            continue;
        }
        if (ctype == "terminal") {
            const auto termId = c.value("terminalId", std::string());
            if (!termId.empty())
                extraSections += u8"\n\n**终端**\n\n`terminal:" + termId + "`";
            const auto tOut = c.value("output", c.value("text", std::string()));
            if (!tOut.empty())
                extraSections += FormatStructuredOrTextFence(u8"**输出**", tOut);
            continue;
        }
        if (c.contains("content") && c["content"].is_object()) {
            const auto &inner = c["content"];
            if (inner.contains("text") && inner["text"].is_string()) {
                contentText += inner["text"].get<std::string>();
            } else if (inner.value("type", std::string()) == "resource" && inner.contains("resource") &&
                       inner["resource"].is_object()) {
                const auto &res = inner["resource"];
                const auto uri = res.value("uri", std::string());
                const auto rtext = res.value("text", std::string());
                if (!uri.empty()) contentText += "[资源: " + uri + "]\n";
                contentText += rtext;
            }
        } else if (c.contains("text") && c["text"].is_string()) {
            contentText += c["text"].get<std::string>();
        }
    }
    TrimTrailingLineBreaks(contentText);

    std::string rawOutText = ExtractMcpResultText(st.rawOutput);
    TrimTrailingLineBreaks(rawOutText);

    std::string combinedOutput = contentText;
    if (!rawOutText.empty() && rawOutText != contentText) {
        if (!combinedOutput.empty()) combinedOutput += "\n";
        combinedOutput += rawOutText;
    }

    text += extraSections;
    if (!combinedOutput.empty())
        text += FormatStructuredOrTextFence(u8"**输出**", std::move(combinedOutput));

    Json tech = {{"toolCallId", toolId}, {"title", title}, {"status", status}};
    if (!st.kind.empty()) tech["kind"] = st.kind;
    if (st.locations.is_array() && !st.locations.empty()) tech["locations"] = st.locations;
    if (!st.rawInput.is_null()) tech["rawInput"] = st.rawInput;
    if (st.content.is_array() && !st.content.empty()) tech["content"] = st.content;
    if (!st.rawOutput.is_null()) tech["rawOutput"] = st.rawOutput;
    outTechnical = tech.dump(2);
    TruncateUtf8Bytes(outTechnical, 32768, u8"\n[技术详情已截断]");

    TruncateUtf8Bytes(text, 65536, u8"\n[工具输出已截断]");
    return text;
}

inline std::string FormatAcpPlanMarkdown(const Json &update, bool &outCompleted) {
    const auto entries = update.value("entries", Json::array());
    size_t doneCount = 0;
    std::string body;
    for (const auto &entry : entries) {
        if (!entry.is_object()) continue;
        const auto content = entry.value("content", std::string());
        if (content.empty()) continue;
        const auto st = entry.value("status", std::string("pending"));
        const bool isDone = st == "completed";
        if (isDone) ++doneCount;
        body += std::string(isDone ? "- [x] " : "- [ ] ") + content;
        if (st == "in_progress") body += u8" （进行中）";
        body += "\n";
    }
    TrimTrailingLineBreaks(body);
    outCompleted = !entries.empty() && doneCount == entries.size();
    std::string text = std::string(u8"**执行计划**") + (entries.empty() ? std::string()
        : " · " + std::to_string(doneCount) + "/" + std::to_string(entries.size()));
    if (!body.empty()) text += "\n\n" + body;
    return text;
}

inline void FinalizeAcpReplayTurn(AcpStreamState &state, const std::string &group) {
    if (group.empty() || !state.replayedMessages.is_array()) return;
    const bool failed = state.replayGroupFailed[group];
    size_t lastAi = SIZE_MAX;
    for (size_t i = 0; i < state.replayedMessages.size(); ++i) {
        auto &m = state.replayedMessages[i];
        if (m.value("group", std::string()) != group) continue;
        if (m.value("role", std::string()) == u8"我") {
            m["text"] = StripInjectedSessionContext(m.value("text", std::string()));
        } else if (m.value("role", std::string()) == "AI") {
            lastAi = i;
        }
        m["failedStep"] = failed;
    }
    if (lastAi != SIZE_MAX) {
        for (size_t i = 0; i < state.replayedMessages.size(); ++i) {
            auto &m = state.replayedMessages[i];
            if (m.value("group", std::string()) == group && m.value("role", std::string()) == "AI")
                m["process"] = (i != lastAi);
        }
    }
}

inline void FinalizeAcpReplay(AcpStreamState &state) {
    for (unsigned t = 1; t <= state.replayTurnIndex; ++t)
        FinalizeAcpReplayTurn(state, "acp-replay-turn-" + std::to_string(t));
}

inline std::vector<Json> DecodeAcpNotification(const Json &wire, AcpStreamState &state) {
    std::vector<Json> events;
    if (wire.contains("event")) {
        events.push_back(wire);
        return events;
    }
    if (wire.value("method", std::string()) != "session/update")
        return events;
    const auto params = wire.value("params", Json::object());
    const auto wireSid = params.value("sessionId", std::string());
    const auto update = params.value("update", Json::object());
    const auto kind = update.value("sessionUpdate", std::string());

    if (state.replayingHistory) {
        if (!wireSid.empty() && !state.replayConversationId.empty() && wireSid != state.replayConversationId)
            return events;
        auto ensureTurn = [&]() {
            if (state.replayTurnIndex == 0) state.replayTurnIndex = 1;
            return "acp-replay-turn-" + std::to_string(state.replayTurnIndex);
        };
        if (kind == "user_message_chunk") {
            const auto content = update.value("content", Json::object());
            const auto text = content.value("text", std::string());
            if (!text.empty()) {
                if (state.replayTurnIndex == 0 || state.replayLastPhase != AcpStreamState::Phase::User) {
                    if (state.replayTurnIndex > 0)
                        FinalizeAcpReplayTurn(state, "acp-replay-turn-" + std::to_string(state.replayTurnIndex));
                    ++state.replayTurnIndex;
                    state.replayLastPhase = AcpStreamState::Phase::User;
                    state.replayedMessages.push_back({{"role", u8"我"},
                                                      {"text", text},
                                                      {"messageId", "replay-user-" + std::to_string(state.replayTurnIndex)},
                                                      {"group", "acp-replay-turn-" + std::to_string(state.replayTurnIndex)},
                                                      {"status", "completed"},
                                                      {"failedStep", false}});
                } else {
                    auto &back = state.replayedMessages.back();
                    back["text"] = back.value("text", std::string()) + text;
                }
            }
        } else if (kind == "agent_thought_chunk") {
            const auto content = update.value("content", Json::object());
            const auto text = content.value("text", std::string());
            if (!text.empty()) {
                const auto group = ensureTurn();
                if (state.replayLastPhase != AcpStreamState::Phase::Thought || state.replayedMessages.empty() ||
                    state.replayedMessages.back().value("role", std::string()) != u8"思考" ||
                    state.replayedMessages.back().value("group", std::string()) != group) {
                    state.replayedMessages.push_back(
                        {{"role", u8"思考"},
                         {"text", text},
                         {"messageId", "thinking:replay-" + std::to_string(state.replayedMessages.size())},
                         {"process", true},
                         {"group", group},
                         {"status", "completed"},
                         {"failedStep", false}});
                } else {
                    auto &back = state.replayedMessages.back();
                    back["text"] = back.value("text", std::string()) + text;
                }
                state.replayLastPhase = AcpStreamState::Phase::Thought;
            }
        } else if (kind == "agent_message_chunk") {
            const auto content = update.value("content", Json::object());
            const auto text = content.value("text", std::string());
            if (!text.empty()) {
                const auto group = ensureTurn();
                if (state.replayLastPhase != AcpStreamState::Phase::Message || state.replayedMessages.empty() ||
                    state.replayedMessages.back().value("role", std::string()) != "AI" ||
                    state.replayedMessages.back().value("group", std::string()) != group) {
                    state.replayedMessages.push_back(
                        {{"role", "AI"},
                         {"text", text},
                         {"messageId", "replay-ai-" + std::to_string(state.replayedMessages.size())},
                         {"process", false},
                         {"group", group},
                         {"status", "completed"},
                         {"failedStep", false}});
                } else {
                    auto &back = state.replayedMessages.back();
                    back["text"] = back.value("text", std::string()) + text;
                }
                state.replayLastPhase = AcpStreamState::Phase::Message;
            }
        } else if (kind == "tool_call" || kind == "tool_call_update") {
            const auto group = ensureTurn();
            const auto toolId = update.value("toolCallId", update.value("id", std::string("acp-tool")));
            auto &st = state.tools[toolId];
            MergeAcpToolUpdate(st, update);
            bool completed = false, failed = false;
            std::string technical;
            auto rendered = RenderAcpToolState(toolId, st, completed, failed, technical);
            if (failed) state.replayGroupFailed[group] = true;
            const auto it = state.replayToolMessageIndex.find(toolId);
            if (it != state.replayToolMessageIndex.end() && it->second < state.replayedMessages.size()) {
                state.replayedMessages[it->second]["text"] = std::move(rendered);
                state.replayedMessages[it->second]["technical"] = std::move(technical);
            } else {
                state.replayToolMessageIndex[toolId] = state.replayedMessages.size();
                state.replayedMessages.push_back({{"role", u8"工具"},
                                                  {"text", std::move(rendered)},
                                                  {"technical", std::move(technical)},
                                                  {"messageId", "server:" + toolId},
                                                  {"process", true},
                                                  {"group", group},
                                                  {"status", "completed"},
                                                  {"failedStep", false}});
            }
            state.replayLastPhase = AcpStreamState::Phase::Tool;
        } else if (kind == "plan") {
            const auto group = ensureTurn();
            bool planCompleted = false;
            auto rendered = FormatAcpPlanMarkdown(update, planCompleted);
            const std::string planKey = "__plan__:" + group;
            const auto it = state.replayToolMessageIndex.find(planKey);
            auto tech = update.value("entries", Json::array()).dump(2);
            TruncateUtf8Bytes(tech, 32768, u8"\n[技术详情已截断]");
            if (it != state.replayToolMessageIndex.end() && it->second < state.replayedMessages.size()) {
                state.replayedMessages[it->second]["text"] = std::move(rendered);
                state.replayedMessages[it->second]["technical"] = std::move(tech);
            } else {
                state.replayToolMessageIndex[planKey] = state.replayedMessages.size();
                state.replayedMessages.push_back({{"role", u8"工具"},
                                                  {"text", std::move(rendered)},
                                                  {"technical", std::move(tech)},
                                                  {"messageId", "server:" + planKey},
                                                  {"process", true},
                                                  {"group", group},
                                                  {"status", "completed"},
                                                  {"failedStep", false}});
            }
            state.replayLastPhase = AcpStreamState::Phase::Tool;
        }
        return events;
    }

    if (!wireSid.empty() && !state.conversationId.empty() && wireSid != state.conversationId)
        return events;

    auto flushActiveMessageAsCommentary = [&]() {
        if (state.lastPhase == AcpStreamState::Phase::Message && state.textStarted && !state.currentMessageId.empty()) {
            events.push_back({{"event", "textComplete"},
                              {"conversationId", state.conversationId},
                              {"taskId", state.taskId},
                              {"messageId", state.currentMessageId},
                              {"text", state.accumulatedText},
                              {"final", false}});
            state.textStarted = false;
            state.currentMessageId.clear();
            state.accumulatedText.clear();
        }
    };

    if (kind == "agent_message_chunk") {
        const auto content = update.value("content", Json::object());
        const auto text = content.value("text", std::string());
        if (!text.empty()) {
            if (!state.textStarted || state.currentMessageId.empty()) {
                ++state.messageSeq;
                if (state.currentMessageId.empty()) {
                    state.currentMessageId =
                        state.messageSeq <= 1 && !state.taskId.empty()
                            ? ("acp-msg-" + state.taskId)
                            : ("acp-msg-" + state.taskId + "-" + std::to_string(state.messageSeq));
                }
                state.textStarted = true;
                events.push_back({{"event", "textStarted"},
                                  {"conversationId", state.conversationId},
                                  {"taskId", state.taskId},
                                  {"messageId", state.currentMessageId},
                                  {"final", true}});
            }
            state.lastPhase = AcpStreamState::Phase::Message;
            state.accumulatedText += text;
            events.push_back({{"event", "textDelta"},
                              {"conversationId", state.conversationId},
                              {"taskId", state.taskId},
                              {"messageId", state.currentMessageId},
                              {"text", text}});
        }
    } else if (kind == "agent_thought_chunk") {
        const auto content = update.value("content", Json::object());
        const auto text = content.value("text", std::string());
        if (!text.empty()) {
            flushActiveMessageAsCommentary();
            if (state.lastPhase != AcpStreamState::Phase::Thought || state.currentThoughtId.empty()) {
                ++state.thoughtSeq;
                state.currentThoughtId = "thinking:" + state.taskId +
                    (state.thoughtSeq > 1 ? (":" + std::to_string(state.thoughtSeq)) : std::string());
                state.lastPhase = AcpStreamState::Phase::Thought;
            }
            events.push_back({{"event", "thinkingDelta"},
                              {"conversationId", state.conversationId},
                              {"taskId", state.taskId},
                              {"messageId", state.currentThoughtId},
                              {"text", text}});
        } else {
            events.push_back({{"event", "taskActivity"}, {"conversationId", state.conversationId}, {"taskId", state.taskId}});
        }
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        flushActiveMessageAsCommentary();
        state.lastPhase = AcpStreamState::Phase::Tool;
        const auto toolId = update.value("toolCallId", update.value("id", std::string("acp-tool")));
        auto &st = state.tools[toolId];
        MergeAcpToolUpdate(st, update);
        bool completed = false, failed = false;
        std::string technical;
        auto text = RenderAcpToolState(toolId, st, completed, failed, technical);
        Json ev = {{"event", "serverTool"},
                   {"conversationId", state.conversationId},
                   {"taskId", state.taskId},
                   {"messageId", "server:" + toolId},
                   {"completed", completed},
                   {"failed", failed},
                   {"appendStream", false},
                   {"technical", std::move(technical)},
                   {"text", std::move(text)}};

        const std::string rawOutStr = ExtractMcpResultText(st.rawOutput);
        Json rawOutJson;
        if (!rawOutStr.empty() && rawOutStr.front() == '{') {
            auto parsed = Json::parse(rawOutStr, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object())
                rawOutJson = std::move(parsed);
        } else if (st.rawOutput.is_object()) {
            rawOutJson = st.rawOutput;
        }

        bool hasTerminalBlock = false;
        std::string terminalOutput;
        Json fileChanges = Json::array();
        for (const auto &c : st.content) {
            if (!c.is_object()) continue;
            const auto ctype = c.value("type", std::string());
            if (ctype == "terminal") {
                hasTerminalBlock = true;
                const auto tOut = c.value("output", c.value("text", std::string()));
                if (!tOut.empty()) terminalOutput += tOut;
            } else if (ctype == "diff") {
                const auto path = c.value("path", std::string());
                const auto oldText = c.value("oldText", std::string());
                const auto newText = c.value("newText", std::string());
                size_t added = 0, removed = 0;
                bool diffTrunc = false;
                auto unified = ComputeUnifiedDiff(path, oldText, newText, 48 * 1024, &added, &removed, &diffTrunc);
                std::string preview = newText;
                if (preview.size() > 256 * 1024) TruncateUtf8Safe(preview, 256 * 1024, u8"\n[文件内容预览已截断]");
                fileChanges.push_back({{"path", path},
                                       {"action", oldText.empty() ? "create" : "edit"},
                                       {"diff", std::move(unified)},
                                       {"contentPreview", std::move(preview)},
                                       {"addedLines", added},
                                       {"removedLines", removed},
                                       {"truncated", diffTrunc}});
            } else if (c.contains("content") && c["content"].is_object() &&
                       c["content"].contains("text") && c["content"]["text"].is_string()) {
                terminalOutput += c["content"]["text"].get<std::string>();
            } else if (c.contains("text") && c["text"].is_string()) {
                terminalOutput += c["text"].get<std::string>();
            }
        }

        const bool isWorkspaceExec = st.title.find("workspace.exec") != std::string::npos;
        const bool isExec = st.kind == "execute" || hasTerminalBlock || isWorkspaceExec ||
                            (st.rawInput.is_object() && st.rawInput.contains("command") && st.rawInput["command"].is_string());
        if (isExec) {
            std::string cmd = (st.rawInput.is_object() && st.rawInput.contains("command") && st.rawInput["command"].is_string())
                ? st.rawInput["command"].get<std::string>()
                : st.title;
            ev["execKind"] = (isWorkspaceExec || (rawOutJson.is_object() && rawOutJson.contains("execId"))) ? "workspace" : "acp";
            ev["execId"] = toolId;
            ev["command"] = std::move(cmd);
            if (rawOutJson.is_object()) {
                if (rawOutJson.contains("execId") && rawOutJson["execId"].is_string())
                    ev["wsExecId"] = rawOutJson["execId"];
                if (rawOutJson.contains("exitCode")) ev["exitCode"] = rawOutJson["exitCode"];
                if (rawOutJson.contains("durationMs")) ev["durationMs"] = rawOutJson["durationMs"];
                if (rawOutJson.contains("timedOut")) ev["timedOut"] = rawOutJson["timedOut"];
                if (rawOutJson.contains("cancelled")) ev["cancelled"] = rawOutJson["cancelled"];
                const auto so = rawOutJson.value("stdout", std::string());
                const auto se = rawOutJson.value("stderr", std::string());
                if (!so.empty() || !se.empty())
                    terminalOutput = so + se;
            }
            if (terminalOutput.empty() && !rawOutStr.empty() && !rawOutJson.is_object())
                terminalOutput = rawOutStr;
            if (!terminalOutput.empty())
                ev["execOutput"] = std::move(terminalOutput);
        }
        if (rawOutJson.is_object() && rawOutJson.value("ok", false) && rawOutJson.contains("changeId") &&
            rawOutJson["changeId"].is_string()) {
            ev["wsChangeId"] = rawOutJson["changeId"];
            if (fileChanges.empty() && rawOutJson.contains("path")) {
                fileChanges.push_back({{"changeId", rawOutJson["changeId"]},
                                       {"path", rawOutJson.value("path", std::string())},
                                       {"action", rawOutJson.value("action", std::string("edit"))},
                                       {"addedLines", rawOutJson.value("addedLines", 0)},
                                       {"removedLines", rawOutJson.value("removedLines", 0)}});
            }
        }
        if (!fileChanges.empty())
            ev["fileChanges"] = std::move(fileChanges);
        events.push_back(std::move(ev));
    } else if (kind == "plan") {
        flushActiveMessageAsCommentary();
        state.lastPhase = AcpStreamState::Phase::Tool;
        bool completed = false;
        auto text = FormatAcpPlanMarkdown(update, completed);
        auto tech = update.value("entries", Json::array()).dump(2);
        TruncateUtf8Bytes(tech, 32768, u8"\n[技术详情已截断]");
        events.push_back({{"event", "serverTool"},
                          {"conversationId", state.conversationId},
                          {"taskId", state.taskId},
                          {"messageId", "server:acp-plan-" + state.taskId},
                          {"completed", completed},
                          {"failed", false},
                          {"appendStream", false},
                          {"technical", std::move(tech)},
                          {"text", std::move(text)}});
    }
    return events;
}

inline std::vector<Json> DecodeAcpNotification(const Json &wire, const std::string &conversationId,
                                               const std::string &taskId, const std::string &messageId,
                                               std::string &accumulatedText, bool &textStarted) {
    AcpStreamState state;
    state.conversationId = conversationId;
    state.taskId = taskId;
    state.currentMessageId = messageId;
    state.accumulatedText = accumulatedText;
    state.textStarted = textStarted;
    auto events = DecodeAcpNotification(wire, state);
    accumulatedText = std::move(state.accumulatedText);
    textStarted = state.textStarted;
    return events;
}
} // namespace serialctl::ai
