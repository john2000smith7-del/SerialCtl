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

namespace serialctl {
// Editable SVG masters are rasterized at build-authoring time, embedded, then
// tinted and nine-sliced here. No SVG framework or filesystem assets at runtime.
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
struct UiSurfaceAsset {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0, index = 0, corner = 0;
    COLORREF fill = 0, border = 0;
    size_t Bytes() const {
        return static_cast<size_t>(width) * height * 4;
    }
    ~UiSurfaceAsset() {
        if (previous)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }
};
inline bool UiResourceBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    auto &mask = UiResource(2010), &outline = UiResource(2011);
    if (!mask.bitmap || !outline.bitmap || radius < 1)
        return false;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        static_cast<size_t>(width) * height > 16 * 1024 * 1024)
        return false;
    const int dpi = std::max(96, GetDeviceCaps(dc, LOGPIXELSX));
    const int wanted = MulDiv(radius, 96, dpi), radii[] = {2, 3, 4, 8, 10, 12};
    int index = 0;
    for (int i = 1; i < 6; ++i)
        if (std::abs(radii[i] - wanted) < std::abs(radii[index] - wanted))
            index = i;
    const int corner = std::min({MulDiv(radii[index] + 2, dpi, 96), width / 2, height / 2});
    if (corner <= 0)
        return false;
    // Cache only a bounded set of fully composited resource surfaces. Native
    // AlphaBlend publishes once, respecting the DC's viewport and complex clip;
    // independently scaling nine GDI+ images introduced seams on partial paints.
    static auto *cache = new std::list<std::unique_ptr<UiSurfaceAsset>>;
    auto found = std::find_if(cache->begin(), cache->end(), [&](const auto &a) {
        return a->width == width && a->height == height && a->index == index && a->corner == corner &&
               a->fill == fill && a->border == border;
    });
    if (found != cache->end())
        cache->splice(cache->begin(), *cache, found);
    else {
        auto asset = std::make_unique<UiSurfaceAsset>();
        asset->width = width;
        asset->height = height;
        asset->index = index;
        asset->corner = corner;
        asset->fill = fill;
        asset->border = border;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        asset->dc = CreateCompatibleDC(dc);
        asset->bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!asset->dc || !asset->bitmap || !pixels)
            return false;
        asset->previous = SelectObject(asset->dc, asset->bitmap);
        if (!asset->previous || asset->previous == HGDI_ERROR) {
            asset->previous = nullptr;
            return false;
        }
        const int sourceCorner = (radii[index] + 2) * 4;
        auto coordinate = [&](int pos, int size) {
            if (pos < corner)
                return (pos + .5) * sourceCorner / corner - .5;
            if (pos >= size - corner)
                return 256 - sourceCorner + (pos - size + corner + .5) * sourceCorner / corner - .5;
            return sourceCorner + (pos - corner + .5) * (256 - 2 * sourceCorner) / (size - 2 * corner) - .5;
        };
        std::vector<double> xs(width), ys(height);
        for (int x = 0; x < width; ++x)
            xs[x] = coordinate(x, width);
        for (int y = 0; y < height; ++y)
            ys[y] = coordinate(y, height);
        auto alpha = [&](const UiResourceBitmap &image, double x, double y) {
            x = std::clamp(x, 0., 255.);
            y = std::clamp(y, 0., 255.);
            const int ix = static_cast<int>(x), iy = static_cast<int>(y), nx = std::min(255, ix + 1),
                      ny = std::min(255, iy + 1);
            auto sample = [&](int sx, int sy) {
                return image.alpha[static_cast<size_t>(sy) * image.width + index * 256 + sx];
            };
            const double fx = x - ix, fy = y - iy;
            return static_cast<unsigned>((sample(ix, iy) * (1 - fx) * (1 - fy) + sample(nx, iy) * fx * (1 - fy) +
                                          sample(ix, ny) * (1 - fx) * fy + sample(nx, ny) * fx * fy) +
                                         .5);
        };
        if (mask.alpha.empty() || outline.alpha.empty())
            return false;
        auto *output = static_cast<DWORD *>(pixels);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const unsigned a = alpha(mask, xs[x], ys[y]), b = alpha(outline, xs[x], ys[y]);
                auto channel = [&](unsigned f, unsigned o) {
                    return (o * b * 255 + f * a * (255 - b) + 32512) / 65025;
                };
                const unsigned opacity = b + (a * (255 - b) + 127) / 255;
                output[static_cast<size_t>(y) * width + x] =
                    (opacity << 24) | (channel(GetRValue(fill), GetRValue(border)) << 16) |
                    (channel(GetGValue(fill), GetGValue(border)) << 8) | channel(GetBValue(fill), GetBValue(border));
            }
        size_t total = asset->Bytes();
        for (const auto &entry : *cache)
            total += entry->Bytes();
        while (!cache->empty() && (cache->size() >= 96 || total > 16 * 1024 * 1024)) {
            total -= cache->back()->Bytes();
            cache->pop_back();
        }
        cache->push_front(std::move(asset));
    }
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return AlphaBlend(dc, rect.left, rect.top, width, height, cache->front()->dc, 0, 0, width, height, blend) != FALSE;
}
} // namespace serialctl
