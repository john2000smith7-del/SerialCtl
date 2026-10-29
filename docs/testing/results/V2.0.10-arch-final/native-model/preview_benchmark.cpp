#include "TerminalModel.h"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <unistd.h>
size_t Rss() { std::ifstream f("/proc/self/statm"); size_t size=0, resident=0; f>>size>>resident;return resident*sysconf(_SC_PAGESIZE); }
int main() {
 nlohmann::json result;result["scope"]="Linux native C++ model only; not Win32 UI or serial hardware";result["wcharBytes"]=sizeof(wchar_t);
 for(int lines:{0,2000,20000}) {
 serialctl::TerminalModel m(80,24); std::wstring text;
 for(int i=0;i<lines;++i) text+=std::wstring(60,L'x')+L"\r\n";
 m.Feed(text+L"> ",L"T");m.SetLineWrapView(false);
 for(size_t i=0;i<m.DisplayLineCount();++i)m.DisplayLine(i);
 std::vector<double> times;size_t rss=0;auto before=Rss();
 for(int i=0;i<30;++i) {auto start=std::chrono::steady_clock::now();auto p=m.PreviewInput(L"中文long draft",4);times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());rss=std::max(rss,Rss());if(p.PlainText(false).find(L"> 中文long draft")==std::wstring::npos)return 1;}
 std::sort(times.begin(),times.end());result["cases"].push_back({{"historyInputLines",lines},{"retainedRawHistoryRows",m.HistorySize()},{"iterations",30},{"p50Ms",times[15]},{"p95Ms",times[28]},{"maxMs",times.back()},{"rssBeforeBytes",before},{"maxRssWithPreviewBytes",rss},{"samplesMs",times}});
 }std::cout<<result.dump(2)<<'\n';}
