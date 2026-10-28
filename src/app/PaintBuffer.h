#pragma once
#include <windows.h>
namespace serialctl {
class PaintBuffer {
public:
    ~PaintBuffer() { Reset(); }
    PaintBuffer() = default;
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;
    HDC Get(HDC target, int width, int height, int dpi) {
        if (dc_ && width == width_ && height == height_ && dpi == dpi_) return dc_;
        Reset();
        if (width <= 0 || height <= 0) return nullptr;
        dc_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, width, height);
        if (!dc_ || !bitmap_) { Reset(); return nullptr; }
        previous_ = SelectObject(dc_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR) { previous_ = nullptr; Reset(); return nullptr; }
        width_ = width; height_ = height; dpi_ = dpi;
        return dc_;
    }
private:
    void Reset() {
        if (previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr; bitmap_ = nullptr; previous_ = nullptr;
    }
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    int width_ = 0, height_ = 0, dpi_ = 0;
};
} // namespace serialctl
