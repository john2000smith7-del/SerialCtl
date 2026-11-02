#include "XferPane.h"
#include "UiMetrics.h"
#include <commdlg.h>
#include <limits>
namespace serialctl {
namespace {
std::wstring Text(HWND h){const int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
bool Label(HWND h,const std::wstring& value){if(Text(h)==value)return false;SetWindowTextW(h,value.c_str());return true;}
int Measure(HWND h,HFONT font,const std::wstring& text,int width){
    HDC dc=GetDC(h);auto old=SelectObject(dc,font);RECT r{0,0,std::max(1,width),0};
    DrawTextW(dc,text.c_str(),-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
    SelectObject(dc,old);ReleaseDC(h,dc);return r.bottom;
}
}
XferPane::~XferPane(){if(window_)DestroyWindow(window_);if(surface_)DeleteObject(surface_);if(field_)DeleteObject(field_);}
HWND XferPane::Control(int id)const{return GetDlgItem(id==Start||id==Cancel?window_:body_,id);}
bool XferPane::Create(HWND parent,xfer::TransferController& controller,std::function<Target()> target,HFONT font,bool dark,int dpi) {
    controller_=&controller;target_=std::move(target);font_=font;dark_=dark;dpi_=dpi;style_={&dark_,font_,dpi_,false};
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"SerialCtlXferPane";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"串口传输文件",WS_CHILD|WS_CLIPCHILDREN,0,0,100,100,parent,nullptr,wc.hInstance,this);
    if(!window_)return false;
    wc.lpfnWndProc=BodyProc;wc.lpszClassName=L"SerialCtlXferBody";RegisterClassW(&wc);
    body_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"传输选项",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,100,100,window_,nullptr,wc.hInstance,this);
    if(!body_)return false;
    const auto add=[&](int id,const wchar_t* cls,const wchar_t* text,DWORD extra){
        auto h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|extra,0,0,1,1,id==Start||id==Cancel?window_:body_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        if(_wcsicmp(cls,L"BUTTON")==0)UiStyleButton(h);
        if(_wcsicmp(cls,L"EDIT")==0||_wcsicmp(cls,L"COMBOBOX")==0)UiStyleField(h,&style_);
        if(extra&WS_TABSTOP)SetWindowSubclass(h,FocusProc,219,reinterpret_cast<DWORD_PTR>(this));
        return h;
    };
    add(Source,L"STATIC",L"未选择串口",SS_ENDELLIPSIS);
    add(DirectionLabel,L"STATIC",L"方向",0);add(ModeLabel,L"STATIC",L"协议",0);
    for(int id:{Direction,Mode})add(id,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP);
    for(const auto* s:{L"发送文件",L"接收文件"})SendMessageW(Control(Direction),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    for(const auto* s:{L"原始二进制",L"XMODEM",L"YMODEM",L"ZMODEM"})SendMessageW(Control(Mode),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    SendMessageW(Control(Direction),CB_SETCURSEL,0,0);SendMessageW(Control(Mode),CB_SETCURSEL,2,0);
    add(FileLabel,L"STATIC",L"发送文件",0);add(File,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
    UiSetButtonGlyph(add(Browse,L"BUTTON",L"选择文件",BS_OWNERDRAW|WS_TABSTOP),UiGlyph::Folder,L"选择文件");
    add(LengthLabel,L"STATIC",L"接收长度（字节，0 表示未知）",0);add(Length,L"EDIT",L"0",ES_AUTOHSCROLL|ES_NUMBER|WS_TABSTOP);
    add(Help,L"STATIC",L"",SS_EDITCONTROL);add(Status,L"STATIC",L"",SS_EDITCONTROL);
    add(Count,L"STATIC",L"",SS_ENDELLIPSIS);
    add(Start,L"BUTTON",L"开始发送",BS_OWNERDRAW|WS_TABSTOP);
    add(Cancel,L"BUTTON",L"取消传输",BS_OWNERDRAW|WS_TABSTOP);
    Theme(dark,dpi,font);Refresh();return true;
}
void XferPane::Theme(bool dark,int dpi,HFONT font) {
    dark_=dark;dpi_=dpi;font_=font;style_={&dark_,font_,dpi_,false};
    if(surface_)DeleteObject(surface_);if(field_)DeleteObject(field_);
    const auto c=UiTheme(dark_);surface_=CreateSolidBrush(c.surface);field_=CreateSolidBrush(c.field);
    if(!window_)return;
    for(int id=Direction;id<=LengthLabel;++id) {
        auto h=Control(id);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        wchar_t cls[24]{};GetClassNameW(h,cls,24);
        if(_wcsicmp(cls,L"EDIT")==0||_wcsicmp(cls,L"COMBOBOX")==0)UiStyleField(h,&style_);
    }
    Layout();RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
void XferPane::Layout() {
    if(!body_)return;
    RECT client{};GetClientRect(window_,&client);
    const int width=std::max(1L,client.right),footer=D(48);
    bodyHeight_=std::max(1,static_cast<int>(client.bottom)-footer);
    MoveWindow(body_,0,0,width,bodyHeight_,TRUE);
    const int lane=D(16),contentWidth=std::max(D(80),width-lane);
    const int half=std::max(1,(contentWidth-D(12))/2);
    const int helpHeight=std::max(D(40),Measure(body_,font_,Text(Control(Help)),contentWidth));
    const int statusHeight=std::max(D(40),Measure(body_,font_,Text(Control(Status)),contentWidth));
    progressTop_=D(232)+helpHeight+D(16);
    const int countTop=progressTop_+D(16),statusTop=countTop+D(32);
    contentHeight_=statusTop+statusHeight+D(12);
    offset_=std::clamp(offset_,0,std::max(0,contentHeight_-bodyHeight_));
    const auto move=[&](int id,int x,int y,int w,int h){MoveWindow(Control(id),x,y-offset_,std::max(1,w),h,TRUE);};
    move(Source,0,0,contentWidth,D(28));
    move(DirectionLabel,0,D(40),half,D(20));move(ModeLabel,half+D(12),D(40),half,D(20));
    move(Direction,0,D(64),half,D(180));move(Mode,half+D(12),D(64),half,D(200));
    {UiLayoutBatch b;b.ComboVisibleHeight(Control(Direction),D(28));b.ComboVisibleHeight(Control(Mode),D(28));b.Commit();}
    move(FileLabel,0,D(104),contentWidth,D(20));move(File,0,D(128),contentWidth-D(40),D(28));move(Browse,contentWidth-D(28),D(128),D(28),D(28));
    move(LengthLabel,0,D(168),contentWidth,D(20));move(Length,0,D(192),contentWidth,D(28));
    move(Help,0,D(232),contentWidth,helpHeight);
    move(Count,0,countTop,contentWidth,D(24));move(Status,0,statusTop,contentWidth,statusHeight);
    const int buttonWidth=std::max(1,(width-D(12))/2),top=std::max(0,static_cast<int>(client.bottom)-D(36));
    MoveWindow(Control(Start),0,top,buttonWidth,D(36),TRUE);MoveWindow(Control(Cancel),buttonWidth+D(12),top,buttonWidth,D(36),TRUE);
    InvalidateRect(body_,nullptr,FALSE);
}
void XferPane::Scroll(int amount){offset_=std::clamp(offset_+amount,0,std::max(0,contentHeight_-bodyHeight_));Layout();}
void XferPane::Refresh() {
    if(!window_||!controller_)return;
    if(SendMessageW(Control(Direction),CB_GETDROPPEDSTATE,0,0)||SendMessageW(Control(Mode),CB_GETDROPPEDSTATE,0,0))return;
    const auto target=target_();const auto state=controller_->State();
    const bool receive=SendMessageW(Control(Direction),CB_GETCURSEL,0,0)==1;
    const int protocol=static_cast<int>(SendMessageW(Control(Mode),CB_GETCURSEL,0,0));
    const bool supported=target.connection&&target.connection->IsConnected()&&target.connection->SupportsTransfer()&&target.unavailable.empty();
    bool layout=false;
    layout|=Label(Control(Source),state.busy?L"任务："+state.source:target.name.empty()?L"未选择串口":target.name);
    layout|=Label(Control(FileLabel),receive?L"接收保存到（新文件）":L"发送文件");
    const wchar_t* hints[]={
        L"原始模式没有协议校验。接收长度为0时需手动完成；发送完成只说明写出，不证明设备已保存。",
        L"XMODEM 支持CRC/校验和。接收长度为0时保留块补齐字节；Linux常用rx/sx，按设备实际命令操作。",
        L"YMODEM 传递文件长度并校验数据；Linux常用rb/sb，U-Boot常用loady。每次接收单个文件。",
        L"ZMODEM 支持CRC16/CRC32与重传；Linux常用rz/sz。每次接收单个文件，文件小于4GiB。"};
    auto help=std::wstring(hints[std::clamp(protocol,0,3)]);
    if(receive)help+=L" 接收使用新文件名，不覆盖原文件；本版接收上限4GiB。";
    layout|=Label(Control(Help),help);
    const auto note=!state.busy&&!localNotice_.empty()?localNotice_:
        state.busy||(!state.note.empty()&&state.session==target.id)?state.note:
        target.unavailable.empty()?L"请先在设备端进入匹配的收发模式，再开始传输。":target.unavailable;
    layout|=Label(Control(Status),note+(state.partial.empty()?L"":L"\r\n部分文件："+state.partial));
    const bool relevant=state.session&&(state.busy||state.session==target.id);
    displayBytes_=relevant?state.bytes:0;displayTotal_=relevant?state.total:0;
    const auto count=relevant?std::to_wstring(state.bytes)+L" / "+(state.total?std::to_wstring(state.total):L"未知")+L" 字节":L"等待开始";
    Label(Control(Count),count);
    for(int id:{Direction,Mode,File,Browse})EnableWindow(Control(id),!state.busy);
    EnableWindow(Control(Length),!state.busy&&receive&&protocol<2);
    const bool rawFinish=state.busy&&state.protocol==xfer::Protocol::Raw&&state.direction==xfer::Direction::Receive&&
        state.expectedReceiveBytes==0&&!state.stopping&&!state.finishing;
    Label(Control(Start),rawFinish?L"完成接收":state.busy?(state.stopping?L"正在取消":state.finishing?L"正在保存":L"传输中"):receive?L"开始接收":L"开始发送");
    EnableWindow(Control(Start),rawFinish||(!state.busy&&supported&&!Text(Control(File)).empty()));
    EnableWindow(Control(Cancel),state.busy&&!state.stopping);
    if(state.busy!=wasBusy_){wasBusy_=state.busy;if(stateChanged)stateChanged();}
    if(layout)Layout();
    InvalidateRect(body_,nullptr,FALSE);
    if(state.busy||IsWindowVisible(window_))SetTimer(window_,1,100,nullptr);else KillTimer(window_,1);
}
void XferPane::BrowseFile() {
    const bool receive=SendMessageW(Control(Direction),CB_GETCURSEL,0,0)==1;
    std::wstring path(32768,L'\0');if(receive)path.replace(0,12,L"received.bin");
    OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=GetAncestor(window_,GA_ROOT);dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
    dialog.lpstrFilter=L"所有文件\0*.*\0\0";dialog.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER|OFN_DONTADDTORECENT|(receive?0:OFN_FILEMUSTEXIST);
    if(receive?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)){SetWindowTextW(Control(File),path.c_str());Refresh();}
}
void XferPane::Begin() {
    const auto state=controller_->State();
    if(state.busy) {if(state.protocol==xfer::Protocol::Raw&&state.direction==xfer::Direction::Receive&&state.expectedReceiveBytes==0)controller_->FinishRawReceive();return;}
    const auto target=target_();
    if(!target.unavailable.empty()){localNotice_=target.unavailable;Refresh();return;}
    xfer::TransferController::Request request;request.session=target.id;request.source=target.name;request.path=Text(Control(File));request.connection=target.connection;
    request.options.protocol=static_cast<xfer::Protocol>(std::clamp<LRESULT>(SendMessageW(Control(Mode),CB_GETCURSEL,0,0),0,3));
    request.options.direction=SendMessageW(Control(Direction),CB_GETCURSEL,0,0)==1?xfer::Direction::Receive:xfer::Direction::Send;
    try {if(request.options.direction==xfer::Direction::Receive&&request.options.protocol!=xfer::Protocol::Ymodem&&request.options.protocol!=xfer::Protocol::Zmodem) {
        const auto length=Text(Control(Length));
        if(length.empty()||!std::all_of(length.begin(),length.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';}))throw std::runtime_error("length");
        request.options.expectedReceiveBytes=std::stoull(length);
        if(request.options.expectedReceiveBytes>request.options.receiveLimit)throw std::runtime_error("limit");
    }}catch(...) {localNotice_=L"接收长度应为0至4GiB内的字节数";Refresh();return;}
    std::wstring error;
    if(!controller_->Start(std::move(request),error)){localNotice_=error;Refresh();return;}
    localNotice_.clear();
    Refresh();
}
void XferPane::Paint(HDC target) {
    RECT r{};GetClientRect(body_,&r);UiBufferedPaint paint(target,r);auto dc=paint.Dc();
    FillRect(dc,&r,surface_);const auto c=UiTheme(dark_);
    const int right=std::max(D(16),static_cast<int>(r.right)-D(16)),top=progressTop_-offset_;
    UiBox(dc,{0,top,right,top+D(6)},c.raised,c.raised,D(3));
    if(displayTotal_&&displayBytes_) {
        const auto fraction=static_cast<long double>(std::min(displayBytes_,displayTotal_))/displayTotal_;
        UiBox(dc,{0,top,static_cast<LONG>(right*fraction),top+D(6)},c.accent,c.accent,D(3));
    }
    if(contentHeight_>bodyHeight_) {
        const int thumb=std::max(D(28),bodyHeight_*bodyHeight_/std::max(1,contentHeight_));
        const int y=offset_*(bodyHeight_-thumb)/std::max(1,contentHeight_-bodyHeight_);
        UiBox(dc,{r.right-D(5),y,r.right,y+thumb},c.border,c.border,D(2));
    }
}
LRESULT CALLBACK XferPane::FocusProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
    auto* self=reinterpret_cast<XferPane*>(data);
    if(m==WM_SETFOCUS&&GetParent(h)==self->body_) {
        RECT r{};GetWindowRect(h,&r);MapWindowPoints(nullptr,self->body_,reinterpret_cast<POINT*>(&r),2);
        if(r.top<0)self->Scroll(r.top-self->D(8));else if(r.bottom>self->bodyHeight_)self->Scroll(r.bottom-self->bodyHeight_+self->D(8));
    }
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,FocusProc,id);
    return DefSubclassProc(h,m,w,l);
}
LRESULT CALLBACK XferPane::BodyProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    auto* self=reinterpret_cast<XferPane*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE){self=static_cast<XferPane*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(h,m,w,l);
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT){PAINTSTRUCT p{};auto dc=BeginPaint(h,&p);self->Paint(dc);EndPaint(h,&p);return 0;}
    if(m==WM_PRINTCLIENT){self->Paint(reinterpret_cast<HDC>(w));return 0;}
    if(m==WM_MOUSEWHEEL){self->Scroll(-GET_WHEEL_DELTA_WPARAM(w)*self->D(48)/WHEEL_DELTA);return 0;}
    if(m==WM_LBUTTONDOWN&&GET_X_LPARAM(l)>=self->D(0)) {
        RECT r{};GetClientRect(h,&r);
        if(GET_X_LPARAM(l)>=r.right-self->D(16)&&self->contentHeight_>self->bodyHeight_) {
            const int thumb=std::max(self->D(28),self->bodyHeight_*self->bodyHeight_/std::max(1,self->contentHeight_));
            const int top=self->offset_*(self->bodyHeight_-thumb)/std::max(1,self->contentHeight_-self->bodyHeight_);
            self->grab_=GET_Y_LPARAM(l)>=top&&GET_Y_LPARAM(l)<top+thumb?GET_Y_LPARAM(l)-top:thumb/2;
            self->dragging_=true;SetCapture(h);SendMessageW(h,WM_MOUSEMOVE,MK_LBUTTON,l);return 0;
        }
    }
    if(m==WM_MOUSEMOVE&&self->dragging_) {
        const int thumb=std::max(self->D(28),self->bodyHeight_*self->bodyHeight_/std::max(1,self->contentHeight_));
        self->offset_=std::clamp((GET_Y_LPARAM(l)-self->grab_)*(self->contentHeight_-self->bodyHeight_)/std::max(1,self->bodyHeight_-thumb),0,std::max(0,self->contentHeight_-self->bodyHeight_));
        self->Layout();return 0;
    }
    if(m==WM_LBUTTONUP||m==WM_CAPTURECHANGED||m==WM_CANCELMODE){self->dragging_=false;if(m!=WM_CAPTURECHANGED&&GetCapture()==h)ReleaseCapture();}
    if(m==WM_COMMAND||m==WM_DRAWITEM||m==WM_MEASUREITEM||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX)
        return SendMessageW(self->window_,m,w,l);
    return DefWindowProcW(h,m,w,l);
}
LRESULT CALLBACK XferPane::Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    auto* self=reinterpret_cast<XferPane*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE){self=static_cast<XferPane*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(h,m,w,l);
    if(m==WM_SIZE){self->Layout();return 0;}
    if(m==WM_SETFOCUS){SetFocus(self->Control(File));return 0;}
    if(m==WM_SHOWWINDOW){if(w)self->Refresh();else if(!self->controller_->Busy())KillTimer(h,1);return 0;}
    if(m==WM_TIMER){self->Refresh();return 0;}
    if(m==WM_APP+219) {
        if(SendMessageW(self->Control(Direction),CB_GETDROPPEDSTATE,0,0)||SendMessageW(self->Control(Mode),CB_GETDROPPEDSTATE,0,0))return 0;
        if(self->directionChanged_){self->directionChanged_=false;SetWindowTextW(self->Control(File),L"");}
        self->localNotice_.clear();self->Refresh();return 0;
    }
    if(m==WM_ERASEBKGND){RECT r{};GetClientRect(h,&r);if(self->surface_)FillRect(reinterpret_cast<HDC>(w),&r,self->surface_);return 1;}
    if(m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX) {
        const auto c=UiTheme(self->dark_);auto dc=reinterpret_cast<HDC>(w);SetTextColor(dc,c.text);SetBkColor(dc,m==WM_CTLCOLOREDIT?c.field:c.surface);
        return reinterpret_cast<LRESULT>(m==WM_CTLCOLOREDIT?self->field_:self->surface_);
    }
    if(m==WM_COMMAND) {
        const int id=LOWORD(w),note=HIWORD(w);
        if(note==BN_CLICKED) {if(id==Start)self->Begin();else if(id==Cancel){self->controller_->Cancel();self->Refresh();}else if(id==Browse)self->BrowseFile();}
        else if(note==CBN_SELENDOK||note==CBN_CLOSEUP) {if(id==Direction&&note==CBN_SELENDOK)self->directionChanged_=true;PostMessageW(h,WM_APP+219,0,0);}
        else if(note==EN_CHANGE){self->localNotice_.clear();self->Refresh();}
        return 0;
    }
    if(m==WM_DRAWITEM) {
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if(item.CtlType==ODT_COMBOBOX){UiDrawComboItem(item,self->style_);return TRUE;}
        if(item.CtlType==ODT_BUTTON) {
            UiBufferedPaint paint(item.hDC,item.rcItem);auto dc=paint.Dc();const auto c=UiTheme(self->dark_);
            FillRect(dc,&item.rcItem,self->surface_);const bool enabled=IsWindowEnabled(item.hwndItem)!=FALSE;
            const auto v=UiButtonVisual(c,enabled,item.itemState&ODS_SELECTED,GetPropW(item.hwndItem,L"SerialCtl.PowerHover")!=nullptr,item.CtlID==Start);
            UiBox(dc,item.rcItem,v.fill,v.border,UiCapsuleRadius(item.rcItem));
            UiButtonKeyboardFocus(dc,item.rcItem,item.itemState,self->dpi_,v.ink);
            if(!UiDrawButtonGlyph(dc,item,v.ink,self->dpi_)) {
                const auto old=SelectObject(dc,self->font_);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,item.CtlID==Cancel&&enabled?c.danger:v.ink);
                auto r=item.rcItem;DrawTextW(dc,Text(item.hwndItem).c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);
            }
            return TRUE;
        }
    }
    if(m==WM_DESTROY){KillTimer(h,1);return 0;}
    if(m==WM_NCDESTROY){self->window_=nullptr;self->body_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);}
    return DefWindowProcW(h,m,w,l);
}
} // namespace serialctl

