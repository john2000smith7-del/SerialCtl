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
    if(mode>=4) {
        a->wide.resize(resource.alpha.size()*4);
        for(size_t i=0;i<resource.alpha.size();++i) {
            for(int ch=0;ch<3;++ch)a->wide[i*4+ch]=static_cast<WORD>(std::min(65535.,std::round(rgb[ch]*257*(mode==5?resource.alpha[i]/255.:1.))));
            a->wide[i*4+3]=resource.alpha[i]*257;
        }
        a->bitmap=std::make_unique<Gdiplus::Bitmap>(width,height,width*8,mode==5?PixelFormat64bppPARGB:PixelFormat64bppARGB,reinterpret_cast<BYTE*>(a->wide.data()));
    } else {
        a->bytes.resize(resource.alpha.size());
        const int format=mode==3?PixelFormat32bppPARGB:PixelFormat32bppARGB;
        a->bitmap=std::make_unique<Gdiplus::Bitmap>(width,height,width*4,format,reinterpret_cast<BYTE*>(a->bytes.data()));
        if(mode>=2) {
            Gdiplus::Graphics g(a->bitmap.get());g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::ImageAttributes attr;UiResourceTint(attr,color);
            g.DrawImage(resource.bitmap.get(),Gdiplus::Rect(0,0,width,height),0,0,width,height,Gdiplus::UnitPixel,&attr);
        } else {
            const DWORD c0=(std::min(255,GetRValue(color)+mode)<<16)|(std::min(255,GetGValue(color)+mode)<<8)|std::min(255,GetBValue(color)+mode);
            for(size_t i=0;i<a->bytes.size();++i)a->bytes[i]=(static_cast<DWORD>(resource.alpha[i])<<24)|c0;
        }
    }
    return a;
}
int main(){
    Gdiplus::GdiplusStartupInput input;ULONG_PTR token{};Gdiplus::GdiplusStartup(&token,&input,nullptr);
    int status=0;
    {
        Canvas direct,candidate;
        for(int mode=6;mode<11;++mode) {
            unsigned maxError=0,bad=0,cases=0;int lowest=0,highest=0;
            std::map<COLORREF,std::unique_ptr<Atlas>> atlases;
            auto begin=std::chrono::steady_clock::now();
            bool supported=true;
            for(int size:{16,24,48})for(auto ink:{RGB(29,29,31),RGB(245,245,247),RGB(31,79,216),RGB(152,152,157)})for(auto bg:{RGB(255,255,255),RGB(36,36,38)})for(int glyph=0;glyph<27;++glyph) {
                if(!atlases.count(ink)) {
                    auto a=std::make_unique<Atlas>();
                    auto &source=UiResource(2012);
                    a->bitmap.reset(source.bitmap->Clone(0,0,2592,96,mode==6?PixelFormat32bppARGB:PixelFormat32bppPARGB));
                    atlases[ink]=std::move(a);
                }
                auto &atlas=atlases[ink];
                if(!atlas->bitmap || atlas->bitmap->GetLastStatus()!=Gdiplus::Ok){supported=false;continue;}
                direct.Clear(bg);candidate.Clear(bg);const RECT r{7,5,7+size,5+size};
                UiResourceGlyphDirect(direct.dc,r,ink,glyph);
                {
                    Gdiplus::Graphics g(candidate.dc);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    Gdiplus::ImageAttributes attr;UiResourceTint(attr,ink);
                    int x=glyph*96;
                    std::unique_ptr<Gdiplus::Bitmap> crop;
                    if(mode>=8) {
                        const int pad=mode==8?1:mode==9?8:96;
                        int left=std::max(0,x-pad),right=std::min(2592,x+96+pad);
                        crop.reset(UiResource(2012).bitmap->Clone(left,0,right-left,96,PixelFormat32bppARGB));x-=left;
                    }
                    if(g.DrawImage(crop?crop.get():atlas->bitmap.get(),Gdiplus::Rect(r.left,r.top,size,size),x,0,96,96,Gdiplus::UnitPixel,&attr)!=Gdiplus::Ok){supported=false;continue;}
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
