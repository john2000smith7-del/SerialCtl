#include "UiStyle.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace {
void Require(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
struct Canvas {
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=nullptr; HGDIOBJ previous=nullptr; DWORD *pixels=nullptr;
    static constexpr int Side=80;
    Canvas() {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=Side;info.bmiHeader.biHeight=-Side;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void *data=nullptr;bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
        pixels=static_cast<DWORD *>(data);Require(dc&&bitmap&&pixels,"Cannot create the native comparison canvas");
        previous=SelectObject(dc,bitmap);
    }
    ~Canvas() { if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc); }
    void Clear(COLORREF color) {
        RECT bounds{0,0,Side,Side};SetDCBrushColor(dc,color);FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void Save(const char *path) {
        std::ofstream out(path,std::ios::binary);out<<"P6\n"<<Side<<' '<<Side<<"\n255\n";
        for(int i=0;i<Side*Side;++i) {
            const char rgb[]{static_cast<char>((pixels[i]>>16)&255),static_cast<char>((pixels[i]>>8)&255),static_cast<char>(pixels[i]&255)};
            out.write(rgb,3);
        }
    }
};
unsigned notifications=0,depth=0,maximumDepth=0;
LRESULT CALLBACK ObserveRegion(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(m==WM_WINDOWPOSCHANGED) { ++notifications;++depth;maximumDepth=std::max(maximumDepth,depth); }
    const auto result=DefSubclassProc(h,m,w,l);
    if(m==WM_WINDOWPOSCHANGED)--depth;
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,ObserveRegion,id);
    return result;
}
void CheckRegion() {
    const auto parent=CreateWindowExW(0,L"STATIC",L"Region callback counter",WS_OVERLAPPEDWINDOW,
        0,0,500,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    bool dark=false;
    serialctl::UiFieldStyle style{&dark,static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),96};
    const auto combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_HASSTRINGS,
        20,20,200,180,parent,reinterpret_cast<HMENU>(100),GetModuleHandleW(nullptr),nullptr);
    COMBOBOXINFO info{sizeof(info)};Require(GetComboBoxInfo(combo,&info)!=FALSE,"No native popup for region checks");
    SetWindowSubclass(info.hwndList,ObserveRegion,99,0);
    serialctl::UiStyleField(combo,&style);
    std::cout<<"REGION_INITIAL callbacks="<<notifications<<" maximumDepth="<<maximumDepth<<"\n";
    const auto before=notifications;
    for(int i=0;i<100;++i)serialctl::UiComboRegion(info.hwndList,96);
    Require(notifications==before,"Unchanged popup geometry kept generating native position notifications");
    HRGN first=CreateRectRgn(0,0,0,0),second=CreateRectRgn(0,0,0,0);
    Require(GetWindowRgn(info.hwndList,first)!=ERROR,"The native popup lost its rounded region");
    SetWindowPos(info.hwndList,nullptr,0,0,240,220,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    Require(GetWindowRgn(info.hwndList,second)!=ERROR&&!EqualRgn(first,second),"Resizing did not update the actual native region");
    DeleteObject(first);DeleteObject(second);DestroyWindow(parent);
    std::cout<<"REGION callbacks="<<notifications<<" maximumDepth="<<maximumDepth<<" unchangedUpdates=100\n";
}
}
int main() {
    SetProcessDPIAware();INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token{};
    if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)return 2;
    int result=0;
    try {
        Canvas reference,candidate;
        unsigned cases=0,different=0,maximumError=0,badPixels=0;
        bool saved=false;
        for(const auto origin : {POINT{0,0}, POINT{7,5}, POINT{29,27}})
        for(int size:{16,24,48})for(auto ink:{RGB(29,29,31),RGB(245,245,247),RGB(31,79,216),RGB(152,152,157)})
            for(auto background:{RGB(255,255,255),RGB(36,36,38)})for(int glyph=0;glyph<27;++glyph) {
                reference.Clear(background);candidate.Clear(background);const RECT bounds{origin.x,origin.y,origin.x+size,origin.y+size};
                Require(serialctl::UiResourceGlyphDirect(reference.dc,bounds,ink,glyph),"Original artwork failed to render");
                Require(serialctl::UiResourceGlyph(candidate.dc,bounds,ink,glyph),"Cached artwork failed to render");
                GdiFlush();
                for(int i=0;i<Canvas::Side*Canvas::Side;++i) {
                    const auto a=reference.pixels[i],b=candidate.pixels[i];
                    if((a&0xffffff)!=(b&0xffffff))++different;
                    for(int shift:{0,8,16}) {
                        const auto delta=static_cast<unsigned>(std::abs(static_cast<int>((a>>shift)&255)-static_cast<int>((b>>shift)&255)));
                        maximumError=std::max(maximumError,delta);
                        if(delta>2) {
                            ++badPixels;
                            if(!saved) {
                                saved=true;std::cerr<<"FIRST_GLYPH_DIFFERENCE glyph="<<glyph<<" size="<<size<<" ink="<<ink
                                    <<" background="<<background<<" x="<<i%Canvas::Side<<" y="<<i/Canvas::Side
                                    <<" direct="<<std::hex<<(a&0xffffff)<<" cached="<<(b&0xffffff)<<std::dec<<" delta="<<delta<<"\n";
                                reference.Save("ui-glyph-direct.ppm");candidate.Save("ui-glyph-cached.ppm");
                            }
                        }
                    }
                }
                ++cases;
            }
        std::cout<<"GLYPH cases="<<cases<<" differentPixels="<<different<<" maxChannelError="<<maximumError<<" badChannels="<<badPixels<<"\n";
        Require(maximumError<=2,"Cached glyph changed the approved artwork or color beyond the two-byte bicubic-coordinate rounding bound");
        CheckRegion();
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n';result=1; }
    Gdiplus::GdiplusShutdown(token);return result;
}
