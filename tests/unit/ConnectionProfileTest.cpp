#include "PlatformConnection.h"
#include "SshConnectionHistory.h"
#include <stdexcept>
#include <iostream>
using namespace serialctl;
void Check(bool ok) { if(!ok)throw std::runtime_error("Connection profile regression"); }
int main() {
 unsigned port=0;
 Check(PlatformSshPort(L"2866",port)&&port==12866);
 Check(PlatformSshPort(L"28266",port)&&port==38266);
 Check(PlatformSshPort(L"0001",port)&&port==10001);
 Check(PlatformSshPort(L"55535",port)&&port==65535);
 for(const auto invalid:{L"",L"0",L"55536",L"-1",L"1x",L"999999999999"})Check(!PlatformSshPort(invalid,port));
 auto row=nlohmann::json{{"host","host"},{"username","linux"},{"port",12866u},{"employee","2866"},{"password","must-not-persist"},{"lastSuccess",123u}};
 auto history=RememberSshConnection(nlohmann::json::array(),row);
 Check(history.size()==1 && !history[0].contains("password") && history[0]["lastSuccess"]==123);
 row["host"]="HOST";history=RememberSshConnection(history,row);Check(history.size()==1);
 row["username"]="Linux";history=RememberSshConnection(history,row);Check(history.size()==2);
 row["employee"]="28266";Check(NormalizeSshHistory(nlohmann::json::array({row})).empty());
 row["employee"]=true;Check(NormalizeSshHistory(nlohmann::json::array({row})).empty());
 std::cout<<"Platform ranges, conversion, history identity and secret exclusion passed\n";
}
