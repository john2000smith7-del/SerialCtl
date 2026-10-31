#include "UiResources.h"
#include <iostream>
#include <map>
#include <array>
#include <chrono>
using namespace serialctl;
struct Canvas {
    HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;DWORD *data=nullptr;
    Canvas(){BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=80;bi.bmiHeader.biHeight=-80;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;void *p=nullptr;bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&p,nullptr,0);old=SelectObject(dc,bitmap);data=static_cast<DWORD*>(p);}
    ~Canvas(){SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);}
    void Clear(COLORREF color){RECT r{0,0,80,80};SetDCBrushColor(dc,color);FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));}
};
struct Atlas {
    std::vector<DWORD> bytes;std::vector<WORD> wide;std::unique_ptr<Gdiplus::Bitmap> bitmap;
};
std::unique_ptr<Atlas> Make(int mode,COLORREF color) {
    auto &resource=UiResource(2012);auto a=std::make_unique<Atlas>();
    const int width=static_cast<int>(resource.width),height=static_cast<int>(resource.alpha.size()/resource.width);
    const double rgb[]{static_cast<double>(GetBValue(color))+.5,static_cast<double>(GetGValue(color))+.5,static_cast<double>(GetRValue(color))+.5};
    a->bytes.resize(resource.alpha.size());
    for(size_t i=0;i<a->bytes.size();++i) {
        const auto alpha=resource.alpha[i];DWORD pixel=static_cast<DWORD>(alpha)<<24;
        for(int ch=0;ch<3;++ch) {
            double value=(rgb[ch]-(mode==15?.5:0))*alpha/255.;
            unsigned part=static_cast<unsigned>(mode==12?std::floor(value):mode==14?std::ceil(value):std::round(value));
            pixel|=std::min(255u,part)<<(ch*8);
        }
        a->bytes[i]=pixel;
    }
    a->bitmap=std::make_unique<Gdiplus::Bitmap>(width,height,width*4,PixelFormat32bppPARGB,reinterpret_cast<BYTE*>(a->bytes.data()));
    return a;
}
int main(){
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token{};Gdiplus::GdiplusStartup(&token,&input,nullptr);
    int status=0;
    {
        Canvas direct,candidate;
        for(int mode=12;mode<16;++mode) {
            unsigned maxError=0,bad=0,cases=0;int lowest=0,highest=0;
            std::map<COLORREF,std::unique_ptr<Atlas>> atlases;
            auto begin=std::chrono::steady_clock::now();
            bool supported=true;
            for(int size:{16,24,48})for(auto ink:{RGB(29,29,31),RGB(245,245,247),RGB(31,79,216),RGB(152,152,157)})for(auto bg:{RGB(255,255,255),RGB(36,36,38)})for(int glyph=0;glyph<27;++glyph) {
                if(!atlases.count(ink))atlases[ink]=Make(mode,ink);
                auto &atlas=atlases[ink];
                if(!atlas->bitmap || atlas->bitmap->GetLastStatus()!=Gdiplus::Ok){supported=false;continue;}
                direct.Clear(bg);candidate.Clear(bg);const RECT r{7,5,7+size,5+size};
                UiResourceGlyphDirect(direct.dc,r,ink,glyph);
                {
                    Gdiplus::Graphics g(candidate.dc);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    Gdiplus::ImageAttributes attr;attr.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
                    if(g.DrawImage(atlas->bitmap.get(),Gdiplus::Rect(r.left,r.top,size,size),glyph*96,0,96,96,Gdiplus::UnitPixel,&attr)!=Gdiplus::Ok){supported=false;continue;}
                }
                GdiFlush();
                for(int i=0;i<6400;++i)for(int shift:{0,8,16}) {
                    const int diff=static_cast<int>((direct.data[i]>>shift)&255)-static_cast<int>((candidate.data[i]>>shift)&255);
                    lowest=std::min(lowest,diff);highest=std::max(highest,diff);maxError=std::max(maxError,static_cast<unsigned>(std::abs(diff)));if(std::abs(diff)>1)++bad;
                }
                ++cases;
            }
            auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            std::cout<<"GLYPH_FILTER mode="<<mode<<" supported="<<supported<<" cases="<<cases<<" maxError="<<maxError<<" badChannels="<<bad<<" signedLow="<<lowest<<" signedHigh="<<highest<<" elapsedMs="<<ms<<"\n";
        }
    }
    Gdiplus::GdiplusShutdown(token);return status;
}
