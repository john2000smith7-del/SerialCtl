#pragma once
#include "AiProtocol.h"

namespace serialctl::ai {
// UI-facing operations. Only this adapter knows the App Server method names,
// parameter casing, experimental fields and persisted thread item shapes.
enum class Operation { Models, Defaults, NewConversation, StartTask, StopTask, RestoreConversation };
inline Json MakeRequest(Operation operation, std::uint64_t token, const Json &data) {
    Json params = Json::object();
    const char *method = "";
    const Json policy = {{"features.shell_tool", false}, {"features.multi_agent", false}, {"web_search", "disabled"}};
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
            {"developerInstructions", u8"直接读写本次选定终端；用户明确要求时控制 GUI 勾选的已连接电源输出，无需二次弹窗。只使用 serialctl_ 工具，依据真实输出和 action 回读判断结果。"},
            {"sandbox", "read-only"},
            {"approvalPolicy", "never"},
            {"dynamicTools", SerialTools()},
            {"config", policy}};
        if (!data.value("modelId", std::string()).empty())
            params["model"] = data["modelId"];
        break;
    case Operation::StartTask:
        method = "turn/start";
        params = {{"threadId", data.at("conversationId")},
                  {"input", Json::array({{{"type", "text"}, {"text", data.at("text")}}})}};
        if (!data.value("modelId", std::string()).empty())
            params["model"] = data["modelId"];
        break;
    case Operation::StopTask:
        method = "turn/interrupt";
        params = {{"threadId", data.at("conversationId")}, {"turnId", data.at("taskId")}};
        break;
    case Operation::RestoreConversation:
        method = "thread/resume";
        params = {{"threadId", data.at("conversationId")},
                  {"sandbox", "read-only"},
                  {"approvalPolicy", "never"},
                  {"baseInstructions", EngineerInstructions()},
                  {"developerInstructions", u8"直接读写本次选定终端；用户明确要求时控制 GUI 勾选的已连接电源输出，无需二次弹窗。只使用已注册的 serialctl_ 工具；历史缺少电源工具时请新建会话。"},
                  {"config", policy}};
        break;
    }
    return {{"id", token}, {"method", method}, {"params", params}};
}
inline Json DecodeResponse(Operation operation, const Json &result) {
    switch (operation) {
    case Operation::NewConversation:
        return {{"conversationId", result.at("thread").at("id")}};
    case Operation::StartTask:
        return {{"taskId", result.at("turn").at("id")}};
    case Operation::StopTask:
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
        for (const auto &turn : thread.value("turns", Json::array()))
            for (const auto &item : turn.value("items", Json::array())) {
                const auto type = item.value("type", std::string());
                if (type == "agentMessage")
                    messages.push_back({{"role", "AI"},
                                        {"text", item.value("text", std::string())},
                                        {"messageId", item.value("id", std::string())}});
                else if (type == "userMessage") {
                    std::string text;
                    for (const auto &part : item.value("content", Json::array()))
                        if (part.value("type", "") == "text")
                            text += part.value("text", std::string());
                    messages.push_back(
                        {{"role", u8"我"}, {"text", text}, {"messageId", item.value("id", std::string())}});
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
        method != "error")
        return Json();
    const auto p = wire.value("params", Json::object());
    if (wire.contains("id")) {
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
    if (method == "item/agentMessage/delta") {
        event["event"] = "textDelta";
        event["messageId"] = p.value("itemId", std::string());
        event["text"] = p.value("delta", std::string());
    } else if (method == "item/completed" && p.value("item", Json::object()).value("type", "") == "agentMessage") {
        const auto &item = p["item"];
        event["event"] = "textComplete";
        event["messageId"] = item.value("id", std::string());
        event["text"] = item.value("text", std::string());
        event["final"] = !item.contains("phase") || item["phase"].is_null() || item["phase"] == "final_answer";
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
