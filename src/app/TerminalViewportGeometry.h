#pragma once
#include "UiStyle.h"
#include "UiMetrics.h"
#include <algorithm>
namespace serialctl {
struct TerminalScrollMetrics {
    bool visible = false;
    RECT track{};
    RECT thumb{};
    int maximumOffset = 0;
};

inline TerminalScrollMetrics GetTerminalScrollMetrics(
    HWND terminal, size_t totalLines, int visibleRows, int scrollOffset) {
    TerminalScrollMetrics metrics;
    RECT client{};
    GetClientRect(terminal, &client);
    visibleRows = std::max(1, visibleRows);
    metrics.maximumOffset = std::max(0, static_cast<int>(totalLines) - visibleRows);
    if (metrics.maximumOffset <= 0) return metrics;
    metrics.visible = true;
    metrics.track = {client.right - Ui::OverlayScrollLaneWidth, 8, client.right, client.bottom - 8};
    const int trackHeight = std::max(1,
        static_cast<int>(metrics.track.bottom - metrics.track.top));
    const int thumbHeight = std::max(28,
        trackHeight * std::min<int>(visibleRows, static_cast<int>(totalLines)) /
            std::max<int>(1, static_cast<int>(totalLines)));
    const int travel = std::max(1, trackHeight - thumbHeight);
    // scrollOffset is measured upward from the newest line. The thumb is therefore
    // at the bottom for offset zero and moves toward the top as the offset grows.
    const int thumbTop = metrics.track.top + travel -
        scrollOffset * travel / std::max(1, metrics.maximumOffset);
    const int left = client.right - (Ui::OverlayScrollLaneWidth + Ui::OverlayScrollThumbWidth) / 2;
    metrics.thumb = {left, thumbTop, left + Ui::OverlayScrollThumbWidth, thumbTop + thumbHeight};
    return metrics;
}

struct TerminalHorizontalScrollMetrics {
    bool visible = false;
    RECT track{};
    RECT thumb{};
    int maximumOffset = 0;
};

inline TerminalHorizontalScrollMetrics GetTerminalHorizontalScrollMetrics(
    HWND terminal, int textLeft, size_t totalColumns, int visibleColumns, int scrollOffset) {
    TerminalHorizontalScrollMetrics metrics;
    RECT client{};
    GetClientRect(terminal, &client);
    visibleColumns = std::max(1, visibleColumns);
    metrics.maximumOffset = std::max(0,
        static_cast<int>(totalColumns) - visibleColumns);
    if (metrics.maximumOffset <= 0) return metrics;
    metrics.visible = true;
    metrics.track = {textLeft, client.bottom - Ui::OverlayScrollLaneWidth,
        client.right - Ui::OverlayScrollLaneWidth, client.bottom};
    const int trackWidth = std::max(1,
        static_cast<int>(metrics.track.right - metrics.track.left));
    const int thumbWidth = std::max(28,
        trackWidth * std::min<int>(visibleColumns, static_cast<int>(totalColumns)) /
            std::max<int>(1, static_cast<int>(totalColumns)));
    const int travel = std::max(1, trackWidth - thumbWidth);
    const int thumbLeft = metrics.track.left +
        scrollOffset * travel / std::max(1, metrics.maximumOffset);
    const int top = client.bottom -
        (Ui::OverlayScrollLaneWidth + Ui::OverlayScrollThumbWidth) / 2;
    metrics.thumb = {thumbLeft, top, thumbLeft + thumbWidth,
        top + Ui::OverlayScrollThumbWidth};
    return metrics;
}

inline RECT TerminalNewOutputRect(const RECT& client, bool horizontalScroll) {
    const LONG bottom = client.bottom - (horizontalScroll ? Ui::OverlayScrollLaneWidth + Ui::Scale(4) : Ui::Space);
    return {client.right - Ui::Scale(126), bottom - Ui::Scale(30),
        client.right - Ui::OverlayScrollLaneWidth - Ui::Space, bottom};
}
} // namespace serialctl
