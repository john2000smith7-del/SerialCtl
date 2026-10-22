#pragma once
#include "CodexClient.h"
#include "CodexProtocol.h"
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
    bool Connect(ServerSettings settings, Event event, HostKeyConfirmation confirm) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            operations_.clear();
        }
        return client_.Connect(
            std::move(settings),
            [this, event = std::move(event)](Json wire) {
                try {
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
                                   {"error", wire["error"].value("message", std::string("Request rejected"))}});
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
                    event({{"event", "error"}, {"message", u8"Codex 事件格式不兼容；未执行该设备请求"}});
                }
            },
            std::move(confirm));
    }
    bool Request(Operation operation, std::uint64_t token, Json data) {
        std::lock_guard<std::mutex> lock(mutex_);
        operations_[token] = operation;
        if (client_.Send(MakeRequest(operation, token, data)))
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
    CodexClient client_;
    std::mutex mutex_;
    std::map<std::uint64_t, Operation> operations_;
};
} // namespace serialctl::ai
