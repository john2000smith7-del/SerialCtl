#include "UiStyle.h"
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void Expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}
struct Surface {
    static constexpr int Width = 160, Height = 128;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void *pixels = nullptr;
    Surface() {
        if (!dc)
            return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Width;
        info.bmiHeader.biHeight = -Height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels)
            return;
        previous = SelectObject(dc, bitmap);
        if (!Valid())
            return;
        RECT bounds{0, 0, Width, Height};
        HBRUSH brush = CreateSolidBrush(RGB(36, 36, 38));
        FillRect(dc, &bounds, brush);
        DeleteObject(brush);
        SelectObject(dc, GetStockObject(SYSTEM_FIXED_FONT));
        SetTextColor(dc, RGB(245, 245, 247));
        SetBkMode(dc, TRANSPARENT);
    }
    bool Valid() const {
        return dc && bitmap && pixels && previous && previous != HGDI_ERROR;
    }
    ~Surface() {
        if (previous && previous != HGDI_ERROR)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }
    std::vector<DWORD> Pixels() const {
        if (!Valid())
            return {};
        GdiFlush();
        const auto *data = static_cast<const DWORD *>(pixels);
        std::vector<DWORD> result(data, data + Width * Height);
        // DIB alpha is unspecified for GDI; compare the visible RGB channels.
        for (auto &pixel : result)
            pixel &= 0x00ffffff;
        return result;
    }
};
void Paint(HDC dc, const RECT &bounds) {
    HBRUSH background = CreateSolidBrush(RGB(36, 36, 38));
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    RECT card = bounds;
    InflateRect(&card, -2, -2);
    serialctl::UiBox(dc, card, RGB(44, 44, 46), RGB(10, 132, 255), 10);
    RECT text = card;
    text.left += 7;
    text.right -= 7;
    DrawTextW(dc, L"Command / 设置", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    POINT points[]{{bounds.left + 3, bounds.top + 6}, {bounds.left + 8, bounds.top + 11},
                   {bounds.left + 14, bounds.top + 3}};
    serialctl::UiPolyline(dc, points, 3, RGB(10, 132, 255), 2);
}
void Configure(HDC dc, bool offset) {
    if (offset) {
        SetViewportOrgEx(dc, 5, 7, nullptr);
        SetWindowOrgEx(dc, -3, -6, nullptr);
    }
    // A native owner-draw update can have a nonrectangular visibility clip.
    HRGN visible = CreateRectRgn(9, 8, 145, 116);
    HRGN hole = CreateRectRgn(62, 20, 71, 104);
    CombineRgn(visible, visible, hole, RGN_DIFF);
    SelectClipRgn(dc, visible);
    DeleteObject(hole);
    DeleteObject(visible);
}
void TestItem(RECT bounds, bool offset) {
    Surface direct, buffered;
    Expect(direct.Valid() && buffered.Valid(), "test DIB surfaces are available");
    if (!direct.Valid() || !buffered.Valid())
        return;
    Configure(direct.dc, offset);
    Configure(buffered.dc, offset);
    const auto before = buffered.Pixels();
    const auto font = GetCurrentObject(buffered.dc, OBJ_FONT);
    const auto textColor = GetTextColor(buffered.dc);
    Paint(direct.dc, bounds);
    {
        serialctl::UiBufferedPaint paint(buffered.dc, bounds);
        Expect(paint.Dc() != buffered.dc, "normal pixel owner-draw item uses offscreen storage");
        Expect(GetCurrentObject(paint.Dc(), OBJ_FONT) == font, "buffer retains the native selected font");
        Paint(paint.Dc(), bounds);
        Expect(before == buffered.Pixels(), "partial drawing must not change the destination before commit");
        SetTextColor(paint.Dc(), RGB(1, 2, 3));
    }
    Expect(direct.Pixels() == buffered.Pixels(), "GDI/GDI+ output retains item coordinates and target visibility clip");
    Expect(GetTextColor(buffered.dc) == textColor, "painting preserves destination text state");
    Expect(GetCurrentObject(buffered.dc, OBJ_FONT) == font, "painting preserves destination font");
}
void TestFallback() {
    Surface surface;
    Expect(surface.Valid(), "fallback test DIB is available");
    if (!surface.Valid())
        return;
    auto color = GetTextColor(surface.dc);
    {
        serialctl::UiBufferedPaint paint(surface.dc, RECT{0, 0, 0, 0});
        Expect(paint.Dc() == surface.dc, "empty paint bounds safely keep the original DC");
        SetTextColor(paint.Dc(), RGB(1, 2, 3));
    }
    Expect(GetTextColor(surface.dc) == color, "fallback also restores destination state");
    {
        serialctl::UiBufferedPaint paint(surface.dc, RECT{LONG_MIN, 0, LONG_MAX, 1});
        Expect(paint.Dc() == surface.dc, "overflow-sized rectangle safely falls back");
    }
    SetMapMode(surface.dc, MM_ANISOTROPIC);
    {
        serialctl::UiBufferedPaint paint(surface.dc, RECT{0, 0, 30, 20});
        Expect(paint.Dc() == surface.dc, "unusual mappings retain original drawing semantics");
    }
}
void TestCurveCoverage() {
    constexpr double pi = 3.14159265358979323846;
    for (int dpi : {96, 120, 144, 192}) {
        for (bool outline : {false, true}) {
            Surface surface;
            RECT rect{9, 11, 149, 117};
            const int radius = MulDiv(outline ? 6 : 10, dpi, 96);
            const double stroke = static_cast<double>(dpi) / 96;
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(surface.dc, &rect, black); DeleteObject(black);
            Expect(serialctl::UiRoundedSurface(surface.dc, rect, RGB(255, 255, 255), RGB(255, 255, 255),
                                               radius, dpi, outline), "area-coverage surface is available");
            const auto pixels = surface.Pixels();
            const int width = rect.right - rect.left, height = rect.bottom - rect.top;
            double area = 0;
            int partial = 0, allPartial = 0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto value = pixels[(rect.top + y) * Surface::Width + rect.left + x] & 255;
                    area += static_cast<double>(value) / 255;
                    if (value > 0 && value < 255) ++allPartial;
                    if (x < radius && y < radius && value > 0 && value < 255) ++partial;
                    Expect(value == (pixels[(rect.top + y) * Surface::Width + rect.right - 1 - x] & 255),
                           "curve coverage is exactly symmetric horizontally");
                    Expect(value == (pixels[(rect.bottom - 1 - y) * Surface::Width + rect.left + x] & 255),
                           "curve coverage is exactly symmetric vertically");
                }
            }
            double expected = width * height - (4 - pi) * radius * radius;
            if (outline)
                expected -= (width - 2 * stroke) * (height - 2 * stroke) -
                            (4 - pi) * (radius - stroke) * (radius - stroke);
            // One half alpha LSB per partially covered pixel, plus the
            // numerical integration budget. Fractional straight strokes may
            // contain hundreds of partial pixels at 125%/150% DPI.
            const double tolerance = .08 + allPartial / 510.;
            if (std::abs(area - expected) >= tolerance)
                std::cerr << "dpi=" << dpi << " outline=" << outline << " area=" << area
                          << " expected=" << expected << " tolerance=" << tolerance << '\n';
            Expect(std::abs(area - expected) < tolerance, "pixel coverage preserves the analytic rounded-rectangle area");
            Expect(partial >= radius, "each corner retains partial coverage instead of binary stairs");
        }
    }
    Surface pattern;
    GdiFlush();
    auto patternData = static_cast<DWORD *>(pattern.pixels);
    for (int y = 30; y < 70; ++y)
        for (int x = 24; x < 120; ++x)
            patternData[y * Surface::Width + x] = (x + y) % 2 ? 0x00ff3300 : 0x000099ee;
    const auto original = pattern.Pixels();
    serialctl::UiFocusOutline(pattern.dc, RECT{10, 10, 140, 100}, 6, 96, RGB(101, 149, 255));
    const auto focused = pattern.Pixels();
    Expect(focused[50 * Surface::Width + 60] == original[50 * Surface::Width + 60],
           "focus rendering must not fill or blur the control interior");
    Expect(focused[10 * Surface::Width + 10] == original[10 * Surface::Width + 10],
           "focus rendering must preserve the exterior corner");
    bool patternIntact = true;
    for (int y = 30; y < 70; ++y)
        for (int x = 24; x < 120; ++x)
            patternIntact &= focused[y * Surface::Width + x] == original[y * Surface::Width + x];
    Expect(patternIntact, "focus coverage must preserve high-frequency interior detail without blurring");
    for (int size : {1, 2, 3, 7}) {
        Surface tiny;
        RECT rect{2, 2, 2 + size, 2 + size};
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(tiny.dc, &rect, black); DeleteObject(black);
        serialctl::UiRoundedSurface(tiny.dc, rect, RGB(255, 255, 255), RGB(255, 255, 255), 12, 96);
        const auto pixels = tiny.Pixels();
        double area = 0;
        for (int y = rect.top; y < rect.bottom; ++y)
            for (int x = rect.left; x < rect.right; ++x)
                area += (pixels[y * Surface::Width + x] & 255) / 255.;
        Expect(std::abs(area - pi * size * size / 4) < .05,
               "tiny controls clamp their radius and retain circular pixel coverage");
    }
}
} // namespace
int main() {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return 1;
    Expect(serialctl::UiResource(2010).bitmap && serialctl::UiResource(2011).bitmap &&
               serialctl::UiResource(2012).bitmap,
           "Embedded card masks and utility icon atlas are present");
    {
        Surface surface;
        const auto original = GetPixel(surface.dc, 10, 10);
        serialctl::UiBox(surface.dc, {10, 10, 110, 90}, RGB(31, 79, 216), RGB(31, 79, 216), 12);
        Expect(GetPixel(surface.dc, 10, 10) == original, "Resource card corners preserve the surrounding background");
        Expect(GetPixel(surface.dc, 60, 50) == RGB(31, 79, 216), "Resource tint must match the shared accent exactly");
    }
    TestItem({13, 33, 151, 91}, false);
    TestItem({-5, -9, 133, 49}, false);
    TestItem({13, 33, 131, 91}, true);
    TestFallback();
    TestCurveCoverage();
    // Warm the APIs before checking the repeated buffer resource lifetime.
    {
        Surface warm;
        Paint(warm.dc, RECT{0, 0, 160, 128});
    }
    const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 100; ++i) {
        Surface surface;
        if (!surface.Valid()) {
            Expect(false, "resource-lifetime test DIB is available");
            break;
        }
        serialctl::UiBufferedPaint paint(surface.dc, RECT{0, 0, 160, 128});
        Paint(paint.Dc(), RECT{0, 0, 160, 128});
    }
    GdiFlush();
    Expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before,
           "repeated item drawing releases every buffer DC and bitmap");
    Gdiplus::GdiplusShutdown(token);
    if (!failures)
        std::cout << "UI buffered paint tests passed\n";
    return failures ? 1 : 0;
}
