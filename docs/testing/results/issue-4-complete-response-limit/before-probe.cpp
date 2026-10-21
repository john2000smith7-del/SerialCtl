#include "json.hpp"
#include <deque>
#include <map>
#include <mutex>
#include <iostream>
using Json=nlohmann::json;
namespace ws { constexpr size_t MaxMessage=128*1024; }
Json Failure(const char*c,const std::string&m){return {{"error",{{"code",c},{"message",m}}}};}
struct ApiServer {
 struct History {std::deque<Json> events;std::uint64_t seq=0;size_t bytes=0;};
 std::map<std::string,History> histories_;std::mutex mutex_;
 Json EventsLocked(const std::string&,std::uint64_t);Json Events(const std::string&,std::uint64_t);
};
Json ApiServer::EventsLocked(const std::string &target, std::uint64_t after) {
    auto it = histories_.find(target);
    if (it == histories_.end())
        return {{"events", Json::array()}, {"cursor", 0}, {"gap", after != 0}};
    auto &h = it->second;
    bool gap =
        after > h.seq || (h.events.empty() ? after < h.seq : after + 1 < h.events.front()["seq"].get<std::uint64_t>());
    Json events = Json::array();
    if (!gap)
        for (auto &e : h.events)
            if (e["seq"].get<std::uint64_t>() > after)
                events.push_back(e);
    // History retrieval is bounded by the message limit; subscription streams individual frames.
    return {{"events", events}, {"cursor", h.seq}, {"gap", gap}};
}
Json ApiServer::Events(const std::string &target, std::uint64_t after) {
    std::lock_guard<std::mutex> l(mutex_);
    auto result = EventsLocked(target, after);
    if (result.dump().size() > ws::MaxMessage)
        return Failure("HISTORY_TOO_LARGE", "Use subscribe for bounded streamed history");
    return result;
}

int main(){
 ApiServer api;auto&h=api.histories_["session-3"];
 for(int i=0;i<8;++i){++h.seq;h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",std::string(21000,'A')},{"tick",1000}});}
 h.events.pop_back();--h.seq;
 while(api.EventsLocked("session-3",0).dump().size()>130800){h.events.pop_back();--h.seq;}
 ++h.seq;h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",""},{"tick",1000}});
 auto room=ws::MaxMessage-api.EventsLocked("session-3",0).dump().size();room=(room/4)*4;
 h.events.back()["data"]=std::string(room,'A');
 auto result=api.Events("session-3",0);
 Json reply={{"type","response"},{"version",1},{"instance",std::string(32,'a')},{"requestId",std::string(128,'r')},{"ok",true},{"result",result}};
 Json observed={{"result_bytes",result.dump().size()},{"wire_bytes",reply.dump().size()},{"limit",ws::MaxMessage},{"guard_error",result.contains("error")}};
 std::cout<<observed.dump(2)<<'\n';
 return !result.contains("error")&&reply.dump().size()>ws::MaxMessage?0:1;
}