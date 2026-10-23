#pragma once
#include "AiProtocol.h"

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
    CompactConversation
};
inline Json MakeRequest(Operation operation, std::uint64_t token, const Json &data) {
    Json params = Json::object();
    const char *method = "";
    const Json policy = {{"features.shell_tool", true}, {"features.multi_agent", false}, {"web_search", "disabled"}};
    switch (operation) {
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
            {"developerInstructions", u8"可使用 Codex 原生工具读取、修改服务器项目代码并执行服务器命令，权限限于 SSH 登录用户。Windows 内的终端和电源只通过 serialctl_ 工具访问；显示实际路径、命令、输出和文件修改结果，不混淆服务器与板端。"},
            {"sandbox", "danger-full-access"},
            {"approvalPolicy", "never"},
            {"dynamicTools", SerialTools()},
            {"config", policy}};
        break;
    case Operation::StartTask:
        method = "turn/start";
        params = {{"threadId", data.at("conversationId")},
                  {"input", Json::array({{{"type", "text"}, {"text", data.at("text")}}})}};
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
                  {"approvalPolicy", "never"},
                  {"baseInstructions", EngineerInstructions()},
                  {"developerInstructions", u8"服务器项目可用 Codex 原生文件及命令工具；终端和电源仍用已注册的 serialctl_ 工具。权限沿用 SSH 用户，不将容器命令误当成板端命令。"},
                  {"config", policy}};
        break;
    }
    if (operation == Operation::NewConversation || operation == Operation::RestoreConversation || operation == Operation::StartTask) {
        if (!data.value("cwd", std::string()).empty()) params["cwd"] = data["cwd"];
        // This value comes from config/read, never a user model selector or old registry preference.
        if (!data.value("defaultModel", std::string()).empty()) params["model"] = data["defaultModel"];
    }
    return {{"id", token}, {"method", method}, {"params", params}};
}
inline std::string NativeItemText(const Json& item, bool completed) {
    std::string text;
    if (item.value("type", "") == "commandExecution") {
        text = u8"服务器命令 · " + item.value("cwd", std::string()) + "\n```\n" + item.value("command", std::string()) + "\n```\n";
        text += completed ? u8"状态：" + item.value("status", std::string("unknown")) : u8"执行中\n";
        if (item.contains("exitCode") && item["exitCode"].is_number_integer()) text += u8" · 退出码 " + item["exitCode"].dump();
        if (item.contains("aggregatedOutput") && item["aggregatedOutput"].is_string()) text += "\n" + item["aggregatedOutput"].get<std::string>();
    } else {
        text = u8"服务器文件修改 · " + item.value("status", std::string("unknown"));
        for (const auto& change : item.value("changes", Json::array()))
            text += "\n" + change.value("path", std::string()) + "\n```\n" + change.value("diff", std::string()) + "\n```";
    }
    if (text.size() > 65536) {
        size_t end = 65536;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end); text += u8"\n[服务器工具输出已截断]";
    }
    return text;
}
inline Json DecodeResponse(Operation operation, const Json &result) {
    switch (operation) {
    case Operation::NewConversation:
        return {{"conversationId", result.at("thread").at("id")}};
    case Operation::StartTask:
        return {{"taskId", result.at("turn").at("id")}};
    case Operation::StopTask:
    case Operation::CompactConversation:
        return Json::object();
    case Operation::Defaults: {
        const auto config = result.value("config", Json::object());
        return {{"defaultModel", config.contains("model") && config["model"].is_string() ? config["model"] : Json("")}};
    }
    case Operation::Models: {
        Json models = Json::array();
        for (const auto &model : result.value("data", Json::array())) {
            auto id = model.value("model", model.value("id", std::string()));
            if (id.empty())
                continue;
            models.push_back({{"id", id},
                              {"label", model.value("displayName", id)},
                              {"reasoning", !model.value("supportedReasoningEfforts", Json::array()).empty()}});
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
                else if (type == "commandExecution" || type == "fileChange")
                    messages.push_back({{"role", u8"工具"}, {"text", NativeItemText(item, true)},
                        {"messageId", "server:" + item.value("id", std::string())}});
                else if (type == "userMessage") {
                    std::string text;
                    for (const auto &part : item.value("content", Json::array()))
                        if (part.value("type", "") == "text")
                            text += part.value("text", std::string());
                    messages.push_back(
                        {{"role", u8"我"}, {"text", text}, {"messageId", item.value("id", std::string())}});
                }
            }
            for (size_t i = first; i < messages.size(); ++i) {
                messages[i]["group"] = turn.value("id", std::string());
                messages[i]["status"] = turn.value("status", std::string());
                for (const auto &source : turn.value("items", Json::array()))
                    if (source.value("status", std::string()) == "failed" ||
                        (source.contains("exitCode") && source["exitCode"].is_number_integer() &&
                         source["exitCode"].get<int>() != 0))
                        messages[i]["status"] = "failed";
            }
        }
        return {{"conversationId", thread.at("id")}, {"messages", messages}};
    }
    }
    return Json::object();
}
inline Json DecodeNotification(const Json &wire) {
    if (wire.contains("event"))
        return wire; // transport state from CodexClient
    const auto method = wire.value("method", std::string());
    // Ignore unrelated notifications before inspecting their arbitrary params.
    // Presence/order of these notifications does not consume a request ID.
    if (!wire.contains("id") && method != "item/agentMessage/delta" && method != "item/completed" &&
        method != "item/started" && method != "item/reasoning/textDelta" &&
        method != "item/reasoning/summaryTextDelta" && method != "turn/started" && method != "turn/completed" &&
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
    if ((method == "item/started" || method == "item/completed") &&
        (p.value("item", Json::object()).value("type", "") == "commandExecution" ||
         p.value("item", Json::object()).value("type", "") == "fileChange")) {
        event["event"] = "serverTool";
        event["messageId"] = "server:" + p["item"].value("id", std::string());
        event["completed"] = method == "item/completed";
        event["failed"] = p["item"].value("status", std::string()) == "failed" ||
                          (p["item"].contains("exitCode") && p["item"]["exitCode"].is_number_integer() &&
                           p["item"]["exitCode"].get<int>() != 0);
        event["appendStream"] = p["item"].value("type", "") == "commandExecution" &&
            (!p["item"].contains("aggregatedOutput") || !p["item"]["aggregatedOutput"].is_string());
        event["text"] = NativeItemText(p["item"], method == "item/completed");
    } else if (method == "item/commandExecution/outputDelta") {
        event["event"] = "serverToolDelta";
        event["messageId"] = "server:" + p.value("itemId", std::string());
        event["text"] = p.value("delta", std::string());
    } else if (method == "item/agentMessage/delta") {
        event["event"] = "textDelta";
        event["messageId"] = p.value("itemId", std::string());
        event["text"] = p.value("delta", std::string());
    } else if (method == "item/completed" && p.value("item", Json::object()).value("type", "") == "agentMessage") {
        const auto &item = p["item"];
        event["event"] = "textComplete";
        event["messageId"] = item.value("id", std::string());
        event["text"] = item.value("text", std::string());
        event["final"] = !item.contains("phase") || item["phase"].is_null() || item["phase"] == "final_answer";
    } else if (method == "item/completed" && p.value("item", Json::object()).value("type", "") == "contextCompaction") {
        event["event"] = "contextCompacted";
    } else if (method == "item/started" || method == "item/reasoning/textDelta" ||
               method == "item/reasoning/summaryTextDelta") {
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
} // namespace serialctl::ai
