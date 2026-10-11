#!/usr/bin/env python3
"""Compile unchanged portable production routines with controlled transport/scheduler seams.
Does not exercise Win32, hardware, application windows, or production lifecycle.
"""
from pathlib import Path
import hashlib, json, subprocess, sys
root=Path(__file__).resolve().parents[1]
src=root/'source'; out=root/'evidence/security'
def extract(path,start,end):
    text=(src/path).read_text(); return text[text.index(start):text.index(end,text.index(start))]
head='''#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using Json=nlohmann::json;
using Bytes=std::vector<std::uint8_t>;
'''
code=head+extract(Path('src/power/PowerService.cpp'),'void Require(', 'std::string Decimal(')+extract(Path('src/power/PowerService.cpp'),'Json ValidateChannels(', 'std::string Timestamp(')
code+='''struct PowerService {
std::mutex mutex_; std::condition_variable wake_;
std::deque<std::pair<std::string,Json>> queue_; std::map<std::string,Json> actions_;
bool stopping_=false; std::uint64_t next_=1;
Json State(){return {{"mode","independent"}};}
Json Submit(const Json&);
};
'''
code+=extract(Path('src/power/PowerService.cpp'),'Json PowerService::Submit(', 'void PowerService::Command(')
code+='''namespace ws { constexpr size_t MaxMessage=128*1024; }
Json Failure(const char *code,const std::string &detail){return {{"error",{{"code",code},{"message",detail}}}};}
struct ApiServer {
struct History {std::deque<Json> events; std::uint64_t seq=0; size_t bytes=0;};
std::map<std::string,History> histories_; std::mutex mutex_;
Json EventsLocked(const std::string&,std::uint64_t); Json Events(const std::string&,std::uint64_t);
};
'''
code+=extract(Path('src/network/ApiServer.cpp'),'Json ApiServer::EventsLocked(', '} // namespace serialctl')
code+='''int main(){
Json results=Json::object();
PowerService power;
for(int i=0;i<128;++i){auto r=power.Submit({{"type","output"},{"channels",{1}},{"enabled",true}}); if(r.contains("error")) return 1;}
results["power_queue_depth_before"]=power.queue_.size();
results["power_off_at_capacity"]=power.Submit({{"type","output"},{"channels",{1}},{"enabled",false}});
results["power_stop_at_capacity"]=power.Submit({{"type","stop"}});
results["power_disconnect_at_capacity"]=power.Submit({{"type","disconnect"}});
results["power_queue_depth_after"]=power.queue_.size();
ApiServer api; auto &h=api.histories_["session-3"];
// Same event schema as Publish(), fixed valid Base64 payloads, exact JSON byte accounting.
for(int i=0;i<8;++i){ ++h.seq; h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",std::string(21000,'A')},{"tick",1000}}); }
// Tune only the final event's Base64 payload; all message metadata remains production-shaped.
h.events.pop_back(); --h.seq;
while(api.EventsLocked("session-3",0).dump().size()>130800){h.events.pop_back();--h.seq;}
++h.seq; h.events.push_back({{"type","event"},{"version",1},{"instance",std::string(32,'a')},{"resource","session-3"},{"seq",h.seq},{"kind","output"},{"source","device"},{"data",""},{"tick",1000}});
size_t room=ws::MaxMessage-api.EventsLocked("session-3",0).dump().size(); room=(room/4)*4;
h.events.back()["data"]=std::string(room,'A');
auto result=api.Events("session-3",0);
Json reply={{"type","response"},{"version",1},{"instance",std::string(32,'a')},{"requestId",std::string(128,'r')},{"ok",true},{"result",result}};
results["events_result_bytes"]=result.dump().size(); results["events_wire_payload_bytes"]=reply.dump().size(); results["events_max_message"]=ws::MaxMessage; results["events_guard_returned_error"]=result.contains("error");
std::cout<<results.dump(2)<<"\\n";
return results["power_off_at_capacity"].contains("error") && !result.contains("error") && reply.dump().size()>ws::MaxMessage ? 0 : 2;
}
'''
path=out/'production_routines.cpp'; path.write_text(code)
manifest={'scope':'Unchanged extracted production routines; controlled non-consuming power worker and in-memory history. No Windows/hardware evidence.', 'source_sha':'5f12678e4da327f146edb5ce12eb3b3ea9c8afd0','extracted_sha256':hashlib.sha256(code.encode()).hexdigest()}
(out/'harness-manifest.json').write_text(json.dumps(manifest,indent=2))
binary=out/'production_routines'
subprocess.run(['g++','-std=c++17','-O2','-pthread','-I'+str(src/'third_party/json'),str(path),'-o',str(binary)],check=True)
p=subprocess.run([str(binary)],capture_output=True,text=True)
(out/'production-routines-result.json').write_text(p.stdout)
print(p.stdout); sys.exit(p.returncode)
