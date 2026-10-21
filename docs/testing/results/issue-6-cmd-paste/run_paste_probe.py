"""Extract production paste branch, editor and CmdBatch; explicit UI/sender/encoding substitutes."""
from pathlib import Path
import subprocess, json, hashlib, os
out = Path(__file__).parent
repo = Path.cwd()
head = os.environ.get('PASTE_PROBE_REF')
def read(path, ref=None):
    return subprocess.check_output(['git','show',ref+':'+path], text=True) if ref else Path(path).read_text()
source = read('src/app/MainWindow.cpp',head)
service = read('src/network/SessionService.cpp',head)
editor = read('src/terminal/CmdLineEditor.h',head)
start = source.index('void MainWindow::PasteToTerminal()')
chunk = source[start:source.index('    std::wstring transmitted = pasted;',start)]
branch = chunk[chunk.index('    if (selectedMode_ == 4) {'):]
batch = service[service.index('Bytes SessionService::CmdBatch('):service.index('SessionService::Json SessionService::Input(')]
prefix = r'''
#include "Editor.h"
#include "json.hpp"
#include <iostream>
#include <codecvt>
#include <locale>
#include <stdexcept>
#include <cstdint>
using Bytes=std::vector<std::uint8_t>;
using serialctl::CmdLineEditor;
constexpr bool FALSE=false;
void InvalidateRect(int,void*,bool){}
struct SessionService { static Bytes CmdBatch(const Bytes&); };
BATCH
struct Session { CmdLineEditor cmdEditor; };
struct Probe {
 Session session;Session*activeSession_=&session;int selectedMode_=4,terminal_=0;
 std::vector<Bytes> accepted;unsigned attempted=0;bool reject=false,encodingReject=false;
 std::vector<std::wstring> errors;
 void ScrollTerminalToBottom(){} void ClearTerminalSelection(){}
 void AppendStatus(const std::wstring&s,bool){errors.push_back(s);}
 std::string EncodeTerminalText(const std::wstring&s){
  if(encodingReject){AppendStatus(L"controlled encoding rejection",true);return {};}
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(s);
 }
 bool SendBytesToActive(const Bytes&b,bool){
  ++attempted;
  if(reject){AppendStatus(L"controlled queue rejection",true);return false;}
  try {auto n=SessionService::CmdBatch(b);if(n.size()>65536)throw std::runtime_error("bound");accepted.push_back(n);return true;}
  catch(...){AppendStatus(L"controlled transport validation",true);return false;}
 }
 void Paste(const std::wstring&pasted){BRANCH}
};
'''
cases = r'''
std::string Utf8(const std::wstring&s){return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(s);}
int main(){using Json=nlohmann::json;Json cases=Json::array();unsigned failures=0;
auto record=[&](const std::string&name,Probe&p,bool pass){size_t bytes=0;for(auto&b:p.accepted)bytes+=b.size();cases.push_back({{"case",name},{"passed",pass},{"attempted",p.attempted},{"accepted_batches",p.accepted.size()},{"accepted_bytes",bytes},{"draft_units",p.session.cmdEditor.Text().size()},{"caret",p.session.cmdEditor.Cursor()},{"errors",p.errors.size()}});if(!pass)++failures;};
// Exact original report: valid prefix followed by an overlong incomplete tail.
Probe original;original.session.cmdEditor.Insert(L"echo PRESERVE ");original.Paste(L"SAFE\r\n"+std::wstring(8192,L'x'));
record("original_8192_tail_no_prefix",original,original.attempted==0&&original.accepted.empty()&&original.session.cmdEditor.Text()==L"echo PRESERVE "&&original.session.cmdEditor.Cursor()==14);
MATRIX
std::cout<<Json{{"cases",cases},{"failed",failures},{"checks",cases.size()}}.dump(2)<<'\n';return failures?1:0;}
'''
matrix = r'''
for(const std::wstring newline:{L"\r",L"\n",L"\r\n"})for(size_t caret:{size_t(0),size_t(2),size_t(4)})for(size_t length:{size_t(8190),size_t(8191),size_t(8192)}){
 Probe p;auto&e=p.session.cmdEditor;e.Insert(L"abcd");e.Edit(CmdLineEditor::Key::Home);for(size_t i=0;i<caret;++i)e.Edit(CmdLineEditor::Key::Right);
 auto suffix=std::wstring(L"abcd").substr(caret);auto tail=std::wstring(length-suffix.size(),L'x');auto line=std::wstring(L"abcd").substr(0,caret)+L"first";
 p.Paste(L"first"+newline+tail);bool valid=length<=8191;
 bool pass=valid?(p.attempted==1&&p.accepted.size()==1&&std::string(p.accepted[0].begin(),p.accepted[0].end())==Utf8(line)+"\r\n"&&e.Text()==tail+suffix&&e.Cursor()==tail.size()):(p.attempted==0&&p.accepted.empty()&&e.Text()==L"abcd"&&e.Cursor()==caret);
 record("tail_"+std::to_string(length)+"_newline_units_"+std::to_string(newline.size())+"_first_"+std::to_string(unsigned(newline[0]))+"_caret_"+std::to_string(caret),p,pass);
}
for(wchar_t control:{wchar_t(0),wchar_t(8),wchar_t(27),wchar_t(127)}){
 Probe p;auto&e=p.session.cmdEditor;e.Insert(L"keep");e.Edit(CmdLineEditor::Key::Left);p.Paste(L"safe\r\n"+std::wstring(1,control));record("forbidden_control_"+std::to_string(unsigned(control)),p,p.attempted==0&&e.Text()==L"keep"&&e.Cursor()==3);
}
for(size_t length:{size_t(8190),size_t(8191),size_t(8192)}){
 Probe p;p.Paste(std::wstring(length,L'x'));record("no_enter_"+std::to_string(length),p,p.attempted==0&&p.accepted.empty()&&p.session.cmdEditor.Text().size()==(length<=8191?length:0));
}
for(int mode:{0,1}){
 Probe p;auto&e=p.session.cmdEditor;e.Insert(L"keep");e.Edit(CmdLineEditor::Key::Left);p.encodingReject=mode==0;
 p.Paste(L"safe\n"+std::wstring(2731,L'中'));
 record(mode==0?"encoder_refuses_whole_plan":"utf8_tail_8193_bytes",p,p.attempted==0&&e.Text()==L"keep"&&e.Cursor()==3);
}
Probe rejected;auto&draft=rejected.session.cmdEditor;draft.SubmittedBatch(L"previous\r\n");draft.Insert(L"keep");draft.Edit(CmdLineEditor::Key::Left);rejected.reject=true;rejected.Paste(L"safe\r\ntail");
bool same=draft.Text()==L"keep"&&draft.Cursor()==3;draft.Edit(CmdLineEditor::Key::Up);bool history=draft.Text()==L"previous";draft.Edit(CmdLineEditor::Key::Down);
record("prefix_queue_rejection_keeps_draft_caret_history",rejected,rejected.attempted==1&&rejected.accepted.empty()&&same&&history&&draft.Text()==L"keep");
for(size_t last:{size_t(8183),size_t(8184),size_t(8191)}){
 Probe p;std::wstring pasted;for(int i=0;i<8;++i)pasted+=std::wstring(i==7?last:8191,L'x')+L"\n";p.Paste(pasted);
 size_t normalized=7*8193+last+2;bool valid=normalized==65536;
 record("normalized_batch_"+std::to_string(normalized),p,valid?(p.attempted==1&&p.accepted.size()==1&&p.accepted[0].size()==65536&&p.session.cmdEditor.Text().empty()):(p.attempted==0&&p.accepted.empty()&&p.session.cmdEditor.Text().empty()));
}
'''
summary={}
for label, ref in [('before','5f12678e4da327f146edb5ce12eb3b3ea9c8afd0'),('after',None),('mutation_no_encoded_preflight',None)]:
    root=out/label;root.mkdir(exist_ok=True)
    selected=branch
    if label=='before':
        old=read('src/app/MainWindow.cpp',ref);start=old.index('void MainWindow::PasteToTerminal()');part=old[start:old.index('    std::wstring transmitted = pasted;',start)];selected=part[part.index('    if (selectedMode_ == 4) {'):]
    if label=='mutation_no_encoded_preflight':
        a=selected.index('        const auto combined =');b=selected.index('        if (!plan.submitted.empty())',a);selected=selected[:a]+selected[b:]
    (root/'Editor.h').write_text(read('src/terminal/CmdLineEditor.h',ref) if ref else editor)
    code=prefix.replace('BATCH',batch).replace('BRANCH',selected)+cases.replace('MATRIX','' if label=='before' else matrix)
    (root/'probe.cpp').write_text(code)
    binary=root/'probe'
    subprocess.run(['g++','-std=c++17','-O2','-Ithird_party/json',str(root/'probe.cpp'),'-o',str(binary)],check=True)
    r=subprocess.run([str(binary)],capture_output=True,text=True);result=json.loads(r.stdout)
    (out/(label+'-result.json')).write_text(r.stdout)
    summary[label]={'failed':result['failed'],'checks':result['checks'],'exit':r.returncode}
    print(label,summary[label],flush=True)
    assert (r.returncode!=0)==(label!='after')
receipt={'source_sha':head or subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'scope':'Exact production CMD PasteToTerminal branch, CmdLineEditor and CmdBatch; UI, non-consuming sender and UTF8 encoder controlled substitutes. No Win32, clipboard, actual CMD or hardware proof locally.','results':summary,'source_hashes':{p:hashlib.sha256(v.encode()).hexdigest() for p,v in [('paste_branch',branch),('editor',editor),('CmdBatch',batch)]}}
(out/'receipt.json').write_text(json.dumps(receipt,ensure_ascii=False,indent=2)+'\n')
