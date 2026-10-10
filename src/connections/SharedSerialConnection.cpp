#include "SharedSerialConnection.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <future>
namespace serialctl {
namespace {
struct Probe {
    unsigned port = 0;
    std::string instance;
    std::vector<std::wstring> names, descriptions;
};
Probe Query(const std::wstring &host, unsigned port, const std::atomic_bool *cancel = nullptr) {
    Probe p;
    try {
        GatewayClient c;
        c.Connect(host, port, cancel);
        p.port = port;
        p.instance = c.Hello()["instance"];
        auto resources = c.Request("resources");
        for (auto &r : resources["resources"])
            if (r.value("kind", "") == "serial" && r.value("connected", false)) {
                auto com = r.value("com", std::string());
                auto name = MultiByteToWide(reinterpret_cast<const std::uint8_t *>(com.data()), com.size(), CP_UTF8);
                p.names.push_back(name);
                p.descriptions.push_back(name + L" · " + std::to_wstring(r.value("baudRate", 115200)) +
                                         L" · WebSocket");
            }
    } catch (...) {
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
    auto p = Query(host, port);
    Result(p, names, descriptions);
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
                                          std::vector<std::wstring> *descriptions) {
    names.clear();
    if (descriptions)
        descriptions->clear();
    std::vector<std::future<Probe>> queries;
    for (unsigned p = explicitPort ? explicitPort : 7000; p <= (explicitPort ? explicitPort : 7015); ++p)
        queries.push_back(std::async(std::launch::async, [=] { return Query(host, p, cancel); }));
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
    Result(found[0], names, descriptions);
    if (names.empty()) {
        error = L"该实例没有已打开的本地串口";
        return false;
    }
    return true;
}
bool SharedSerialConnection::Start(DataCallback data, StatusCallback status, std::wstring &error) {
    Stop();
    cancel_ = false;
    stopping_ = false;
    data_ = std::move(data);
    status_ = std::move(status);
    try {
        if (!port_) {
            std::vector<std::wstring> names;
            if (!DiscoverAuto(host_, port_, names, error, &cancel_))
                return false;
        }
        client_.Connect(host_, port_, &cancel_, 5000);
        auto resources = client_.Request("resources");
        std::string com = ws::Lower(WideToMultiByte(serialName_, CP_UTF8));
        resource_.clear();
        for (auto &r : resources["resources"])
            if (r.value("kind", "") == "serial" && ws::Lower(r.value("com", "")) == com) {
                if (!resource_.empty())
                    throw std::runtime_error("AMBIGUOUS_COM");
                if (!r.value("connected", false))
                    throw std::runtime_error("TARGET_NOT_OPEN");
                resource_ = r["id"];
            }
        if (resource_.empty())
            throw std::runtime_error("TARGET_NOT_FOUND: COM must be opened locally first");
        // Register before returning: replay preserves output coalesced with handshake/subscribe.
        client_.Request("subscribe", resource_, {{"after", 0}});
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
    if (reader_.joinable())
        reader_.join();
    client_.Close();
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
            if (type == "event" && j.value("resource", "") == resource_) {
                if (j.value("kind", "") == "output" && data_)
                    data_(ws::Unbase64(j["data"]));
                else if (j.value("kind", "") == "error" && status_) {
                    auto b = ws::Unbase64(j["data"]);
                    status_(MultiByteToWide(b.data(), b.size(), CP_UTF8), true);
                }
            } else if (type == "resource_gone")
                throw std::runtime_error("TARGET_GONE");
            else if (type == "response" && !j.value("ok", false)) {
                auto msg = j["error"].dump();
                if (status_)
                    status_(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(msg.data()), msg.size(), CP_UTF8),
                            true);
            }
        }
    } catch (const std::exception &e) {
        if (!stopping_ && status_)
            status_(MultiByteToWide(reinterpret_cast<const std::uint8_t *>(e.what()), strlen(e.what()), CP_UTF8), true);
    }
    connected_ = false;
}
} // namespace serialctl
