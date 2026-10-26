#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace serialctl::ui_test {
// Optional screenshots from the actual native controls/painters, kept with CI
// evidence rather than packaged runtime files. Synthetic content is test-only.
inline void SaveUiEvidence(HWND window, const std::wstring &name,
                           const std::function<void(HDC)> &paint = {}) {
    wchar_t directory[32768]{};
    const auto length = GetEnvironmentVariableW(L"SERIALCTL_EVIDENCE_DIR", directory, 32768);
    if (!length || length >= 32768)
        return;
    std::filesystem::create_directories(directory);
    RECT bounds{};
    GetClientRect(window, &bounds);
    HDC screen = GetDC(window), buffer = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, bounds.right, bounds.bottom);
    auto previous = SelectObject(buffer, bitmap);
    bool success = false;
    if (screen && buffer && bitmap) {
        if (paint) paint(buffer);
        else {
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            BitBlt(buffer, 0, 0, bounds.right, bounds.bottom, screen, 0, 0, SRCCOPY);
        }
        const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        Gdiplus::Bitmap image(bitmap, nullptr);
        success = image.Save((std::filesystem::path(directory) / name).c_str(), &png, nullptr) == Gdiplus::Ok;
    }
    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    ReleaseDC(window, screen);
    if (!success)
        throw std::runtime_error("Failed to retain native UI screenshot");
}
} // namespace serialctl::ui_test
