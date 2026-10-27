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
inline bool UiResourceGlyph(HDC dc, const RECT &rect, COLORREF color, int index) {
    auto &resource = UiResource(2012);
    if (!resource.bitmap || index < 0 || index >= 22)
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
// Resource glyphs keep their embedded artwork; control geometry is rasterized
// at the destination DPI to retain subpixel coverage instead of point sampling.
inline bool UiResourceBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    return UiRoundedSurface(dc, rect, fill, border, radius, dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96);
}
} // namespace serialctl
