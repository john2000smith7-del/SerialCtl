#pragma once
#include "CodexClient.h"
#include "CodexProtocol.h"
#include <chrono>
#include <map>

namespace serialctl::ai {
// Presentation adapter: widgets receive domain events rather than JSON-RPC.
// A protocol upgrade is confined to this layer and CodexProtocol.
class CodexSession {
  public:
    using Event = CodexClient::Event;
    ~CodexSession() {
        Shutdown();
    }
    bool Connect(ServerSettings settings, Event event, HostKeyConfirmation confirm, bool reverifyHostKey = false) {
        if (settings.provider == AiProvider::Antigravity && !settings.localMode && settings.remoteMcpPort == 0) {
            static std::atomic<unsigned> tunnelSeq{0};
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            const unsigned seed = static_cast<unsigned>(nowMs & 0x7FFFu) +
                                  tunnelSeq.fetch_add(137u, std::memory_order_relaxed);
            settings.remoteMcpPort = 28000u + (seed % 30000u);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            operations_.clear();
            acpHints_.clear();
            acp_ = {};
            acpLoadSession_ = false;
            acpSessionList_ = false;
            acpSessionResume_ = false;
            provider_ = settings.provider;
            localMode_ = settings.localMode;
            mcpPort_ = (!settings.localMode && settings.remoteMcpPort)
                           ? settings.remoteMcpPort
                           : (settings.mcpPort ? settings.mcpPort : 7080);
            emitEvent_ = event;
        }
        return client_.Connect(
            std::move(settings),
            [this, event = std::move(event)](Json wire) {
                try {
                    if (provider_ == AiProvider::Antigravity) {
                        if (wire.value("event", std::string()) == "ready") {
                            std::lock_guard<std::mutex> lock(mutex_);
                            acpLoadSession_ = wire.value("acpLoadSession", false);
                            acpSessionList_ = wire.value("acpSessionList", false);
                            acpSessionResume_ = wire.value("acpSessionResume", false);
                        }
                        if (wire.contains("id") && !wire.contains("method")) {
                            if (!wire["id"].is_number())
                                return;
                            const auto token = wire["id"].get<std::uint64_t>();
                            Operation operation;
                            AcpHint hint;
                            unsigned portSnapshot = 7080;
                            std::string convId, taskId, msgId, accumulated;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                const auto found = operations_.find(token);
                                if (found == operations_.end())
                                    return;
                                operation = found->second;
                                operations_.erase(found);
                                const auto h = acpHints_.find(token);
                                if (h != acpHints_.end()) {
                                    hint = std::move(h->second);
                                    acpHints_.erase(h);
                                }
                                portSnapshot = mcpPort_;
                                if (operation == Operation::StartTask) {
                                    convId = acp_.conversationId;
                                    taskId = acp_.taskId;
                                    msgId = acp_.currentMessageId;
                                    accumulated = std::move(acp_.accumulatedText);
                                    acp_.accumulatedText.clear();
                                    acp_.currentMessageId.clear();
                                    acp_.textStarted = false;
                                    acp_.lastPhase = AcpStreamState::Phase::None;
                                    acp_.activePromptToken = 0;
                                    acp_.tools.clear();
                                }
                            }
                            if (operation == Operation::StartTask) {
                                if (wire.contains("error")) {
                                    event({{"event", "taskFinished"},
                                           {"conversationId", convId},
                                           {"taskId", taskId},
                                           {"status", "failed"},
                                           {"message", wire["error"].value("message", std::string("Request rejected"))}});
                                } else {
                                    if (!accumulated.empty())
                                        event({{"event", "textComplete"},
                                               {"conversationId", convId},
                                               {"taskId", taskId},
                                               {"messageId", msgId},
                                               {"text", accumulated},
                                               {"final", true}});
                                    const auto stopReason =
                                        wire.value("result", Json::object()).value("stopReason", std::string("end_turn"));
                                    event({{"event", "taskFinished"},
                                           {"conversationId", convId},
                                           {"taskId", taskId},
                                           {"status", stopReason == "cancelled" ? "interrupted" : "completed"},
                                           {"message", std::string()}});
                                }
                                return;
                            }
                            if (wire.contains("error")) {
                                const int code = wire["error"].value("code", 0);
                                if (operation == Operation::RestoreConversation && hint.allowMethodFallback &&
                                    code == -32601) {
                                    Json retryData = hint.originalData;
                                    const bool nextLoad = !hint.usedLoadSession;
                                    retryData["loadSession"] = nextLoad;
                                    Json retryWire = MakeAcpRequest(Operation::RestoreConversation, token, retryData,
                                                                    portSnapshot);
                                    {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        operations_[token] = Operation::RestoreConversation;
                                        hint.usedLoadSession = nextLoad;
                                        hint.allowMethodFallback = false;
                                        acpHints_[token] = hint;
                                        acp_.replayingHistory = nextLoad;
                                        acp_.replayConversationId = hint.conversationId;
                                        acp_.replayTurnIndex = 0;
                                        acp_.replayLastPhase = AcpStreamState::Phase::None;
                                        acp_.replayToolMessageIndex.clear();
                                        acp_.replayGroupFailed.clear();
                                        acp_.replayedMessages = Json::array();
                                        acp_.tools.clear();
                                    }
                                    if (client_.Send(std::move(retryWire)))
                                        return;
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    operations_.erase(token);
                                    acpHints_.erase(token);
                                    acp_.replayingHistory = false;
                                } else if (operation == Operation::RestoreConversation) {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    acp_.replayingHistory = false;
                                    acp_.replayedMessages = Json::array();
                                    acp_.replayToolMessageIndex.clear();
                                    acp_.replayGroupFailed.clear();
                                    acp_.tools.clear();
                                }
                                event({{"event", "response"},
                                       {"token", token},
                                       {"error", wire["error"].value("message", std::string("Request rejected"))},
                                       {"code", code}});
                            } else {
                                try {
                                    Json replayed = Json::array();
                                    if (operation == Operation::RestoreConversation) {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        FinalizeAcpReplay(acp_);
                                        replayed = std::move(acp_.replayedMessages);
                                        acp_.replayingHistory = false;
                                        acp_.replayedMessages = Json::array();
                                        acp_.replayToolMessageIndex.clear();
                                        acp_.replayGroupFailed.clear();
                                        acp_.tools.clear();
                                    }
                                    auto decodedResult = DecodeAcpResponse(operation, wire.at("result"),
                                                                           hint.conversationId, hint.cwd, replayed);
                                    if (operation == Operation::NewConversation ||
                                        operation == Operation::RestoreConversation) {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        if (decodedResult.contains("conversationId") &&
                                            decodedResult["conversationId"].is_string())
                                            acp_.conversationId = decodedResult["conversationId"].get<std::string>();
                                        acp_.tools.clear();
                                    }
                                    event({{"event", "response"},
                                           {"token", token},
                                           {"result", std::move(decodedResult)}});
                                } catch (const std::exception &) {
                                    if (operation == Operation::RestoreConversation) {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        acp_.replayingHistory = false;
                                        acp_.replayedMessages = Json::array();
                                        acp_.replayToolMessageIndex.clear();
                                        acp_.replayGroupFailed.clear();
                                    }
                                    event({{"event", "response"},
                                           {"token", token},
                                           {"error", u8"服务器响应结构与客户端适配层不兼容"}});
                                }
                            }
                            return;
                        }
                        if (wire.contains("id")) {
                            client_.Send({{"jsonrpc", "2.0"},
                                          {"id", wire["id"]},
                                          {"error", {{"code", -32601}, {"message", "Unsupported ACP server request"}}}});
                            return;
                        }
                        std::vector<Json> decoded;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            decoded = DecodeAcpNotification(wire, acp_);
                        }
                        for (auto &ev : decoded)
                            event(std::move(ev));
                        return;
                    }
                    if (wire.contains("id") && !wire.contains("method")) {
                        if (!wire["id"].is_number())
                            return;
                        const auto token = wire["id"].get<std::uint64_t>();
                        Operation operation;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            const auto found = operations_.find(token);
                            if (found == operations_.end())
                                return;
                            operation = found->second;
                            operations_.erase(found);
                        }
                        if (wire.contains("error"))
                            event({{"event", "response"},
                                   {"token", token},
                                   {"error", wire["error"].value("message", std::string("Request rejected"))},
                                   {"code", wire["error"].value("code", 0)}});
                        else {
                            try {
                                event({{"event", "response"},
                                       {"token", token},
                                       {"result", DecodeResponse(operation, wire.at("result"))}});
                            } catch (const std::exception &) {
                                event({{"event", "response"},
                                       {"token", token},
                                       {"error", u8"服务器响应结构与客户端适配层不兼容"}});
                            }
                        }
                    } else {
                        auto decoded = DecodeNotification(wire);
                        if (!decoded.is_null())
                            event(std::move(decoded));
                    }
                } catch (const std::exception &) {
                    if (wire.contains("id"))
                        client_.Send({{"id", wire["id"]},
                                      {"error", {{"code", -32602}, {"message", "Unsupported request shape"}}}});
                    event({{"event", "error"}, {"message", u8"AI 事件格式不兼容；未执行该设备请求"}});
                }
            },
            std::move(confirm), reverifyHostKey);
    }
    bool Request(Operation operation, std::uint64_t token, Json data) {
        if (provider_ == AiProvider::Antigravity) {
            Event cb;
            unsigned port = 7080;
            bool local = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = emitEvent_;
                port = mcpPort_;
                local = localMode_;
            }
            if (operation == Operation::Defaults) {
                if (cb) cb({{"event", "response"}, {"token", token}, {"result", {{"defaultModel", ""}}}});
                return true;
            }
            if (operation == Operation::Models) {
                if (cb) cb({{"event", "response"}, {"token", token}, {"result", {{"models", Json::array()}, {"nextCursor", Json()}}}});
                return true;
            }
            if (operation == Operation::ServerHome) {
                if (cb) cb({{"event", "response"}, {"token", token}, {"result", {{"exitCode", local ? 1 : 0}, {"stdout", local ? "" : "/home/linux"}}}});
                return true;
            }
            if (operation == Operation::ReadDirectory || operation == Operation::DirectoryFallback ||
                operation == Operation::RunCommand || operation == Operation::CompactConversation) {
                if (cb) cb({{"event", "response"}, {"token", token}, {"result", {{"exitCode", 1}, {"stdout", ""}, {"entries", Json::array()}}}});
                return true;
            }
            if (operation == Operation::StopTask) {
                Json cancel = MakeAcpRequest(Operation::StopTask, token, data, port);
                client_.Send(std::move(cancel));
                if (cb) cb({{"event", "response"}, {"token", token}, {"result", Json::object()}});
                return true;
            }
            Json reqData = data;
            bool useLoadSession = false;
            bool allowFallback = false;
            if (operation == Operation::RestoreConversation) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reqData.contains("loadSession") && reqData["loadSession"].is_boolean()) {
                    useLoadSession = reqData["loadSession"].get<bool>();
                } else {
                    useLoadSession = acpLoadSession_;
                    allowFallback = true;
                }
                reqData["loadSession"] = useLoadSession;
            }
            Json message = MakeAcpRequest(operation, token, reqData, port);
            std::string taskId;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                operations_[token] = operation;
                if (operation == Operation::NewConversation || operation == Operation::RestoreConversation) {
                    AcpHint hint;
                    hint.conversationId = reqData.value("conversationId", std::string());
                    hint.cwd = reqData.value("cwd", std::string());
                    hint.usedLoadSession = useLoadSession;
                    hint.allowMethodFallback = allowFallback;
                    hint.originalData = reqData;
                    acpHints_[token] = std::move(hint);
                    if (operation == Operation::RestoreConversation) {
                        acp_.replayingHistory = useLoadSession;
                        acp_.replayConversationId = hint.conversationId;
                        acp_.replayTurnIndex = 0;
                        acp_.replayLastPhase = AcpStreamState::Phase::None;
                        acp_.replayToolMessageIndex.clear();
                        acp_.replayGroupFailed.clear();
                        acp_.replayedMessages = Json::array();
                        acp_.tools.clear();
                    }
                } else if (operation == Operation::StartTask) {
                    taskId = "acp-turn-" + std::to_string(token);
                    acp_.activePromptToken = token;
                    acp_.conversationId = reqData.value("conversationId", std::string());
                    acp_.taskId = taskId;
                    acp_.currentThoughtId.clear();
                    acp_.currentMessageId.clear();
                    acp_.accumulatedText.clear();
                    acp_.textStarted = false;
                    acp_.lastPhase = AcpStreamState::Phase::None;
                    acp_.thoughtSeq = 0;
                    acp_.messageSeq = 0;
                    acp_.replayingHistory = false;
                    acp_.tools.clear();
                }
            }
            if (!client_.Send(std::move(message))) {
                std::lock_guard<std::mutex> lock(mutex_);
                operations_.erase(token);
                acpHints_.erase(token);
                if (operation == Operation::RestoreConversation)
                    acp_.replayingHistory = false;
                return false;
            }
            if (operation == Operation::StartTask && cb)
                cb({{"event", "response"}, {"token", token}, {"result", {{"taskId", taskId}}}});
            return true;
        }
        if (operation == Operation::SetConfig) {
            Event cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = emitEvent_;
            }
            if (cb) cb({{"event", "response"}, {"token", token}, {"result", Json::object()}});
            return true;
        }
        // Build the message first: MakeRequest may throw, and a token must not stay
        // registered for a request that was never sent.
        Json message = MakeRequest(operation, token, data);
        std::lock_guard<std::mutex> lock(mutex_);
        operations_[token] = operation;
        if (client_.Send(std::move(message)))
            return true;
        operations_.erase(token);
        return false;
    }
    bool ReplyTool(const Json &requestId, const Json &result, bool success) {
        return client_.Send({{"id", requestId}, {"result", ToolResult(result, success)}});
    }
    void Reject(const Json &requestId, bool approval) {
        if (approval)
            client_.Send({{"id", requestId}, {"result", {{"decision", "decline"}}}});
        else
            client_.Send({{"id", requestId},
                          {"error", {{"code", -32601}, {"message", "Only bounded SerialCtl tools are supported"}}}});
    }
    void Approve(const Json &requestId) {
        client_.Send({{"id", requestId}, {"result", {{"decision", "accept"}}}});
    }
    void Disconnect() {
        client_.Disconnect();
    }
    void Shutdown() {
        client_.Shutdown();
    }
    bool Running() const {
        return client_.Running();
    }
    bool Ready() const {
        return client_.Ready();
    }
    bool Cancelled() const {
        return client_.Cancelled();
    }

  private:
    struct AcpHint {
        std::string conversationId, cwd;
        bool usedLoadSession = false;
        bool allowMethodFallback = false;
        Json originalData;
    };
    CodexClient client_;
    std::mutex mutex_;
    std::map<std::uint64_t, Operation> operations_;
    std::map<std::uint64_t, AcpHint> acpHints_;
    AcpStreamState acp_;
    bool acpLoadSession_ = false;
    bool acpSessionList_ = false;
    bool acpSessionResume_ = false;
    AiProvider provider_ = AiProvider::Codex;
    bool localMode_ = false;
    unsigned mcpPort_ = 7080;
    Event emitEvent_;
};
} // namespace serialctl::ai
