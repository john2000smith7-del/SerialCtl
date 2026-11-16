#include "UiMetrics.h"
#include "UiStyle.h"
#include <chrono>
#include <iostream>
#include <thread>

using namespace serialctl;

namespace {

int g_failures = 0;

void Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAILED: " << msg << '\n';
        ++g_failures;
    }
}

} // namespace

int main() {
    // 1. U1: Unified 3-tier rounding constants & DPI scaling (12 / 8 / 6 at 96 DPI)
    Expect(Ui::CardRadius == 12, "CardRadius is 12px at 96 DPI");
    Expect(Ui::Radius == 8, "Control Radius is 8px at 96 DPI");
    Expect(Ui::InnerRadius96 == 6, "InnerRadius96 is 6px at 96 DPI");

    Expect(UiCardRadius(96) == 12, "UiCardRadius(96) == 12");
    Expect(UiControlRadius(96) == 8, "UiControlRadius(96) == 8");
    Expect(UiInnerRadius(96) == 6, "UiInnerRadius(96) == 6");
    Expect(UiCardRadius(144) == 18, "UiCardRadius scales cleanly at 150% DPI");
    Expect(UiControlRadius(144) == 12, "UiControlRadius scales cleanly at 150% DPI");
    Expect(UiInnerRadius(144) == 9, "UiInnerRadius scales cleanly at 150% DPI");

    // 2. U2: Universal semicircle capsule buttons, padding & compact icon square detection
    const RECT compactBtn{0, 0, 96, 32};
    const RECT standardBtn{0, 0, 112, 36};
    Expect(UiButtonRadius(compactBtn, false, 96) == 16,
           "all buttons use half-height semicircle capsule radius (32px -> 16px)");
    Expect(UiButtonRadius(compactBtn, true, 96) == 16,
           "capsule pill button keeps half-height semicircle radius");
    Expect(UiButtonRadius(standardBtn, false, 96) == 18,
           "standard 36px-high button uses 18px semicircle capsule radius");

    const RECT compactIcon{0, 0, 28, 28};
    const RECT wideBtn{0, 0, 88, 28};
    Expect(UiIsCompactIconRect(compactIcon, 96), "28x28 detected as compact icon button");
    Expect(!UiIsCompactIconRect(wideBtn, 96), "88x28 not misclassified as compact icon");
    Expect(UiButtonHorizontalPad(compactIcon, 96) == 0,
           "square compact icon button keeps 0 horizontal text pad");
    Expect(UiButtonHorizontalPad(compactBtn, 96) == Ui::ButtonPadXCompact96,
           "compact 32px button uses unified 12px horizontal capsule pad");
    Expect(UiButtonHorizontalPad(standardBtn, 96) == Ui::ButtonPadXStandard96,
           "standard 36px button uses unified 16px horizontal capsule pad");
    const RECT paddedCompact = UiCapsuleContentRect(compactBtn, 96);
    Expect(paddedCompact.left == 12 && paddedCompact.right == 84,
           "UiCapsuleContentRect applies symmetric 12px padding to 96x32 capsule");

    // 3. F1: Action cooldown debouncing (suppress rapid double-clicks, allow after expiry)
    HWND fakeHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(0x12345678));
    constexpr UINT kActionId = 9001;
    Expect(UiTryAcquireActionCooldown(fakeHwnd, kActionId, 120),
           "first cooldown acquisition succeeds");
    Expect(!UiTryAcquireActionCooldown(fakeHwnd, kActionId, 120),
           "immediate repeat within 120ms is suppressed");
    Expect(UiTryAcquireActionCooldown(fakeHwnd, kActionId + 1, 120),
           "different actionId on same window is independent");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Expect(UiTryAcquireActionCooldown(fakeHwnd, kActionId, 120),
           "cooldown acquisition succeeds after interval expires");

    return g_failures == 0 ? 0 : 1;
}
