#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <vector>
#include <windows.h>

namespace serialctl {
namespace rounded_detail {
// Integrate horizontal pixel coverage at 32 vertical subpixel positions.
// Only curved/edge pixels need this work; the opaque middle stays a bulk fill.
constexpr int Samples = 32;
struct Row {
    std::array<double, Samples> left{}, right{};
    Row(double width, double height, double radius, double inset, int y) {
        const double l = inset, r = width - inset, t = inset, b = height - inset;
        radius = std::min({radius, (r - l) / 2, (b - t) / 2});
        for (int i = 0; i < Samples; ++i) {
            const double v = y + (i + .5) / Samples;
            if (r <= l || b <= t || v <= t || v >= b) continue;
            const double dy = std::max({t + radius - v, v - (b - radius), 0.});
            const double curve = dy > 0 ? radius - std::sqrt(std::max(0., radius * radius - dy * dy)) : 0.;
            left[i] = l + curve;
            right[i] = r - curve;
        }
    }
    double Coverage(int x) const {
        double area = 0;
        for (int i = 0; i < Samples; ++i)
            area += std::clamp(std::min(x + 1., right[i]) - std::max(static_cast<double>(x), left[i]), 0., 1.);
        return std::clamp(area / Samples, 0., 1.);
    }
};
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0, dpi = 0;
    double radius = 0;
    bool outline = false;
    COLORREF fill = 0, border = 0;
    size_t Bytes() const { return static_cast<size_t>(width) * height * sizeof(DWORD); }
    ~Surface() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};
inline DWORD Pixel(double outside, double inside, COLORREF fill, COLORREF border, bool outline) {
    inside = std::min(inside, outside);
    const double ring = outside - inside;
    if (outline) inside = 0;
    const auto channel = [&](unsigned f, unsigned b) { return static_cast<DWORD>(std::lround(f * inside + b * ring)); };
    const auto opacity = static_cast<DWORD>(std::lround((inside + ring) * 255));
    return (opacity << 24) | (channel(GetRValue(fill), GetRValue(border)) << 16) |
           (channel(GetGValue(fill), GetGValue(border)) << 8) | channel(GetBValue(fill), GetBValue(border));
}
} // namespace rounded_detail

inline bool UiRoundedSurface(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius, int dpi,
                             bool outline = false) {
    const auto wideWidth = static_cast<std::int64_t>(rect.right) - rect.left;
    const auto wideHeight = static_cast<std::int64_t>(rect.bottom) - rect.top;
    if (!dc || wideWidth <= 0 || wideHeight <= 0 || wideWidth > 16384 || wideHeight > 16384 ||
        wideWidth * wideHeight > 16 * 1024 * 1024) return false;
    const int width = static_cast<int>(wideWidth), height = static_cast<int>(wideHeight);
    dpi = std::max(96, dpi);
    const double r = std::min({static_cast<double>(std::max(0, radius)), width / 2., height / 2.});
    const double stroke = static_cast<double>(dpi) / 96;
    if (outline) fill = 0;
    // Keep process-lifetime GDI ownership independent of static destruction order.
    // Normal surfaces total at most 16 MiB/96 entries; one larger surface may
    // occupy the cache alone, bounded by the 16M-pixel input limit above.
    static auto *cache = new std::list<std::unique_ptr<rounded_detail::Surface>>;
    auto found = std::find_if(cache->begin(), cache->end(), [&](const auto &a) {
        return a->width == width && a->height == height && a->dpi == dpi && a->radius == r &&
               a->outline == outline && a->fill == fill && a->border == border;
    });
    if (found != cache->end()) cache->splice(cache->begin(), *cache, found);
    else {
        auto asset = std::make_unique<rounded_detail::Surface>();
        asset->width = width; asset->height = height; asset->dpi = dpi; asset->radius = r;
        asset->outline = outline; asset->fill = fill; asset->border = border;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void *pixels = nullptr;
        asset->dc = CreateCompatibleDC(dc);
        asset->bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!asset->dc || !asset->bitmap || !pixels) return false;
        asset->previous = SelectObject(asset->dc, asset->bitmap);
        if (!asset->previous || asset->previous == HGDI_ERROR) { asset->previous = nullptr; return false; }
        auto output = static_cast<DWORD *>(pixels);
        const int corner = std::min(width / 2, static_cast<int>(std::ceil(std::max(r, stroke))));
        const int curvedRows = static_cast<int>(std::ceil(std::max(r, stroke)));
        std::vector<DWORD> body;
        for (int y = 0; y < (height + 1) / 2; ++y) {
            auto row = output + static_cast<size_t>(y) * width;
            if (y >= curvedRows && !body.empty()) std::memcpy(row, body.data(), width * sizeof(DWORD));
            else {
                const rounded_detail::Row outside(width, height, r, 0, y);
                const rounded_detail::Row inside(width, height, std::max(0., r - stroke), stroke, y);
                const auto pixel = [&](int x) {
                    return rounded_detail::Pixel(outside.Coverage(x), inside.Coverage(x), fill, border, outline);
                };
                for (int x = 0; x < corner; ++x) row[x] = row[width - 1 - x] = pixel(x);
                if (width > corner * 2) std::fill(row + corner, row + width - corner, pixel(corner));
                if (y >= curvedRows) body.assign(row, row + width);
            }
            if (y != height - 1 - y)
                std::memcpy(output + static_cast<size_t>(height - 1 - y) * width, row, width * sizeof(DWORD));
        }
        size_t total = asset->Bytes();
        for (const auto &a : *cache) total += a->Bytes();
        while (!cache->empty() && (cache->size() >= 96 || total > 16 * 1024 * 1024)) {
            total -= cache->back()->Bytes(); cache->pop_back();
        }
        cache->push_front(std::move(asset));
    }
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return AlphaBlend(dc, rect.left, rect.top, width, height, cache->front()->dc, 0, 0, width, height, blend) != FALSE;
}
} // namespace serialctl
