#include "XferPane.h"
#include "UiMetrics.h"
#include <commdlg.h>
#include <limits>
namespace serialctl {
namespace {
std::wstring Text(HWND h){const int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,s.data(),n+1);s.resize(n);return s;}
bool Label(HWND h,const std::wstring& value){if(Text(h)==value)return false;SetWindowTextW(h,value.c_str());return true;}
int Measure(HWND h,HFONT font,const std::wstring& text,int width){
    if (text.empty()) return 0;
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
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"串口传输文件",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,parent,nullptr,wc.hInstance,this);
    if(!window_)return false;
    wc.lpfnWndProc=BodyProc;wc.lpszClassName=L"SerialCtlXferBody";RegisterClassW(&wc);
    body_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"传输选项",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,0,0,100,100,window_,nullptr,wc.hInstance,this);
    if(!body_)return false;
    const auto add=[&](int id,const wchar_t* cls,const wchar_t* text,DWORD extra){
        auto h=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|extra,0,0,1,1,id==Start||id==Cancel?window_:body_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        if(_wcsicmp(cls,L"BUTTON")==0)UiStyleButton(h);
        if(_wcsicmp(cls,L"EDIT")==0||_wcsicmp(cls,L"COMBOBOX")==0) {
            UiStyleField(h,&style_);
            SetPropW(h,L"SerialCtl.FieldSurface",reinterpret_cast<HANDLE>(1));
        }
        if(extra&WS_TABSTOP)SetWindowSubclass(h,FocusProc,219,reinterpret_cast<DWORD_PTR>(this));
        return h;
    };
    add(Source,L"STATIC",L"未选择串口",SS_ENDELLIPSIS|SS_NOPREFIX);
    add(DirectionLabel,L"STATIC",L"方向",SS_NOPREFIX);add(ModeLabel,L"STATIC",L"协议",SS_NOPREFIX);
    for(int id:{Direction,Mode})add(id,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP);
    for(const auto* s:{L"发送文件",L"接收文件"})SendMessageW(Control(Direction),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    for(const auto* s:{L"原始二进制",L"XMODEM",L"YMODEM",L"ZMODEM"})SendMessageW(Control(Mode),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    SendMessageW(Control(Direction),CB_SETCURSEL,0,0);SendMessageW(Control(Mode),CB_SETCURSEL,2,0);
    add(FileLabel,L"STATIC",L"发送文件",SS_NOPREFIX);add(File,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
    auto browseBtn = add(Browse,L"BUTTON",L"选择文件",BS_OWNERDRAW|WS_TABSTOP);
    UiSetFlatIconButton(browseBtn);
    UiSetButtonGlyph(browseBtn,UiGlyph::Folder,L"选择文件");
    add(LengthLabel,L"STATIC",L"接收长度（字节，0 表示未知）",SS_NOPREFIX);add(Length,L"EDIT",L"0",ES_AUTOHSCROLL|ES_NUMBER|WS_TABSTOP);
    add(Help,L"STATIC",L"",SS_EDITCONTROL|SS_NOPREFIX);add(Status,L"STATIC",L"",SS_EDITCONTROL|SS_NOPREFIX);
    add(Count,L"STATIC",L"",SS_ENDELLIPSIS|SS_NOPREFIX);
    auto startBtn = add(Start,L"BUTTON",L"开始发送",BS_OWNERDRAW|WS_TABSTOP);
    auto cancelBtn = add(Cancel,L"BUTTON",L"取消传输",BS_OWNERDRAW|WS_TABSTOP);
    UiSetFlatIconButton(startBtn);
    UiSetFlatIconButton(cancelBtn);
    UiSetButtonGlyph(cancelBtn,UiGlyph::Stop,L"停止当前传输");
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
        if(_wcsicmp(cls,L"EDIT")==0||_wcsicmp(cls,L"COMBOBOX")==0) {
            UiStyleField(h,&style_);
            SetPropW(h,L"SerialCtl.FieldSurface",reinterpret_cast<HANDLE>(1));
        }
    }
    Layout();RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
void XferPane::Layout() {
    if(!body_)return;
    RECT client{};GetClientRect(window_,&client);
    const int width=std::max(1L,client.right),footer=D(48);
    bodyHeight_=std::max(1,static_cast<int>(client.bottom)-footer);
    UiLayoutBatch frame(true);
    frame.Move(body_,0,0,width,bodyHeight_);
    const int lane=D(16),contentWidth=std::max(D(80),width-lane);
    const int half=std::max(1,(contentWidth-D(12))/2);
    const int helpHeight=std::max(D(28),Measure(body_,font_,Text(Control(Help)),contentWidth)+D(2));
    const int countHeight=std::max(D(20),Measure(body_,font_,Text(Control(Count)),contentWidth));
    const int statusHeight=std::max(D(28),Measure(body_,font_,Text(Control(Status)),contentWidth)+D(2));
    const int ySource = 0;
    const int yDirLabel = ySource + D(32);
    const int yDir = yDirLabel + D(22);
    const int yFileLabel = yDir + D(36);
    const int yFile = yFileLabel + D(22);
    const int yLengthLabel = yFile + D(36);
    const int yLength = yLengthLabel + D(22);
    const int yHelp = yLength + D(36);
    progressTop_ = yHelp + helpHeight + D(12);
    const int countTop = progressTop_ + D(6) + D(10);
    const int statusTop = countTop + countHeight + D(8);
    contentHeight_ = statusTop + statusHeight + D(16);
    offset_ = std::clamp(offset_,0,std::max(0,contentHeight_-bodyHeight_));
    UiLayoutBatch fields(true);
    const auto move=[&](int id,int x,int y,int w,int h){fields.Move(Control(id),x,y-offset_,std::max(1,w),h);};
    move(Source,0,ySource,contentWidth,D(28));
    move(DirectionLabel,0,yDirLabel,half,D(20));move(ModeLabel,half+D(12),yDirLabel,half,D(20));
    move(Direction,0,yDir,half,D(180));move(Mode,half+D(12),yDir,half,D(200));
    fields.ComboVisibleHeight(Control(Direction),D(28));
    fields.ComboVisibleHeight(Control(Mode),D(28));
    move(FileLabel,0,yFileLabel,contentWidth,D(20));move(File,0,yFile,contentWidth-D(40),D(28));move(Browse,contentWidth-D(28),yFile,D(28),D(28));
    move(LengthLabel,0,yLengthLabel,contentWidth,D(20));move(Length,0,yLength,contentWidth,D(28));
    move(Help,0,yHelp,contentWidth,helpHeight);
    move(Count,0,countTop,contentWidth,countHeight);move(Status,0,statusTop,contentWidth,statusHeight);
    const int buttonWidth=std::max(1,(width-D(12))/2),top=std::max(0,static_cast<int>(client.bottom)-D(36));
    frame.Move(Control(Start),0,top,buttonWidth,D(36));
    frame.Move(Control(Cancel),buttonWidth+D(12),top,buttonWidth,D(36));
    frame.Commit();
    fields.Commit();
    RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
}
void XferPane::Scroll(int amount){offset_=std::clamp(offset_+amount,0,std::max(0,contentHeight_-bodyHeight_));Layout();}
void XferPane::Refresh() {
    if(!window_||!controller_)return;
    if(SendMessageW(Control(Direction),CB_GETDROPPEDSTATE,0,0)||SendMessageW(Control(Mode),CB_GETDROPPEDSTATE,0,0))return;
    const auto target=target_();const auto state=controller_->State();
    if(target.id!=lastSessionId_) {
        lastSessionId_=target.id;
        localNotice_.clear();
    }
    const bool receive=SendMessageW(Control(Direction),CB_GETCURSEL,0,0)==1;
    const int protocol=static_cast<int>(SendMessageW(Control(Mode),CB_GETCURSEL,0,0));
    const bool supported=target.connection&&target.connection->IsConnected()&&target.connection->SupportsTransfer()&&target.unavailable.empty();
    const bool relevant=(target.id!=0)&&(state.session==target.id);
    const bool targetActive=state.busy&&relevant;
    targetActive_=targetActive;
    const bool showPartial=!state.partial.empty()&&relevant;

    const std::wstring sourceText=state.busy?L"任务："+state.source:target.name.empty()?L"未选择串口":target.name;
    const std::wstring fileLabelText=receive?L"接收保存到（新文件）":L"发送文件";
    const wchar_t* hints[]={
        L"原始模式没有协议校验。接收长度为0时需手动完成；发送完成只说明写出，不证明设备已保存。",
        L"XMODEM 支持CRC/校验和。接收长度为0时保留块补齐字节；Linux常用rx/sx，按设备实际命令操作。",
        L"YMODEM 传递文件长度并校验数据；Linux常用rb/sb，U-Boot常用loady。每次接收单个文件。",
        L"ZMODEM 支持CRC16/CRC32与重传；Linux常用rz/sz。每次接收单个文件，文件小于4GiB。"};
    auto help=std::wstring(hints[std::clamp(protocol,0,3)]);
    if(receive)help+=L" 接收使用新文件名，不覆盖原文件；本版接收上限4GiB。";

    const auto note=state.busy&&!relevant?L"其他连接正在传输，切回任务来源可查看或停止。":
        !state.busy&&!localNotice_.empty()?localNotice_:
        targetActive||(!state.note.empty()&&relevant)?state.note:
        target.unavailable.empty()?(receive&&protocol==0?
            L"请先开始接收，再触发设备发送原始字节；原始模式不会补收开始前的数据。":
            L"请先在设备端进入匹配的收发模式，再开始传输。"):target.unavailable;
    const std::wstring statusText=note+(showPartial?L"\r\n部分文件："+state.partial:L"");

    std::wstring countText;
    if(targetActive) {
        if(state.total>0) countText=std::to_wstring(state.bytes)+L" / "+std::to_wstring(state.total)+L" 字节";
        else if(state.bytes==0) countText=L"等待数据…";
        else countText=L"已接收 "+std::to_wstring(state.bytes)+L" 字节（总长度未知）";
    } else if(relevant) {
        if(state.success) countText=L"传输完成 ("+std::to_wstring(state.bytes)+L" 字节)";
        else if(state.cancelled) countText=L"传输已停止 ("+std::to_wstring(state.bytes)+L" 字节)";
        else if(state.bytes>0) countText=std::to_wstring(state.bytes)+L" 字节";
        else countText=L"等待开始";
    } else {
        countText=L"等待开始";
    }

    bool layoutNeeded=false;
    layoutNeeded|=Label(Control(Source),sourceText);
    layoutNeeded|=Label(Control(FileLabel),fileLabelText);
    layoutNeeded|=Label(Control(Help),help);
    layoutNeeded|=Label(Control(Status),statusText);
    layoutNeeded|=Label(Control(Count),countText);

    displayBytes_=targetActive?state.bytes:(relevant?state.bytes:0);
    displayTotal_=targetActive?state.total:(relevant?state.total:0);

    if((IsWindowEnabled(Control(File))!=FALSE)!=true)EnableWindow(Control(File),TRUE);
    if(((GetWindowLongPtrW(Control(File),GWL_STYLE)&ES_READONLY)!=0)!=state.busy)
        SendMessageW(Control(File),EM_SETREADONLY,state.busy?TRUE:FALSE,0);
    for(int id:{Direction,Mode,Browse}) {
        if((IsWindowEnabled(Control(id))!=FALSE)!=(!state.busy))
            EnableWindow(Control(id),!state.busy);
    }
    const bool lengthEnabled=!state.busy&&receive&&protocol<2;
    if((IsWindowEnabled(Control(Length))!=FALSE)!=lengthEnabled)
        EnableWindow(Control(Length),lengthEnabled);
    if(((GetWindowLongPtrW(Control(Length),GWL_STYLE)&ES_READONLY)!=0)!=state.busy)
        SendMessageW(Control(Length),EM_SETREADONLY,state.busy?TRUE:FALSE,0);

    const bool rawFinish=targetActive&&state.protocol==xfer::Protocol::Raw&&state.direction==xfer::Direction::Receive&&
        state.expectedReceiveBytes==0&&!state.stopping&&!state.finishing;
    lastRender_.rawFinish=rawFinish;
    UiGlyph startGlyph=UiGlyph::Run;
    std::wstring startTip,startText;
    if(rawFinish) {
        startGlyph=UiGlyph::Check;
        startTip=L"完成接收并保存当前已收数据";
        startText=L"完成接收";
    } else if(state.busy) {
        startGlyph=state.stopping?UiGlyph::Stop:(receive?UiGlyph::Import:UiGlyph::Export);
        startTip=state.stopping?L"正在取消传输…":state.finishing?L"正在保存文件…":(receive?L"正在接收文件…":L"正在发送文件…");
        startText=state.stopping?L"正在取消":state.finishing?L"正在保存":L"传输中";
    } else {
        startGlyph=receive?UiGlyph::Import:UiGlyph::Export;
        startTip=receive?L"开始接收文件":L"开始发送文件";
        startText=receive?L"开始接收":L"开始发送";
    }
    Label(Control(Start),startText);
    if(lastRender_.startGlyph!=startGlyph||lastRender_.startTip!=startTip) {
        UiSetButtonGlyph(Control(Start),startGlyph,startTip.c_str());
        lastRender_.startGlyph=startGlyph;
        lastRender_.startTip=startTip;
    }
    const std::wstring cancelTip=L"停止当前传输";
    if(lastRender_.cancelTip!=cancelTip) {
        UiSetButtonGlyph(Control(Cancel),UiGlyph::Stop,cancelTip.c_str());
        lastRender_.cancelTip=cancelTip;
    }
    const bool startEnabled=rawFinish||(!state.busy&&supported&&!Text(Control(File)).empty());
    const bool cancelEnabled=targetActive&&!state.stopping;
    if((IsWindowEnabled(Control(Start))!=FALSE)!=startEnabled) {
        EnableWindow(Control(Start),startEnabled);
    }
    if((IsWindowEnabled(Control(Cancel))!=FALSE)!=cancelEnabled) {
        EnableWindow(Control(Cancel),cancelEnabled);
    }
    lastRender_.startEnabled=startEnabled;
    lastRender_.cancelEnabled=cancelEnabled;

    const auto filePath=Text(Control(File));
    if(filePath!=lastRender_.filePath) {
        UiSetControlTooltip(Control(File),filePath.c_str());
        lastRender_.filePath=filePath;
    }
    const auto srcVal=Text(Control(Source));
    if(srcVal!=lastRender_.sourceText) {
        UiSetControlTooltip(Control(Source),srcVal.c_str());
        lastRender_.sourceText=srcVal;
    }

    if(state.busy!=wasBusy_){wasBusy_=state.busy;if(stateChanged)stateChanged();}
    if(layoutNeeded) {
        Layout();
    } else if(state.busy||targetActive_!=lastRender_.busy||displayBytes_!=lastRender_.bytes||displayTotal_!=lastRender_.total) {
        RECT client{};GetClientRect(body_,&client);
        const int right=std::max(D(16),static_cast<int>(client.right)-D(16)),top=progressTop_-offset_;
        RECT progressRect{0,top,right,top+D(8)};
        InvalidateRect(body_,&progressRect,FALSE);
    }
    lastRender_.busy=targetActive_;
    lastRender_.bytes=displayBytes_;
    lastRender_.total=displayTotal_;
    if(state.busy)SetTimer(window_,1,100,nullptr);else KillTimer(window_,1);
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
    const auto target=target_();
    // A click queued while the button showed Finish must never start another
    // receive after the worker completes but before the next UI refresh.
    if(lastRender_.rawFinish) {
        if(state.busy&&state.session==target.id&&!state.stopping&&!state.finishing&&state.protocol==xfer::Protocol::Raw&&
            state.direction==xfer::Direction::Receive&&state.expectedReceiveBytes==0)
            controller_->FinishRawReceive();
        else Refresh();
        return;
    }
    if(state.busy)return;
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
    UiDrawProgressBar(dc,0,top,right,dpi_,displayBytes_,displayTotal_,targetActive_,c.raised,c.accent);
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
    if(m==WM_SHOWWINDOW){
        if(w){
            self->Refresh();
            RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
        } else if(!self->controller_->Busy())KillTimer(h,1);
        return 0;
    }
    if(m==WM_TIMER){self->Refresh();return 0;}
    if(m==WM_APP+219) {
        if(SendMessageW(self->Control(Direction),CB_GETDROPPEDSTATE,0,0)||SendMessageW(self->Control(Mode),CB_GETDROPPEDSTATE,0,0))return 0;
        if(self->directionChanged_){self->directionChanged_=false;SetWindowTextW(self->Control(File),L"");}
        self->localNotice_.clear();self->Refresh();return 0;
    }
    if(m==WM_ERASEBKGND){RECT r{};GetClientRect(h,&r);if(self->surface_)FillRect(reinterpret_cast<HDC>(w),&r,self->surface_);return 1;}
    if(m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX) {
        const auto c=UiTheme(self->dark_);auto dc=reinterpret_cast<HDC>(w);
        HWND controlHwnd=reinterpret_cast<HWND>(l);
        const bool isEdit=(m==WM_CTLCOLOREDIT)||(controlHwnd==self->Control(File))||(controlHwnd==self->Control(Length));
        if(isEdit) {
            const bool enabled=IsWindowEnabled(controlHwnd)!=FALSE;
            const bool readOnly=(GetWindowLongPtrW(controlHwnd,GWL_STYLE)&ES_READONLY)!=0;
            SetTextColor(dc,(!enabled||readOnly)?c.muted:c.text);
            SetBkColor(dc,c.field);
            return reinterpret_cast<LRESULT>(self->field_);
        }
        SetTextColor(dc,c.text);SetBkColor(dc,c.surface);
        return reinterpret_cast<LRESULT>(self->surface_);
    }
    if(m==WM_COMMAND) {
        const int id=LOWORD(w),note=HIWORD(w);
        if(note==BN_CLICKED) {
            if(id==Start)self->Begin();
            else if(id==Cancel) {
                const auto state=self->controller_->State();
                if(state.busy&&state.session==self->target_().id)self->controller_->Cancel();
                self->Refresh();
            } else if(id==Browse)self->BrowseFile();
        }
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
            const bool flat=GetPropW(item.hwndItem,L"SerialCtl.FlatIconVisual")!=nullptr;
            const bool pressed=(item.itemState&ODS_SELECTED)!=0;
            const bool hovered=GetPropW(item.hwndItem,L"SerialCtl.PowerHover")!=nullptr;
            const auto v=flat?UiFlatIconVisual(c,c.surface,enabled,pressed,hovered):UiButtonVisual(c,enabled,pressed,hovered,item.CtlID==Start);
            if(!flat||v.fill!=c.surface)
                UiBox(dc,item.rcItem,v.fill,flat?v.fill:v.border,UiCapsuleRadius(item.rcItem));
            UiButtonKeyboardFocus(dc,item.rcItem,item.itemState,self->dpi_,flat?UiFocusInk(self->dark_):v.ink);
            if(!UiDrawButtonGlyph(dc,item,item.CtlID==Cancel&&enabled?c.danger:v.ink,self->dpi_)) {
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
