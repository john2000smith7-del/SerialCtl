#include "TerminalModel.h"
#include <iostream>
int main() {
    using Model = serialctl::TerminalModel;
    Model owner(80,24), first(100,30), second(120,40);
    first.SetResponseRole(Model::ResponseRole::Viewer);
    second.SetResponseRole(Model::ResponseRole::Viewer);
    const std::wstring query=L"\x1b[5n\x1b[6n\x1b[c\x1b[18t";
    auto a=owner.Feed(query,L"").response;
    auto b=first.Feed(query,L"").response;
    auto c=second.Feed(query,L"").response;
    first.Resize(132,50); first.Clear(); first.Reset();
    auto d=first.Feed(std::wstring(L"\x1b")+L"c"+query,L"").response;
    std::cout << "owner_reply_bytes=" << a.size() << "\nfirst_viewer_reply_bytes=" << b.size()
              << "\nsecond_viewer_reply_bytes=" << c.size() << "\nresized_reset_RIS_replay_reply_bytes=" << d.size() << '\n';
    if(a!="\x1b[0n\x1b[1;1R\x1b[?1;2c\x1b[8;24;80t"||!b.empty()||!c.empty()||!d.empty()) return 1;
    std::cout << "PASS: production models, owner-only replies; not a GUI, socket or hardware run.\n";
}
