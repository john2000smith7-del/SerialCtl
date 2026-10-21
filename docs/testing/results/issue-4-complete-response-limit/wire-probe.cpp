#include "ApiResponse.h"
#include <iostream>
int main() {
 using Json=nlohmann::json;
 const size_t limit=128*1024;
 for (const auto&id:{std::string("r"),std::string(128,'r'),std::string(128,'"')}) {
  for(int delta:{-1,0,1}) {
   Json result={{"events",{{{"source","串口设备"},{"data","AAAA"}}}},{"cursor",1},{"gap",false}};
   auto base=serialctl::MakeApiResponse(std::string(32,'a'),id,result,"events",limit).dump().size();
   result["events"][0]["source"]="串口设备"+std::string(limit+delta-base,'x');
   auto reply=serialctl::MakeApiResponse(std::string(32,'a'),id,result,"events",limit);
   std::cout<<Json{{"idBytes",id.size()},{"escaped",id.front()=='"'}, {"candidateBytes",limit+delta},{"reply",reply}}.dump()<<'\n';
  }
 }
}