#include "SessionService.h"
#include "QueuedConnection.h"
#include "Win32Helpers.h"
#include <stdexcept>
namespace serialctl {
namespace {
nlohmann::json Error(const char *code, const std::string &message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}
} // namespace
void SessionService::Register(Json resource, std::shared_ptr<IConnection> transport) {
    auto e = std::make_shared<Entry>();
    e->resource = std::move(resource);
    e->transport = std::move(transport);
    std::lock_guard<std::mutex> l(mutex_);
    entries_[e->resource.at("id").get<std::string>()] = e;
}
std::shared_ptr<SessionService::Entry> SessionService::Find(const std::string &id) {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second;
}
void SessionService::Unregister(const std::string &id) {
    std::lock_guard<std::mutex> l(mutex_);
    entries_.erase(id);
}
std::vector<std::string> SessionService::Ids() {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<std::string> ids;
    for (auto &e : entries_)
        ids.push_back(e.first);
    return ids;
}
SessionService::Json SessionService::Resources() {
    std::lock_guard<std::mutex> l(mutex_);
    Json out = Json::array();
    for (auto &p : entries_) {
        auto r = p.second->resource;
        r["connected"] = p.second->transport->IsConnected();
        r["operations"] = {"session.get", "input", "subscribe", "unsubscribe", "events"};
        out.push_back(r);
    }
    return out;
}
SessionService::Json SessionService::State(const std::string &id) {
    auto e = Find(id);
    if (!e)
        return Error("TARGET_NOT_FOUND", "Session does not exist");
    auto r = e->resource;
    r["connected"] = e->transport->IsConnected();
    if (e->transport->InputCodePage()) {
        r["inputCodePage"] = e->transport->InputCodePage();
        r["outputCodePage"] = e->transport->OutputCodePage();
        r["codePageSource"] = e->transport->CodePageSource();
    }
    return r;
}
Bytes SessionService::CmdBatch(const Bytes &bytes) {
    if (bytes.empty() || (bytes.back() != 10 && bytes.back() != 13))
        throw std::runtime_error("CMD requires complete lines ending CR/LF; local editor submits on Enter");
    Bytes batch;
    size_t line = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto b = bytes[i];
        if (b == 13 || b == 10) {
            batch.push_back(13);
            batch.push_back(10);
            line = 0;
            if (b == 13 && i + 1 < bytes.size() && bytes[i + 1] == 10)
                ++i;
        } else {
            if ((b < 32 && b != 9) || b == 127)
                throw std::runtime_error("CMD pipe mode does not support control keys or full-screen input");
            if (++line > 8191)
                throw std::runtime_error("CMD line exceeds the Windows 8191-byte command limit");
            batch.push_back(b);
        }
    }
    return batch;
}
SessionService::Json SessionService::Input(const std::string &id, const Bytes &bytes, const std::string &source) {
    if (bytes.empty() || bytes.size() > 65536)
        return Error("INPUT_SIZE", "Input must contain 1..65536 bytes");
    auto e = Find(id);
    if (!e)
        return Error("TARGET_NOT_FOUND", "Session does not exist");
    Bytes batch;
    try {
        batch = e->resource.at("kind") == "cmd" ? CmdBatch(bytes) : bytes;
    } catch (const std::exception &ex) {
        return Error("CMD_SUBMIT_REQUIRED", ex.what());
    }
    std::lock_guard<std::mutex> l(e->input);
    std::wstring error;
    if (!e->transport->IsConnected())
        return Error("TARGET_NOT_OPEN", "Existing local session is disconnected");
    auto accepted = [&] {
        if (observer_)
            observer_(id, batch, source);
    };
    auto queued = dynamic_cast<QueuedConnection *>(e->transport.get());
    if (!(queued ? queued->SendObserved(batch, error, accepted) : e->transport->Send(batch, error)))
        return Error("INPUT_REJECTED", WideToMultiByte(error, CP_UTF8));
    if (!queued)
        accepted();
    return {{"accepted", batch.size()}, {"state", "queued"}, {"execution", "unknown"}};
}
} // namespace serialctl
