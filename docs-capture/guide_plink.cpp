// Documentation-only offline transport. Never shipped and never opens SSH.
#include "json.hpp"
#include <windows.h>
#include <iostream>
#include <regex>
#include <string>
using J=nlohmann::json;
void send(const J& j){std::cout<<j.dump()<<'\n'<<std::flush;}
std::string threadId="guide-thread-1",turnId="guide-turn-1",target,prompt;
int roundNumber=0;
void note(const std::string& text,const std::string& id,const char* phase){
 send({{"method","item/completed"},{"params",{{"threadId",threadId},{"turnId",turnId},{"item",{{"type","agentMessage"},{"id",id},{"text",text},{"phase",phase}}}}}});
}
void finish(){
 Sleep(8500);
 const std::string text=roundNumber==1 ? u8"日志停在 Waiting for root device /dev/mmcblk0p2...。\n\n目前可确认：内核正在等待根设备，尚未进入用户空间。仅凭这一行还不能确定具体原因。\n\n建议核对启动参数 root= 与实际分区名称，再检查存储控制器和设备树配置。" : u8"诊断命令已提交，新的终端输出已读取。\n\n请结合右侧终端的实际回显判断设备状态；发送成功不等于目标设备执行成功。";
 send({{"method","item/agentMessage/delta"},{"params",{{"threadId",threadId},{"turnId",turnId},{"itemId","answer-"+std::to_string(roundNumber)},{"delta",text}}}});
 note(text,"answer-"+std::to_string(roundNumber),"final_answer");
 send({{"method","turn/completed"},{"params",{{"threadId",threadId},{"turn",{{"id",turnId},{"status","completed"},{"error",nullptr}}}}}});
}
int wmain(int argc,wchar_t** argv){
 bool key=false,probe=false;std::wstring path;
 for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"-hostkey")key=true;if(a==L"exit")probe=true;if(a==L"-pwfile"&&i+1<argc)path=argv[++i];}
 if(!path.empty()){auto p=CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);if(p==INVALID_HANDLE_VALUE)return 41;char b=0;DWORD n;while(ReadFile(p,&b,1,&n,nullptr)&&n&&b!='\n'){}CloseHandle(p);SecureZeroMemory(&b,sizeof(b));}
 if(probe){if(key)return 0;std::cerr<<"The host key is not cached for this server\nkey fingerprint is:\nssh-ed25519 255 SHA256:serialctl-documentation-demo\n";return 1;}
 std::string line;
 while(std::getline(std::cin,line)){
  J m=J::parse(line);if(!m.contains("method")){if(m.value("id",J())==501){if(roundNumber>1){send({{"id",502},{"method","item/tool/call"},{"params",{{"threadId",threadId},{"turnId",turnId},{"callId","guide-read-after-"+std::to_string(roundNumber)},{"tool","serialctl_read"},{"arguments",{{"sessionId",target},{"lines",30}}}}}});}else finish();}else if(m.value("id",J())==502)finish();continue;}
  const std::string method=m.value("method","");const J id=m.value("id",J());
  if(method=="initialize")send({{"id",id},{"result",{{"userAgent","documentation-demo"}}}});
  else if(method=="config/read")send({{"id",id},{"result",{{"config",{{"model","server-default"}}}}}});
  else if(method=="model/list")send({{"id",id},{"result",{{"data",J::array()},{"nextCursor",nullptr}}}});
  else if(method=="thread/start")send({{"id",id},{"result",{{"thread",{{"id",threadId}}}}}});
  else if(method=="turn/start"){
   ++roundNumber;turnId="guide-turn-"+std::to_string(roundNumber);prompt=m["params"]["input"][0].value("text","");std::smatch match;
   if(std::regex_search(prompt,match,std::regex("associated terminal session: ([^;]+);")))target=match[1];
   send({{"id",id},{"result",{{"turn",{{"id",turnId}}}}}});
   send({{"method","turn/started"},{"params",{{"threadId",threadId},{"turn",{{"id",turnId}}}}}});
   note(u8"正在读取关联终端的最近输出，并检查日志中的停顿位置。","progress-"+std::to_string(roundNumber),"commentary");
   send({{"id",501},{"method","item/tool/call"},{"params",{{"threadId",threadId},{"turnId",turnId},{"callId","guide-tool-"+std::to_string(roundNumber)},{"tool",roundNumber==1?"serialctl_read":"serialctl_command"},{"arguments",roundNumber==1?J{{"sessionId",target},{"lines",100}}:J{{"sessionId",target},{"command","uname -a"},{"reason",u8"根据用户要求查询系统版本"}}}}}});
  }else if(method=="thread/resume"){
   J u={{"type","userMessage"},{"id","history-user"},{"content",J::array({{{"type","text"},{"text",u8"帮我分析当前开发板的启动日志。"}}})}};
   J a={{"type","agentMessage"},{"id","history-answer"},{"phase","final_answer"},{"text",u8"当前日志停在等待根设备的位置，请核对启动参数与实际分区。"}};
   J t={{"id",turnId},{"status","completed"},{"items",J::array({u,a})}};
   J history={{"id",threadId},{"turns",J::array({t})}};
   send({{"id",id},{"result",{{"thread",history}}}});
  }
  else if(method=="thread/compact/start"){send({{"id",id},{"result",J::object()}});send({{"method","thread/compacted"},{"params",{{"threadId",threadId}}}});}
  else if(method=="turn/interrupt"){send({{"id",id},{"result",J::object()}});send({{"method","turn/completed"},{"params",{{"threadId",threadId},{"turn",{{"id",turnId},{"status","interrupted"}}}}}});}
 }
}
