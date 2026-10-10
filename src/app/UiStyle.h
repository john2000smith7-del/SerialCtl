#pragma once
#include <windows.h>
namespace serialctl
{
struct UiColors
{
    COLORREF surface, raised, field, text, muted, border, accent, danger;
};
inline UiColors UiTheme(bool dark)
{
    return dark ? UiColors{RGB(36, 36, 38),    RGB(44, 44, 46),    RGB(20, 20, 22),
                           RGB(245, 245, 247), RGB(152, 152, 157), RGB(58, 58, 60),
                           RGB(10, 132, 255),  RGB(255, 69, 58)}
                : UiColors{RGB(255, 255, 255), RGB(242, 242, 247), RGB(255, 255, 255),
                           RGB(29, 29, 31),    RGB(110, 110, 115), RGB(210, 210, 215),
                           RGB(0, 122, 255),   RGB(255, 59, 48)};
}
inline void UiBox(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}
} // namespace serialctl
