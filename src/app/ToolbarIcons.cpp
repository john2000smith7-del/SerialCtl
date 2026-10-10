#include "ToolbarIcons.h"
#include <cstring>
#include <propidl.h>
#include <gdiplus.h>
#include <objidl.h>

namespace serialctl
{
struct ToolbarIcons::Images
{
    struct Atlas
    {
        IStream *stream = nullptr;
        std::unique_ptr<Gdiplus::Bitmap> bitmap;
        explicit Atlas(int id)
        {
            auto module = GetModuleHandleW(nullptr);
            auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
            if (!resource)
                return;
            DWORD length = SizeofResource(module, resource);
            auto source = LockResource(LoadResource(module, resource));
            auto memory = GlobalAlloc(GMEM_MOVEABLE, length);
            if (!memory || !source)
            {
                if (memory)
                    GlobalFree(memory);
                return;
            }
            auto destination = GlobalLock(memory);
            if (!destination)
            {
                GlobalFree(memory);
                return;
            }
            std::memcpy(destination, source, length);
            GlobalUnlock(memory);
            if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)))
            {
                GlobalFree(memory);
                return;
            }
            bitmap.reset(Gdiplus::Bitmap::FromStream(stream));
            if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
                bitmap.reset();
        }
        ~Atlas()
        {
            bitmap.reset();
            if (stream)
                stream->Release();
        }
    };
    Atlas black{2000}, white{2001};
};
ToolbarIcons::ToolbarIcons() = default;
ToolbarIcons::~ToolbarIcons() = default;
void ToolbarIcons::Draw(HDC dc, ToolbarIcon icon, const RECT &bounds, bool white)
{
    if (!images_)
        images_ = std::make_unique<Images>();
    auto &atlas = white ? images_->white : images_->black;
    if (!atlas.bitmap)
        return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const int cell = static_cast<int>(atlas.bitmap->GetHeight());
    graphics.DrawImage(atlas.bitmap.get(),
                       Gdiplus::Rect(bounds.left, bounds.top, bounds.right - bounds.left,
                                     bounds.bottom - bounds.top),
                       static_cast<int>(icon) * cell, 0, cell, cell, Gdiplus::UnitPixel);
}
} // namespace serialctl
