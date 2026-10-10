#pragma once
#include "Connection.h"
#include "WebSocket.h"
#include "json.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
namespace serialctl {
using Json = nlohmann::json;
// One Winsock/RFC 6455 gateway. HTTP is used only for the Upgrade handshake.
class ApiServer {
public:
    using Handler=std::function<Json(const std::string& op,const std::string& resource,const Json& params)>;
    static constexpr size_t MaxClients=128, MaxQueueBytes=4*1024*1024, MaxHistoryBytes=1024*1024;
    ~ApiServer(){Stop();}
    bool Start(Handler,std::wstring& error,unsigned first=7000,unsigned last=7015);
    void Stop();
    void SetSessions(const std::vector<std::string>&);
    bool Running()const{return running_;}
    unsigned Port()const{return port_;}
    std::string Instance()const{return instance_;}
    void Publish(const std::string& resource,const Bytes&,const char* source="device",const char* kind="output");
    Json Events(const std::string&,std::uint64_t after);
    Json Diagnostics();
private:
    struct ClientState {
        SOCKET socket=INVALID_SOCKET;
        std::uint64_t id=0;
        std::string peer;
        std::thread reader,sender;
        std::atomic_bool finished{false}, slow{false};
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<ws::Stream::Message> queue;
        size_t queued=0;
        bool closing=false;
        std::set<std::string> subscriptions; // protected by server mutex
        std::map<std::string,std::pair<Json,Json>> responses; // reader only
        std::deque<std::string> responseOrder;
    };
    struct History {std::deque<Json> events;std::uint64_t seq=0;size_t bytes=0;};
    void Accept();
    void Client(const std::shared_ptr<ClientState>&);
    void Send(const std::shared_ptr<ClientState>&);
    void Request(const std::shared_ptr<ClientState>&,const Json&);
    void Queue(const std::shared_ptr<ClientState>&,unsigned,const Bytes&);
    void Queue(const std::shared_ptr<ClientState>&,const Json&);
    void Close(const std::shared_ptr<ClientState>&,unsigned,const std::string&);
    Json EventsLocked(const std::string&,std::uint64_t);
    void Trace(const std::shared_ptr<ClientState>&,const std::string& phase,const std::string& detail="");
    mutable std::mutex mutex_;
    std::set<std::string> sessions_;
    std::map<std::string,History> histories_;
    size_t historyBytes_=0;
    std::vector<std::shared_ptr<ClientState>> clients_;
    std::deque<Json> diagnostics_;
    std::uint64_t nextClient_=1;
    std::atomic_bool running_{false};
    std::atomic<SOCKET> listen_{INVALID_SOCKET};
    unsigned port_=0;
    std::string instance_;
    Handler handler_;
    std::thread accept_;
};
inline std::string Encode64(const Bytes& bytes){return ws::Base64(bytes);}
inline Bytes Decode64(const std::string& text){return ws::Unbase64(text);}
} // namespace serialctl
