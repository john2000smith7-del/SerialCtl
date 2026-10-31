#pragma once
#include "UiStyle.h"
#include "UiDialogViewport.h"

namespace serialctl {
class UiPasswordPrompt {
public:
    static bool Show(HWND owner,HFONT font,bool dark,int dpi,std::wstring& value) {
        State state{font,dark,dpi};state.field={&state.dark,font,dpi};
        struct Template {DLGTEMPLATE dialog{};WORD menu=0,cls=0,title=0;} data;
        data.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;data.dialog.cx=200;data.dialog.cy=100;
        const auto result=UiDialogBoxIndirectOwned(GetModuleHandleW(nullptr),&data.dialog,owner,Proc,reinterpret_cast<LPARAM>(&state));
        if(result==IDOK)value=std::move(state.value);
        if(!state.value.empty())SecureZeroMemory(state.value.data(),state.value.size()*sizeof(wchar_t));
        return result==IDOK;
    }
private:
    struct State {HFONT font;bool dark;int dpi;UiFieldStyle field{};std::wstring value;};
    static INT_PTR CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(h,DWLP_USER));
        if(m==WM_INITDIALOG) {
            s=reinterpret_cast<State*>(l);SetWindowLongPtrW(h,DWLP_USER,l);SetWindowTextW(h,L"启用 Test");
            UiResizeDialog(h,s->dpi,360,176);
            auto add=[&](const wchar_t* type,const wchar_t* text,int id,int x,int y,int width,int height,DWORD style=0) {
                auto c=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,MulDiv(x,s->dpi,96),MulDiv(y,s->dpi,96),MulDiv(width,s->dpi,96),MulDiv(height,s->dpi,96),h,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
                SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),FALSE);
                if(id==10)UiStyleField(c,&s->field);else if(id==IDOK || id==IDCANCEL)UiStyleButton(c);
                return c;
            };
            add(L"STATIC",L"输入测试密码 · 仅本次运行有效",-1,16,16,328,24);
            auto edit=add(L"EDIT",L"",10,16,48,328,28,ES_PASSWORD|ES_AUTOHSCROLL|WS_TABSTOP);
            SendMessageW(edit,EM_SETLIMITTEXT,128,0);
            add(L"BUTTON",L"取消",IDCANCEL,140,124,96,36,BS_OWNERDRAW|WS_TABSTOP);
            add(L"BUTTON",L"验证",IDOK,248,124,96,36,BS_OWNERDRAW|WS_TABSTOP);
            RECT owner{},r{};GetWindowRect(GetWindow(h,GW_OWNER),&owner);GetWindowRect(h,&r);
            SetWindowPos(h,nullptr,owner.left+(owner.right-owner.left-(r.right-r.left))/2,owner.top+(owner.bottom-owner.top-(r.bottom-r.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
            UiDialogViewport::Fit(h,s->dpi);SetFocus(edit);return FALSE;
        }
        if(!s)return FALSE;
        if(m==WM_COMMAND && LOWORD(w)==IDOK) {
            wchar_t value[129]{};GetDlgItemTextW(h,10,value,129);s->value=value;SecureZeroMemory(value,sizeof(value));UiEndOwnedDialog(h,IDOK);return TRUE;
        }
        if(m==WM_CLOSE || (m==WM_COMMAND && LOWORD(w)==IDCANCEL)){UiEndOwnedDialog(h,IDCANCEL);return TRUE;}
        if(m==WM_DRAWITEM) {
            const auto& i=*reinterpret_cast<DRAWITEMSTRUCT*>(l);const auto c=UiTheme(s->dark);
            UiBufferedPaint paint(i.hDC,i.rcItem);auto dc=paint.Dc();auto v=UiButtonVisual(c,IsWindowEnabled(i.hwndItem)!=FALSE,(i.itemState&ODS_SELECTED)!=0,false,i.CtlID==IDOK);
            UiBox(dc,i.rcItem,v.fill,v.border,MulDiv(18,s->dpi,96));SetTextColor(dc,v.ink);SetBkMode(dc,TRANSPARENT);SelectObject(dc,s->font);
            DrawTextW(dc,i.CtlID==IDOK?L"验证":L"取消",-1,const_cast<RECT*>(&i.rcItem),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            UiButtonKeyboardFocus(dc,i.rcItem,i.itemState,s->dpi);return TRUE;
        }
        if(m==WM_CTLCOLORDLG || m==WM_CTLCOLORSTATIC || m==WM_CTLCOLOREDIT) {
            const auto c=UiTheme(s->dark);SetTextColor(reinterpret_cast<HDC>(w),c.text);const auto bg=m==WM_CTLCOLOREDIT?c.field:c.surface;
            SetBkColor(reinterpret_cast<HDC>(w),bg);SetDCBrushColor(reinterpret_cast<HDC>(w),bg);return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
        }
        return FALSE;
    }
};
} // namespace serialctl
