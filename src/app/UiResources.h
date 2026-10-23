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
#include <memory>

namespace serialctl {
// Editable SVG masters are rasterized at build-authoring time, embedded, then
// tinted and nine-sliced here. No SVG framework or filesystem assets at runtime.
struct UiResourceBitmap {
    IStream *stream = nullptr;
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
    matrix.m[4][0] = GetRValue(color) / 255.f;
    matrix.m[4][1] = GetGValue(color) / 255.f;
    matrix.m[4][2] = GetBValue(color) / 255.f;
    attributes.SetColorMatrix(&matrix);
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
}
inline bool UiResourceGlyph(HDC dc, const RECT &rect, COLORREF color, int index) {
    auto &resource = UiResource(2012);
    if (!resource.bitmap || index < 0 || index >= 19)
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
inline bool UiResourceBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    auto &mask = UiResource(2010);
    auto &outline = UiResource(2011);
    if (!mask.bitmap || !outline.bitmap || radius < 1)
        return false;
    const int dpi = std::max(96, GetDeviceCaps(dc, LOGPIXELSX));
    const int wanted = MulDiv(radius, 96, dpi), radii[] = {2, 3, 4, 8, 10, 12};
    int index = 0;
    for (int i = 1; i < 6; ++i)
        if (std::abs(radii[i] - wanted) < std::abs(radii[index] - wanted))
            index = i;
    const int sourceCorner = (radii[index] + 2) * 4;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const int corner = std::min({MulDiv(radii[index] + 2, dpi, 96), width / 2, height / 2});
    if (corner <= 0)
        return false;
    const int x[] = {rect.left, rect.left + corner, rect.right - corner, rect.right};
    const int y[] = {rect.top, rect.top + corner, rect.bottom - corner, rect.bottom};
    const int s[] = {0, sourceCorner, 256 - sourceCorner, 256};
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    auto layer = [&](Gdiplus::Bitmap *image, COLORREF color) {
        Gdiplus::ImageAttributes attributes;
        UiResourceTint(attributes, color);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                if (x[col + 1] > x[col] && y[row + 1] > y[row])
                    graphics.DrawImage(image, Gdiplus::Rect(x[col], y[row], x[col + 1] - x[col], y[row + 1] - y[row]),
                                       index * 256 + s[col], s[row], s[col + 1] - s[col], s[row + 1] - s[row],
                                       Gdiplus::UnitPixel, &attributes);
    };
    layer(mask.bitmap.get(), fill);
    layer(outline.bitmap.get(), border);
    return true;
}
} // namespace serialctl
