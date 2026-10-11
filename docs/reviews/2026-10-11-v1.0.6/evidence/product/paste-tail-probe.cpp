#include "CmdLineEditor.h"
#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
using Bytes = std::vector<std::uint8_t>;
using serialctl::CmdLineEditor;
constexpr bool FALSE=false;
void InvalidateRect(int,void*,bool){}
struct Session { CmdLineEditor cmdEditor; };
struct PasteProbe {
 Session session; Session* activeSession_=&session; int selectedMode_=4,terminal_=0;
 std::vector<std::string> accepted; std::vector<std::wstring> errors;
 void ScrollTerminalToBottom(){} void ClearTerminalSelection(){}
 std::string EncodeTerminalText(const std::wstring& s){return std::string(s.begin(),s.end());}
 bool SendBytesToActive(const Bytes& b,bool){accepted.emplace_back(b.begin(),b.end());return true;}
 void AppendStatus(const std::wstring& s,bool){errors.push_back(s);}
 void Paste(const std::wstring& pasted){
    if (selectedMode_ == 4) {
        ScrollTerminalToBottom();
        auto &editor = activeSession_->cmdEditor;
        auto draft = editor.Text();
        draft.insert(editor.Cursor(), pasted);
        auto end = draft.find_last_of(L"\r\n");
        if (end == std::wstring::npos) {
            if (!editor.Insert(pasted))
                AppendStatus(L"CMD 输入过长", true);
        } else {
            auto submitted = draft.substr(0, end + 1);
            auto encoded = EncodeTerminalText(submitted);
            if (SendBytesToActive(Bytes(encoded.begin(), encoded.end()), false)) {
                editor.SubmittedBatch(submitted);
                if (!editor.Insert(draft.substr(end + 1)))
                    AppendStatus(L"CMD 尾行过长", true);
            }
        }
        ClearTerminalSelection();
        InvalidateRect(terminal_, nullptr, FALSE);
        return;
    }
 }
};
int main(){
 PasteProbe p;
 p.activeSession_->cmdEditor.Insert(L"echo PRESERVE ");
 const auto before=p.activeSession_->cmdEditor.Text();
 p.Paste(L"SAFE\r\n"+std::wstring(8192,L'x'));
 std::cout << "method=exact production CMD paste branch with transport acceptance and GUI calls stubbed\n";
 std::cout << "branch_source=src/app/MainWindow.cpp:4247-4268\n";
 std::cout << "accepted_batches="<<p.accepted.size()<<"\n";
 std::cout << "accepted_prefix="<<(p.accepted.empty()?"":p.accepted[0])<<"\n";
 std::cout << "draft_before_chars="<<before.size()<<"\n";
 std::cout << "draft_after_chars="<<p.activeSession_->cmdEditor.Text().size()<<"\n";
 std::cout << "tail_requested_chars=8192\nerrors="<<p.errors.size()<<"\n";
 bool reproduced=p.accepted.size()==1 && p.activeSession_->cmdEditor.Text().empty() && p.errors.size()==1;
 std::cout << "confirmed_partial_submission_and_tail_loss="<<reproduced<<"\n";
 return reproduced?0:1;
}
