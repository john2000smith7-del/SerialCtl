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
} // namespace
int main() {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return 1;
    TestItem({13, 33, 151, 91}, false);
    TestItem({-5, -9, 133, 49}, false);
    TestItem({13, 33, 131, 91}, true);
    TestFallback();
    // Warm the APIs before checking the repeated buffer resource lifetime.
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
