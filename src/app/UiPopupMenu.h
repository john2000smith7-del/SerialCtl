#pragma once
#include "UiStyle.h"
#include <deque>
#include <map>

namespace serialctl {
// Keep the native menu loop, keyboard semantics and accessibility. Only item
// measurement/painting and the popup contour are shared with themed fields.
class UiPopupMenu {
    struct Row {
        HMENU menu{};
        UINT position{}, originalType{};
        ULONG_PTR originalData{};
        std::wstring label;
        bool separator = false;
    };
    struct Context {
        UiFieldStyle style;
        std::deque<Row> rows;
        std::vector<HWND> popups;
        std::map<HWND,SIZE> regionSizes;
        int D(int n) const { return MulDiv(n, style.dpi, 96); }
        void Prepare(HMENU menu) {
            for (int i = 0; i < GetMenuItemCount(menu); ++i) {
                MENUITEMINFOW info{sizeof(info)};
                info.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(menu,i,TRUE,&info)) continue;
                std::wstring text(info.cch + 1, L'\0');
                info.dwTypeData = text.data(); info.cch = static_cast<UINT>(text.size());
                if (!GetMenuItemInfoW(menu,i,TRUE,&info)) continue;
                text.resize(info.cch);
                if (text.empty() && (info.fType & MFT_OWNERDRAW) && info.dwItemData)
                    text = reinterpret_cast<const wchar_t*>(info.dwItemData);
                rows.push_back({menu,static_cast<UINT>(i),info.fType,info.dwItemData,std::move(text),!!(info.fType&MFT_SEPARATOR)});
                MENUITEMINFOW changed{sizeof(changed)};
                changed.fMask = MIIM_FTYPE | MIIM_DATA;
                changed.fType = info.fType | MFT_OWNERDRAW;
                changed.dwItemData = reinterpret_cast<ULONG_PTR>(&rows.back());
                SetMenuItemInfoW(menu,i,TRUE,&changed);
                if (info.hSubMenu) Prepare(info.hSubMenu);
            }
        }
        void Restore() {
            for (const auto& row : rows) {
                MENUITEMINFOW info{sizeof(info)};
                info.fMask=MIIM_FTYPE|MIIM_DATA;info.fType=row.originalType;info.dwItemData=row.originalData;
                SetMenuItemInfoW(row.menu,row.position,TRUE,&info);
            }
        }
    };
    inline static thread_local Context* active_ = nullptr;
    static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto& c=*reinterpret_cast<Context*>(data);
        if (m==WM_NCPAINT) {
            RECT bounds{},client{};GetWindowRect(h,&bounds);GetClientRect(h,&client);
            POINT origin{};ClientToScreen(h,&origin);
            OffsetRect(&client,origin.x-bounds.left,origin.y-bounds.top);
            OffsetRect(&bounds,-bounds.left,-bounds.top);
            if(HDC dc=GetWindowDC(h)) {
                const int saved=SaveDC(dc);
                ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
                const auto colors=UiFieldColors(c.style);
                UiBox(dc,bounds,colors.field,colors.border,c.D(10));
                RestoreDC(dc,saved);ReleaseDC(h,dc);
            }
            return 0;
        }
        if (m==WM_WINDOWPOSCHANGED) {
            RECT r{};GetWindowRect(h,&r);
            if(r.right>r.left&&r.bottom>r.top) {
                const SIZE size{r.right-r.left,r.bottom-r.top};
                const auto found=c.regionSizes.find(h);
                // SetWindowRgn synchronously sends WINDOWPOS notifications.
                // Publish the dimensions first, including for reentrant calls.
                if(found==c.regionSizes.end()||found->second.cx!=size.cx||found->second.cy!=size.cy) {
                    HRGN region=CreateRoundRectRgn(0,0,size.cx+1,size.cy+1,c.D(20),c.D(20));
                    if(region) {
                        c.regionSizes[h]=size;
                        if(!SetWindowRgn(h,region,FALSE)){DeleteObject(region);c.regionSizes.erase(h);}
                    }
                }
            }
        }
        if(m==WM_NCDESTROY){c.regionSizes.erase(h);RemoveWindowSubclass(h,PopupProc,id);}
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l) {
        if(code>=0&&active_) {
            const auto& message=*reinterpret_cast<CWPSTRUCT*>(l);
            if(message.message==WM_CREATE||message.message==WM_WINDOWPOSCHANGING) {
                wchar_t name[24]{};GetClassNameW(message.hwnd,name,24);
                if(wcscmp(name,L"#32768")==0) {
                    DWORD_PTR unused{};
                    if(!GetWindowSubclass(message.hwnd,PopupProc,219,&unused) &&
                        SetWindowSubclass(message.hwnd,PopupProc,219,reinterpret_cast<DWORD_PTR>(active_)))
                        active_->popups.push_back(message.hwnd);
                }
            }
        }
        return CallNextHookEx(nullptr,code,w,l);
    }
    static LRESULT CALLBACK OwnerProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto& c=*reinterpret_cast<Context*>(data);
        if(m==WM_MEASUREITEM) {
            auto& item=*reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if(item.CtlType==ODT_MENU&&item.itemData) {
                const auto& row=*reinterpret_cast<Row*>(item.itemData);
                HDC dc=GetDC(h);auto old=SelectObject(dc,c.style.font);
                RECT r{};DrawTextW(dc,row.label.c_str(),-1,&r,DT_CALCRECT|DT_NOPREFIX);
                SelectObject(dc,old);ReleaseDC(h,dc);
                item.itemWidth=std::max<LONG>(c.D(144),r.right+c.D(60));
                item.itemHeight=row.separator?c.D(12):std::max<LONG>(c.D(32),r.bottom+c.D(12));
                return TRUE;
            }
        }
        if(m==WM_DRAWITEM) {
            auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if(item.CtlType==ODT_MENU&&item.itemData) {
                const auto& row=*reinterpret_cast<Row*>(item.itemData);
                UiBufferedPaint paint(item.hDC,item.rcItem);HDC dc=paint.Dc();
                const auto colors=UiFieldColors(c.style);
                const auto brush=CreateSolidBrush(colors.field);FillRect(dc,&item.rcItem,brush);DeleteObject(brush);
                RECT r=item.rcItem;r.left+=c.D(4);r.right-=c.D(4);
                if(row.separator) {
                    r.left+=c.D(12);r.right-=c.D(12);r.top=(r.top+r.bottom)/2;r.bottom=r.top+std::max(1,c.D(1));
                    const auto line=CreateSolidBrush(colors.border);FillRect(dc,&r,line);DeleteObject(line);
                    return TRUE;
                }
                const bool disabled=!!(item.itemState&(ODS_DISABLED|ODS_GRAYED));
                const bool selected=!!(item.itemState&ODS_SELECTED)&&!disabled;
                if(selected)UiBox(dc,r,colors.accentSoft,colors.accentSoft,c.D(8));
                const auto ink=disabled?colors.muted:colors.text;
                if(item.itemState&ODS_CHECKED) {
                    const int y=(r.top+r.bottom-c.D(16))/2;
                    UiDrawGlyph(dc,{r.left+c.D(8),y,r.left+c.D(24),y+c.D(16)},disabled?colors.muted:UiAccentInk(*c.style.dark),UiGlyph::Check);
                }
                auto text=r;text.left+=c.D(32);text.right-=c.D(16);
                const auto old=SelectObject(dc,c.style.font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ink);
                const auto split=row.label.find(L'\n');
                if(split==std::wstring::npos)DrawTextW(dc,row.label.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                else {
                    RECT title=text;title.top+=c.D(4);title.bottom=title.top+c.D(20);
                    DrawTextW(dc,row.label.data(),static_cast<int>(split),&title,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                    text.top=title.bottom;SetTextColor(dc,colors.muted);
                    DrawTextW(dc,row.label.c_str()+split+1,-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                }
                SelectObject(dc,old);return TRUE;
            }
        }
        return DefSubclassProc(h,m,w,l);
    }
  public:
    static UINT Track(HMENU menu, UINT flags, int x, int y, HWND owner, const UiFieldStyle& style) {
        if(!menu||!IsWindow(owner)||active_)return 0;
        Context context{style,{},{},{}};
        context.Prepare(menu);
        auto* previous=active_;active_=&context;
        const bool subclassed=SetWindowSubclass(owner,OwnerProc,219,reinterpret_cast<DWORD_PTR>(&context))!=FALSE;
        if(!subclassed) {context.Restore();active_=previous;return TrackPopupMenu(menu,flags,x,y,0,owner,nullptr);}
        const auto colors=UiFieldColors(style);
        const auto background=CreateSolidBrush(colors.field);
        MENUINFO previousInfo{sizeof(previousInfo)};previousInfo.fMask=MIM_BACKGROUND;GetMenuInfo(menu,&previousInfo);
        MENUINFO info{sizeof(info)};info.fMask=MIM_BACKGROUND;info.hbrBack=background;SetMenuInfo(menu,&info);
        HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROC,Hook,nullptr,GetCurrentThreadId());
        const UINT command=TrackPopupMenu(menu,flags|TPM_RETURNCMD,x,y,0,owner,nullptr);
        if(hook)UnhookWindowsHookEx(hook);
        for(HWND popup:context.popups) {
            DWORD_PTR reference{};
            if(IsWindow(popup)&&GetWindowSubclass(popup,PopupProc,219,&reference)&&reference==reinterpret_cast<DWORD_PTR>(&context)) {
                RemoveWindowSubclass(popup,PopupProc,219);SetWindowRgn(popup,nullptr,FALSE);
            }
        }
        RemoveWindowSubclass(owner,OwnerProc,219);
        context.Restore();active_=previous;
        info.hbrBack=previousInfo.hbrBack;SetMenuInfo(menu,&info);DeleteObject(background);
        return command;
    }
};
} // namespace serialctl
