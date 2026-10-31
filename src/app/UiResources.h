#pragma once
#include <algorithm>
// clang-format off
#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
// clang-format on
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <vector>
#include "UiRoundedSurface.h"
#ifdef SERIALCTL_DIALOG_PROFILE
#include "../../tests/performance/DialogTimingHooks.h"
#endif

namespace serialctl {
// Editable SVG artwork is embedded for icons. Rounded control geometry uses
// destination-pixel coverage; no SVG framework or filesystem assets at runtime.
struct UiResourceBitmap {
    IStream *stream = nullptr;
    std::vector<unsigned char> alpha;
    unsigned width = 0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
    explicit UiResourceBitmap(int id) {
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource)
            return;
        const auto length = SizeofResource(module, resource);
        const auto source = LockResource(LoadResource(module, resource));
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, length);
        if (!source || !memory) {
            if (memory)
                GlobalFree(memory);
            return;
        }
        auto destination = GlobalLock(memory);
        if (!destination) {
            GlobalFree(memory);
            return;
        }
        std::memcpy(destination, source, length);
        GlobalUnlock(memory);
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
            GlobalFree(memory);
            return;
        }
        bitmap.reset(Gdiplus::Bitmap::FromStream(stream));
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
            bitmap.reset();
        if (bitmap && (id == 2010 || id == 2011)) {
            width = bitmap->GetWidth();
            const unsigned height = bitmap->GetHeight();
            Gdiplus::BitmapData data{};
            const Gdiplus::Rect bounds(0, 0, width, height);
            if (bitmap->LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                alpha.resize(static_cast<size_t>(width) * height);
                for (unsigned y = 0; y < height; ++y) {
                    auto row = static_cast<const unsigned char *>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
                    for (unsigned x = 0; x < width; ++x)
                        alpha[static_cast<size_t>(y) * width + x] = row[x * 4 + 3];
                }
                bitmap->UnlockBits(&data);
            }
        }
    }
};
inline UiResourceBitmap &UiResource(int id) {
    // Fixed process-lifetime resource set. Bitmap destruction must not run
    // after App.cpp shuts down GDI+, and painting never creates a growing cache.
    static auto *fill = new UiResourceBitmap(2010);
    static auto *stroke = new UiResourceBitmap(2011);
    static auto *icons = new UiResourceBitmap(2012);
    return id == 2010 ? *fill : id == 2011 ? *stroke : *icons;
}
inline void UiResourceTint(Gdiplus::ImageAttributes &attributes, COLORREF color) {
    Gdiplus::ColorMatrix matrix{};
    matrix.m[3][3] = matrix.m[4][4] = 1.f;
    matrix.m[4][0] = std::min(255.f, GetRValue(color) + .5f) / 255.f;
    matrix.m[4][1] = std::min(255.f, GetGValue(color) + .5f) / 255.f;
    matrix.m[4][2] = std::min(255.f, GetBValue(color) + .5f) / 255.f;
    attributes.SetColorMatrix(&matrix);
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
}
inline bool UiResourceGlyphOriginal(HDC dc, const RECT &rect, COLORREF color, int index) {
    auto &resource = UiResource(2012);
    if (!resource.bitmap || index < 0 || index >= 27)
        return false;
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::ImageAttributes attributes;
    UiResourceTint(attributes, color);
    graphics.DrawImage(resource.bitmap.get(),
                       Gdiplus::Rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top), index * 96,
                       0, 96, 96, Gdiplus::UnitPixel, &attributes);
    return true;
}
struct UiGlyphSurface {
    HDC dc=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ previous=nullptr;
    int width=0,height=0,index=0; COLORREF color=0;
    ~UiGlyphSurface() { if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc); }
    size_t Bytes()const{return static_cast<size_t>(width)*height*4;}
};
inline bool UiResourceGlyphCached(HDC dc,const RECT &rect,COLORREF color,int index) {
    const auto wideWidth=static_cast<long long>(rect.right)-rect.left;
    const auto wideHeight=static_cast<long long>(rect.bottom)-rect.top;
    if(!dc || wideWidth<=0 || wideHeight<=0 || wideWidth>512 || wideHeight>512) return false;
    auto &resource=UiResource(2012);
    if(!resource.bitmap || index<0 || index>=27)return false;
    const int width=static_cast<int>(wideWidth),height=static_cast<int>(wideHeight);
    static thread_local std::list<std::unique_ptr<UiGlyphSurface>> cache;
    auto found=std::find_if(cache.begin(),cache.end(),[&](const auto &a){return a->width==width && a->height==height && a->index==index && a->color==color;});
    if(found!=cache.end())cache.splice(cache.begin(),cache,found);
    else {
        auto asset=std::make_unique<UiGlyphSurface>();asset->width=width;asset->height=height;asset->index=index;asset->color=color;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void *pixels=nullptr;asset->dc=CreateCompatibleDC(dc);asset->bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!asset->dc || !asset->bitmap || !pixels)return false;
        asset->previous=SelectObject(asset->dc,asset->bitmap);
        if(!asset->previous || asset->previous==HGDI_ERROR){asset->previous=nullptr;return false;}
        std::memset(pixels,0,asset->Bytes());
        {
            Gdiplus::Bitmap surface(width,height,width*4,PixelFormat32bppPARGB,static_cast<BYTE *>(pixels));
            Gdiplus::Graphics graphics(&surface);graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::ImageAttributes attributes;UiResourceTint(attributes,color);
            if(graphics.DrawImage(resource.bitmap.get(),Gdiplus::Rect(0,0,width,height),index*96,0,96,96,Gdiplus::UnitPixel,&attributes)!=Gdiplus::Ok)return false;
        }
        size_t total=asset->Bytes();for(const auto &a:cache)total+=a->Bytes();
        while(!cache.empty() && (cache.size()>=192 || total>4*1024*1024)){total-=cache.back()->Bytes();cache.pop_back();}
        cache.push_front(std::move(asset));
    }
    const BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    return AlphaBlend(dc,rect.left,rect.top,width,height,cache.front()->dc,0,0,width,height,blend)!=FALSE;
}
inline bool UiResourceGlyph(HDC dc,const RECT &rect,COLORREF color,int index) {
#ifdef SERIALCTL_DIALOG_PROFILE
    dialog_timing::Scope profile(dialog_timing::Glyph);
    if(GetEnvironmentVariableW(L"SERIALCTL_PROFILE_GLYPH_CACHE",nullptr,0)!=0 && UiResourceGlyphCached(dc,rect,color,index))return true;
#endif
    return UiResourceGlyphOriginal(dc,rect,color,index);
}
// Resource glyphs keep their embedded artwork; control geometry is rasterized
// at the destination DPI to retain subpixel coverage instead of point sampling.
inline bool UiResourceBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    return UiRoundedSurface(dc, rect, fill, border, radius, dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96);
}
} // namespace serialctl
