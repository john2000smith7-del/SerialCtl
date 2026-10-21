from pathlib import Path
import subprocess,json,os
out=Path(__file__).parent
prefix=r'''#include "json.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <codecvt>
#include <locale>
using Bytes=std::vector<std::uint8_t>;
using UINT=unsigned;
constexpr unsigned CP_UTF8=65001,CommandTimerId=4002,StatusTimerId=4003;
constexpr bool FALSE=false;
bool timer=false,timerFail=false;
void InvalidateRect(void*,void*,bool){}
void KillTimer(void*,unsigned id){if(id==CommandTimerId)timer=false;}
unsigned SetTimer(void*,unsigned id,unsigned,void*){if(timerFail)return 0;if(id==CommandTimerId)timer=true;return id;}
std::string WideToMultiByte(const std::wstring&s,unsigned){return std::string(s.begin(),s.end());}
struct Connection {bool connected=true;bool IsConnected()const{return connected;}};
struct Session {std::uint64_t id=1;};
struct CommandItem {std::wstring name=L"probe";std::vector<std::wstring> commands{L"slow",L"done"};unsigned intervalMs=100;};
struct MainWindow {
 enum class CommandSequenceEnd {Silent,Submitted,UserStopped,Interrupted};
 std::vector<CommandItem> commands_{CommandItem{}};
 Connection connectionValue;Connection*connection_=&connectionValue;
 Session session;Session*activeSession_=&session;
 int runningCommandIndex_=-1,selectedMode_=4;
 size_t runningCommandStep_=0,submittedCommandSteps_=0;
 std::uint64_t runningCommandSessionId_=0;
 UINT runningCommandCodePage_=CP_UTF8;
 std::wstring runningCommandLineEnding_,statusText_;
 void*window_=nullptr,*commandList_=nullptr;
 bool localEchoEnabled_=false,reject=false;
 std::vector<Bytes> accepted;
 bool statusError=false;
 std::wstring SelectedLineEnding(){return L"\r\n";}
 UINT SelectedCodePage(){return CP_UTF8;}
 std::string EncodeTerminalText(const std::wstring&s){return WideToMultiByte(s,CP_UTF8);}
 void UpdateCommandActions(){}
 void AppendStatus(const std::wstring&s,bool error){statusText_=s;statusError=error;}
 bool SendBytesToActive(const Bytes&b,bool,const std::wstring&){if(reject){AppendStatus(L"queue full",true);return false;}accepted.push_back(b);return true;}
 void SendCommand(size_t);void SendNextCommandStep();
'''
scenarios=r'''
std::string Utf8(const std::wstring&s){return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(s);}
int main(){using Json=nlohmann::json;Json cases=Json::array();unsigned failed=0;
auto record=[&](const char*name,MainWindow&m,bool pass){cases.push_back({{"case",name},{"status",Utf8(m.statusText_)},{"accepted",m.accepted.size()},{"active",m.runningCommandIndex_>=0},{"error",m.statusError},{"passed",pass}});if(!pass)++failed;};
MainWindow complete;complete.SendCommand(0);complete.SendCommand(0);complete.SendNextCommandStep();
record("submitted_not_executed",complete,complete.accepted.size()==2&&complete.runningCommandIndex_==-1&&complete.statusText_.find(L"已提交 2/2")!=std::wstring::npos&&complete.statusText_.find(L"执行完成")==std::wstring::npos);
MainWindow stopped;stopped.SendCommand(0);STOP_USER;stopped.SendNextCommandStep();
record("stop_keeps_accepted_prevents_future",stopped,stopped.accepted.size()==1&&!timer&&stopped.statusText_.find(L"已停止后续提交（1/2）")!=std::wstring::npos&&stopped.statusText_.find(L"已提交内容可能继续执行")!=std::wstring::npos);
MainWindow disconnected;disconnected.SendCommand(0);disconnected.connectionValue.connected=false;disconnected.SendNextCommandStep();disconnected.SendNextCommandStep();
record("disconnected_no_resend",disconnected,disconnected.accepted.size()==1&&disconnected.runningCommandIndex_==-1&&disconnected.statusError&&disconnected.statusText_.find(L"执行状态未知")!=std::wstring::npos);
MainWindow rejected;rejected.SendCommand(0);rejected.reject=true;rejected.SendNextCommandStep();
record("rejected_count_and_reason_retained",rejected,rejected.accepted.size()==1&&rejected.statusError&&rejected.statusText_.find(L"1/2")!=std::wstring::npos&&rejected.statusText_.find(L"queue full")!=std::wstring::npos);
MainWindow badTimer;timerFail=true;badTimer.SendCommand(0);timerFail=false;
record("timer_failure_after_acceptance",badTimer,badTimer.accepted.size()==1&&badTimer.statusError&&badTimer.statusText_.find(L"1/2")!=std::wstring::npos&&badTimer.statusText_.find(L"计时器创建失败")!=std::wstring::npos);
MainWindow single;single.commands_[0].commands={L"one"};single.SendCommand(0);
record("single_command_also_only_submitted",single,single.accepted.size()==1&&single.statusText_.find(L"已提交命令")!=std::wstring::npos);
std::cout<<Json{{"cases",cases},{"failed",failed},{"checks",cases.size()},{"executed_by_model",0}}.dump(2)<<'\n';return failed?1:0;}
'''
for label,ref in [('before','119ad60'),('after',os.environ.get('MACRO_PROBE_AFTER_REF'))]:
 source=subprocess.check_output(['git','show',ref+':src/app/MainWindow.cpp'],text=True) if ref else Path('src/app/MainWindow.cpp').read_text()
 methods=source[source.index('void MainWindow::SendCommand('):source.index('void MainWindow::PostData(')]
 decl='void StopCommandSequence(bool);};\n' if label=='before' else 'void StopCommandSequence(CommandSequenceEnd,const std::wstring& detail={});};\n'
 stop='stopped.StopCommandSequence(true)' if label=='before' else 'stopped.StopCommandSequence(MainWindow::CommandSequenceEnd::UserStopped)'
 code=prefix+decl+methods+scenarios.replace('STOP_USER',stop)
 path=out/(label+'-probe.cpp');path.write_text(code)
 binary=out/(label+'-probe')
 subprocess.run(['g++','-std=c++17','-O2','-Ithird_party/json',str(path),'-o',str(binary)],check=True)
 r=subprocess.run([str(binary)],stdout=subprocess.PIPE,text=True)
 (out/(label+'-result.json')).write_text(r.stdout)
 result=json.loads(r.stdout)
 print(label,{'failed':result['failed'],'checks':result['checks'],'exit':r.returncode})
 assert r.returncode==(1 if label=='before' else 0)
