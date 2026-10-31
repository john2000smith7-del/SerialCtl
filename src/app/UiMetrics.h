#pragma once
#include <windows.h>
namespace serialctl {
// Shared native view metrics; values and scaling policy are unchanged.
namespace Ui {
inline int Dpi=96;
inline int Scale(int value) { return MulDiv(value,Dpi,96); }
inline int Radius = 10;
inline int CardRadius = 12;
inline int Space = 8;
inline int Gap = 12;
inline int OverlayScrollLaneWidth = 16;
inline int OverlayScrollThumbWidth = 5;
inline int TerminalPadding = 8;
inline int TerminalTimestampWidth = 112;
inline int Section = 16;
inline int ToolbarHeight = 72;
inline constexpr int ToolbarButtonWidths[] = {89, 88, 98, 118, 92, 88, 80};
inline constexpr int CompactIconButtonSize = 28;
inline constexpr int CompactGlyphSize = 16;
inline constexpr int CompactIconRadius = 14;
inline constexpr int UtilityGlyphSize = 16;
inline int StandardHeight = 36;
inline int CompactHeight = 32;
inline int SegmentHeight = 32;
inline int SegmentWidth = 70;
inline int PanelPadding = 16;
inline int IconButtonSize = 28;
inline int MinLeftWidth = 190;
inline int MaxLeftWidth = 190;
inline constexpr int MaxResizableLeftWidth96 = 280;
inline constexpr int StatusViewportHeight96 = 72;
inline constexpr int StatusHeaderHeight96 = 24;
inline int MinRightWidth = 260;
inline int MaxRightWidth = 650;
inline int CollapsedRightWidth = 48;
inline int MinCenterWidth = 400;
inline int SftpTableRowHeight = 36;
inline int SftpTransferRowHeight = 48;
inline int SftpTransferDrawerHeight = 144;
inline int TerminalFontMinimumHeight = 10;
inline int TerminalFontMaximumHeight = 32;
inline constexpr wchar_t HoverProperty[] = L"SerialCtl.ButtonHover";
inline constexpr wchar_t FieldFrameProperty[] = L"SerialCtl.FieldFrame";
inline constexpr wchar_t FieldEditProperty[] = L"SerialCtl.FieldEdit";
}
} // namespace serialctl
