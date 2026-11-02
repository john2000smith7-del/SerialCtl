#include "SharedSerialConnection.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <future>
namespace serialctl {
namespace {
struct Probe {
    unsigned port = 0;
    std::string instance;
    std::string error;
    std::vector<std::wstring> names, descriptions;
};
constexpr ULONGLONG DiscoveryBudgetMs = 1500;
Probe Query(const std::wstring &host, unsigned port, const TcpAddresses &addresses,
            ULONGLONG deadline, const std::atomic_bool *cancel = nullptr) {
    Probe p;
    try {
        GatewayClient c;
        c.ConnectUntil(host, port, addresses, cancel, deadline);
        p.port = port;
        p.instance = c.Hello()["instance"];
        auto resources = c.Request("resources");
        for (auto &r : resources["resources"])
            if (r.value("kind", "") == "serial" && r.value("connected", false)) {
                auto com = r.value("com", std::string());
                auto name = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(com.data()), com.size(), CP_UTF8);
                p.names.push_back(name);
                auto friendly = r.value("friendlyName", std::string());
                auto full =
                    MultiByteToWide(reinterpret_cast<const std::uint8_t *>(friendly.data()), friendly.size(), CP_UTF8);
                p.descriptions.push_back(name + (full.empty() ? L"" : L" · " + full) + L" · " +
                                         std::to_wstring(r.value("baudRate", 115200)) + L" · WebSocket");
            }
    } catch (const ws::Error &e) {
        if (!p.instance.empty() || !e.instance.empty()) {
            p.port = port;
            if (!e.instance.empty())
                p.instance = e.instance;
            p.error = e.what();
        }
    } catch (const std::exception &e) {
        if (!p.instance.empty())
            p.error = e.what();
    }
    return p;
}
void Result(const Probe &p, std::vector<std::wstring> &names, std::vector<std::wstring> *descriptions) {
    names = p.names;
    if (descriptions)
        *descriptions = p.descriptions;
}
} // namespace
bool SharedSerialConnection::Discover(const std::wstring &host, std::uint16_t port, std::vector<std::wstring> &names,
                                      std::wstring &error, std::vector<std::wstring> *descriptions) {
    const auto deadline = GetTickCount64() + DiscoveryBudgetMs;
    TcpAddresses addresses;
    if (!ResolveTcpAddresses(host, deadline, addresses, error)) return false;
    auto p = Query(host, port, addresses, deadline);
    Result(p, names, descriptions);
    if (!p.error.empty()) {
        error = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(p.error.data()), p.error.size(), CP_UTF8);
        return false;
    }
    if (p.instance.empty()) {
        error = L"对端要求升级为 SerialCtl WebSocket serialctl.v1，或服务不可达";
        return false;
    }
    if (names.empty()) {
        error = L"该实例没有已打开的本地串口";
        return false;
    }
    return true;
}
bool SharedSerialConnection::DiscoverAuto(const std::wstring &host, std::uint16_t &port,
                                          std::vector<std::wstring> &names, std::wstring &error,
                                          const std::atomic_bool *cancel, std::uint16_t explicitPort,
                                          std::vector<std::wstring> *descriptions, std::string *instance) {
    names.clear();
    if (instance)
        instance->clear();
    if (descriptions)
        descriptions->clear();
    const auto deadline = GetTickCount64() + DiscoveryBudgetMs;
    TcpAddresses addresses;
    if (!ResolveTcpAddresses(host, deadline, addresses, error, cancel)) return false;
    std::vector<std::future<Probe>> queries;
    for (unsigned p = explicitPort ? explicitPort : 7000; p <= (explicitPort ? explicitPort : 7015); ++p)
        queries.push_back(std::async(std::launch::async, [&, p] { return Query(host, p, addresses, deadline, cancel); }));
    std::vector<Probe> found;
    for (auto &q : queries) {
        auto p = q.get();
        if (!p.instance.empty())
            found.push_back(std::move(p));
    }
    if (cancel && *cancel) {
        error = L"查询已取消";
        return false;
    }
    if (found.size() != 1) {
        error = found.empty() ? L"未发现 SerialCtl WebSocket 服务（7000–7015）"
                              : L"发现多个 SerialCtl 实例，请明确指定端口：";
        for (auto &p : found)
            error += L" " + std::to_wstring(p.port);
        return false;
    }
    port = static_cast<std::uint16_t>(found[0].port);
    if (!found[0].error.empty()) {
        const auto &reason = found[0].error;
        error = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(reason.data()), reason.size(), CP_UTF8);
        return false;
    }
    if (instance)
        *instance = found[0].instance;
    Result(found[0], names, descriptions);
    if (names.empty()) {
        error = L"该实例没有已打开的本地串口";
        return false;
    }
    return true;
}
bool SharedSerialConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    Stop();
    // A cancellation arriving just before Start must not be cleared and turn
    // into a new connection after the user has already disconnected it.
    if (cancel_.exchange(false)) { error = L"连接已取消。"; return false; }
    stopping_ = false;
    data_ = std::move(data);
    status_ = std::move(status);
    try {
        const auto deadline = GetTickCount64() + 5000;
        if (!port_) {
            std::vector<std::wstring> names;
            if (!DiscoverAuto(host_, port_, names, error, &cancel_, 0, nullptr, &expectedInstance_))
                return false;
        }
        client_.ConnectUntil(host_, port_, &cancel_, deadline);
        if (!expectedInstance_.empty() && client_.Hello()["instance"] != expectedInstance_)
            throw std::runtime_error("INSTANCE_MISMATCH: discover again; server restarted or port reused");
        auto resources = client_.Request("resources");
        std::string com = ws::Lower(WideToMultiByte(serialName_, CP_UTF8));
        resource_.clear();
        transferSupported_=false;
        for (auto &r : resources["resources"])
            if (r.value("kind", "") == "serial" && ws::Lower(r.value("com", "")) == com) {
                if (!resource_.empty())
                    throw std::runtime_error("AMBIGUOUS_COM");
                if (!r.value("connected", false))
                    throw std::runtime_error("TARGET_NOT_OPEN");
                resource_ = r["id"];
                transferSupported_=r.value("xfer",false);
            }
        if (resource_.empty())
            throw std::runtime_error("TARGET_NOT_FOUND: COM must be opened locally first");
        // Register before returning: replay preserves output coalesced with handshake/subscribe.
        try {
            client_.Request("subscribe", resource_, {{"after", 0}});
        } catch (const std::runtime_error &e) {
            auto failure = nlohmann::json::parse(e.what(), nullptr, false);
            if (failure.is_discarded() || failure.value("code", "") != "HISTORY_GAP")
                throw;
            auto history = client_.Request("events", resource_, {{"after", 0}});
            // This is a fresh desktop selection, not a resumed task. Explicitly
            // report unavailable history, then atomically subscribe at this cursor.
            if (status_)
                status_(L"HISTORY_GAP：旧输出已超出缓存，本次从当前游标接收；不保证旧历史完整", false);
            client_.Request("subscribe", resource_, {{"after", history.at("cursor")}});
        }
        client_.Live();
        connected_ = true;
        reader_ = std::thread(&SharedSerialConnection::Read, this);
        return true;
    } catch (const std::exception &e) {
        error = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(e.what()), strlen(e.what()), CP_UTF8);
        client_.Close();
        return false;
    }
}
void SharedSerialConnection::Stop() {
    stopping_ = true;
    connected_ = false;
    client_.Cancel();
    rpcWake_.notify_all();
    if (reader_.joinable())
        reader_.join();
    client_.Close();
    {std::lock_guard<std::mutex> lock(rpcMutex_);pendingRpc_.clear();rpcReplies_.clear();transferToken_.clear();}
    data_ = {};
    status_ = {};
}
bool SharedSerialConnection::Send(const Bytes &data, std::wstring &error) {
    if (!connected_) {
        error = L"远程会话已断开";
        return false;
    }
    if (data.empty() || data.size() > 65536) {
        error = L"单次输入必须为 1–65536 字节";
        return false;
    }
    try {
        client_.SendRequest("input", resource_, {{"data", ws::Base64(data)}});
        return true;
    } catch (...) {
        error = L"网络发送失败，执行状态未知；请勿自动重发";
        return false;
    }
}
void SharedSerialConnection::Read() {
    try {
        while (!stopping_) {
            auto j = client_.Receive();
            auto type = j.value("type", "");
            if(type=="xfer_data"&&j.value("resource","")==resource_) {
                bool accepted=false;
                {std::lock_guard<std::mutex> lock(rpcMutex_);accepted=!transferToken_.empty()&&j.value("token",std::string())==transferToken_;}
                if(accepted&&data_)data_(ws::Unbase64(j.at("data")));
            } else if(type=="response") {
                bool transferReply=false;
                {std::lock_guard<std::mutex> lock(rpcMutex_);const auto id=j.value("requestId",std::string());
                    if(pendingRpc_.count(id)){rpcReplies_[id]=j;transferReply=true;rpcWake_.notify_all();}}
                if(!transferReply&&!j.value("ok",false)&&status_) {
                    const auto msg=j["error"].dump();status_(MultiByteToWide(reinterpret_cast<const std::uint8_t*>(msg.data()),msg.size(),CP_UTF8),true);
                }
            } else if (type == "event" && j.value("resource", "") == resource_) {
                if (j.value("kind", "") == "output" && data_)
                    data_(ws::Unbase64(j["data"]));
                else if (j.value("kind", "") == "error" && status_) {
                    auto b = ws::Unbase64(j["data"]);
                    status_(MultiByteToWide(b.data(), b.size(), CP_UTF8), true);
                }
            } else if (type == "resource_gone")
                throw std::runtime_error("TARGET_GONE");
        }
    } catch (const std::exception &e) {
        if (!stopping_ && status_)
            status_(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(e.what()), strlen(e.what()), CP_UTF8), true);
    }
    connected_ = false;
    rpcWake_.notify_all();
}
bool SharedSerialConnection::TransferRequest(const std::string& op,nlohmann::json params,std::wstring& error,nlohmann::json* result) {
    std::unique_lock<std::mutex> lock(rpcMutex_);
    if(!connected_||stopping_) {error=L"远程串口已断开";return false;}
    std::string id;
    try {
        id=client_.SendRequest(op,resource_,std::move(params));
        pendingRpc_.insert(id);
        if(!rpcWake_.wait_for(lock,std::chrono::seconds(5),[&]{return rpcReplies_.count(id)||!connected_||stopping_;})) {
            pendingRpc_.erase(id);lock.unlock();client_.Cancel();connected_=false;rpcWake_.notify_all();
            error=L"远端传输确认超时，已断开以释放所有权；已写出的字节不可撤回";return false;
        }
        pendingRpc_.erase(id);
        const auto it=rpcReplies_.find(id);
        if(it==rpcReplies_.end()) {error=L"远程串口断开，传输结果未知";return false;}
        auto reply=std::move(it->second);rpcReplies_.erase(it);
        if(!reply.value("ok",false)) {
            const auto detail=reply.value("error",nlohmann::json::object()).dump();
            error=MultiByteToWide(reinterpret_cast<const std::uint8_t*>(detail.data()),detail.size(),CP_UTF8);return false;
        }
        if(result)*result=reply.at("result");
        return true;
    }catch(const std::exception& e) {
        pendingRpc_.erase(id);
        error=MultiByteToWide(reinterpret_cast<const std::uint8_t*>(e.what()),strlen(e.what()),CP_UTF8);return false;
    }
}
bool SharedSerialConnection::BeginTransferWire(const std::string& owner,std::wstring& error) {
    if(!transferSupported_) {error=L"远端 SerialCtl 不支持 Xfer，请升级远端；原有串口收发仍可用";return false;}
    {std::lock_guard<std::mutex> lock(rpcMutex_);if(!transferToken_.empty()){error=L"已有远端传输";return false;}transferToken_=owner;}
    if(TransferRequest("transfer.begin",{{"token",owner}},error))return true;
    {std::lock_guard<std::mutex> lock(rpcMutex_);transferToken_.clear();}
    return false;
}
bool SharedSerialConnection::SendTransferWire(const std::string& owner,const Bytes& bytes,std::wstring& error) {
    {std::lock_guard<std::mutex> lock(rpcMutex_);if(transferToken_!=owner||owner.empty()){error=L"远端传输所有权已变化";return false;}}
    nlohmann::json result;
    if(!TransferRequest("transfer.write",{{"token",owner},{"data",ws::Base64(bytes)}},error,&result))return false;
    if(!result.contains("written")||!result["written"].is_number_unsigned()||result["written"].get<std::uint64_t>()!=bytes.size()) {
        error=L"远端未确认完整写入，结果未知";client_.Cancel();connected_=false;rpcWake_.notify_all();return false;
    }
    return true;
}
bool SharedSerialConnection::EndTransferWire(const std::string& owner,std::wstring& error) {
    const bool released=TransferRequest("transfer.end",{{"token",owner}},error);
    {std::lock_guard<std::mutex> lock(rpcMutex_);if(transferToken_==owner)transferToken_.clear();}
    if(!released){client_.Cancel();connected_=false;rpcWake_.notify_all();}
    return released;
}

} // namespace serialctl
