#pragma once
#include <algorithm>
// clang-format off
#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
// clang-format on
#include <array>
#include <new>
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
inline bool UiResourceGlyphDirect(HDC dc, const RECT &rect, COLORREF color, int index) {
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
struct UiGlyphSource {
    int origin = 0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
};
inline const UiGlyphSource *UiGlyphSourceFor(UiResourceBitmap &source, int index) {
    // GDI+ applies a color matrix to the complete source bitmap even when a
    // 96px source rectangle selects one icon. Retain guard pixels for bicubic
    // filtering, but avoid transforming the other 26 icons on every paint.
    // This fixed artwork cache needs about 1.2MiB, independent of DPI, ink,
    // theme, window count and repaint count. It is used on the GUI thread.
    static auto *cache = new std::array<UiGlyphSource, 27>;
    auto &entry = (*cache)[static_cast<size_t>(index)];
    if (!entry.bitmap) {
        // Keep the source origin aligned for the common 16/24/32/48px
        // scales (96/size denominators divide 12), reducing GDI+ phase rounding.
        constexpr int guard = 12;
        const int left = std::max(0, index * 96 - guard);
        const int right = std::min(2592, (index + 1) * 96 + guard);
        std::unique_ptr<Gdiplus::Bitmap> bitmap(source.bitmap->Clone(left, 0, right - left, 96,
                                                                   PixelFormat32bppARGB));
        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
        entry.origin = index * 96 - left;
        entry.bitmap = std::move(bitmap);
    }
    return &entry;
}
inline bool UiResourceGlyph(HDC dc, const RECT &rect, COLORREF color, int index) {
    auto &resource = UiResource(2012);
    if (!resource.bitmap || index < 0 || index >= 27) return false;
    try {
        if (const auto source = UiGlyphSourceFor(resource, index)) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::ImageAttributes attributes;
            UiResourceTint(attributes, color);
            return graphics.DrawImage(source->bitmap.get(),
                Gdiplus::Rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top),
                source->origin, 0, 96, 96, Gdiplus::UnitPixel, &attributes) == Gdiplus::Ok;
        }
    } catch (const std::bad_alloc &) {
        // Optional cache allocation must not turn a paint into a process crash.
    }
    return UiResourceGlyphDirect(dc, rect, color, index);
}
// Resource glyphs keep their embedded artwork; control geometry is rasterized
// at the destination DPI to retain subpixel coverage instead of point sampling.
inline bool UiResourceBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    return UiRoundedSurface(dc, rect, fill, border, radius, dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96);
}
} // namespace serialctl
