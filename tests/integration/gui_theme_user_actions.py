"""Actual mouse theme changes; inspect the unhovered all-disconnect screen pixels.

No paint messages, forced invalidation, PrintWindow, or product state injection.
The narrow color oracle detects #29's stale background; it is not a general
visual-quality test. Early samples are retained, settled samples are asserted.
"""
import argparse
import contextlib
import ctypes as c
import json
import os
import time
import traceback

from desktop_user_actions import BITMAPINFO, BITMAPINFOHEADER, DesktopActions, POINT, require
from gui_paint_user_actions import write_png


class ThemeActions(DesktopActions):
    def save(self):
        (self.output / f"{self.args.prefix}-theme.json").write_text(
            json.dumps(self.report, ensure_ascii=False, indent=2), encoding="utf-8")

    def session_count(self):
        result = c.c_size_t()
        require(self.user.SendMessageTimeoutW(self.control(106), 0x18b, 0, 0, 2, 2000, c.byref(result)),
                "Cannot read real connection list count")
        return result.value

    def pixels(self, label, theme, state="idle", settled=True):
        bounds = self.rect(self.control(302))
        width, height = bounds[2] - bounds[0], bounds[3] - bounds[1]
        dc = memory = bitmap = previous = None
        try:
            dc = self.user.GetDC(None)
            require(dc, "Cannot acquire actual desktop DC")
            memory = self.gdi.CreateCompatibleDC(dc)
            require(memory, "Cannot create capture DC")
            info = BITMAPINFO()
            info.header.size = c.sizeof(BITMAPINFOHEADER)
            info.header.width, info.header.height = width, -height
            info.header.planes, info.header.bit_count = 1, 32
            pointer = c.c_void_p()
            bitmap = self.gdi.CreateDIBSection(dc, c.byref(info), 0, c.byref(pointer), None, 0)
            require(bitmap and pointer.value, "Cannot allocate capture DIB")
            previous = self.gdi.SelectObject(memory, bitmap)
            started = self.timestamp()
            require(self.gdi.BitBlt(memory, 0, 0, width, height, dc, bounds[0], bounds[1], 0x40CC0020),
                    "Actual visible screen capture failed")
            require(self.gdi.GdiFlush(), "Cannot synchronize captured DIB pixels")
            raw = c.string_at(pointer, width * height * 4)
        finally:
            if previous:
                self.gdi.SelectObject(memory, previous)
            if bitmap:
                self.gdi.DeleteObject(bitmap)
            if memory:
                self.gdi.DeleteDC(memory)
            if dc:
                self.user.ReleaseDC(None, dc)
        enabled = bool(self.user.IsWindowEnabled(self.control(302)))
        expected = ((31, 55, 80) if theme == "dark" else (229, 241, 255)) if state in ("hover", "pressed") else (
            ((44, 44, 46) if enabled else (36, 36, 38)) if theme == "dark" else
            ((242, 242, 247) if enabled else (255, 255, 255)))
        # Interior strips avoid the rounded border and central Chinese text.
        ys = list(range(round(4 * self.scale), round(8 * self.scale)))
        ys += [height - 1 - y for y in ys]
        matches = total = 0
        for y in ys:
            for x in range(width // 4, width * 3 // 4):
                offset = (y * width + x) * 4
                rgb = (raw[offset + 2], raw[offset + 1], raw[offset])
                matches += max(abs(a - b) for a, b in zip(rgb, expected)) <= 2
                total += 1
        name = f"{self.args.prefix}-theme-{label}.png"
        digest = write_png(self.output / name, raw, width, height)
        row = {**started, "path": name, "sha256": digest, "capture_bounds": bounds,
               "theme": theme, "state": state, "enabled": enabled, "settled": settled,
               "expected_rgb": expected, "sampled_pixels": total,
               "matching_fraction": matches / total, "caption": self.caption(self.control(302))}
        self.report["color_samples"].append(row)
        if settled and row["matching_fraction"] < .95:
            self.report["behavior_failures"].append(f"{label}: stale/incorrect footer background: {row['matching_fraction']:.3f}")

    def run(self):
        require(self.user.GetForegroundWindow() == self.window, "Production window must be foreground")
        require(self.visible(302) and self.user.IsWindowEnabled(self.control(302)), "Need an actual connected session")
        initial = self.caption(self.control(104))
        require(initial in ("深色", "浅色"), "Unknown theme button state")
        self.report["initial_theme_caption"] = initial
        self.report["purpose"] = "#29 theme changes without hovering the affected footer"
        self.report["color_samples"] = []
        self.report["theme_clicks"] = []
        count_before = self.session_count()
        marker_before = int(self.user.GetPropW(self.control(107), "SerialCtl.PaintProbe") or 0)
        self.capture("theme-before-full-desktop")
        self.flush_frames()
        for index in range(20):
            # This case checks theme invalidation, not double-click behavior.
            # Let the system double-click interval expire before reusing 104.
            time.sleep((self.user.GetDoubleClickTime() + 30) / 1000)
            before = self.caption(self.control(104))
            rect = self.rect(self.control(104))
            point = ((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2)
            require(self.user.WindowFromPoint(POINT(*point)) == self.control(104), "Theme button obstructed")
            clicked = self.timestamp()
            self.click(point)
            time.sleep(.016)
            theme = "dark" if before == "深色" else "light"
            self.pixels(f"{index:02d}-{theme}-early", theme, settled=False)
            time.sleep(.10)
            after = self.caption(self.control(104))
            require(after != before, "Real theme click was not applied")
            self.report["theme_clicks"].append({**clicked, "before": before, "after": after, "point": point})
            self.pixels(f"{index:02d}-{theme}-settled", theme)
        require(self.caption(self.control(104)) == initial, "Theme not restored after 20 changes")
        self.capture("theme-after-unhovered-full-desktop")
        self.flush_frames()
        # Exercise hover/pressed/focus using a real press cancelled outside; do
        # not activate all-disconnect or mask the preceding idle-theme check.
        theme = "dark" if initial == "浅色" else "light"
        rect = self.rect(self.control(302))
        point = ((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2)
        require(self.user.WindowFromPoint(POINT(*point)) == self.control(302), "Footer button obstructed")
        self.move_mouse(point)
        time.sleep(.10)
        self.pixels("hover", theme, "hover")
        self.button(True)
        try:
            time.sleep(.10)
            self.pixels("pressed", theme, "pressed")
            outside = self.rect(self.control(107))
            self.move_mouse((outside[0] + 40, outside[1] + 40))
        finally:
            self.button(False)
        time.sleep(.10)
        self.pixels("cancelled-focused", theme)
        count_after = self.session_count()
        marker_after = int(self.user.GetPropW(self.control(107), "SerialCtl.PaintProbe") or 0)
        self.report["session_count"] = [count_before, count_after]
        self.report["stdout_marker"] = [marker_before, marker_after]
        require(count_after == count_before, "Cancelled footer press changed the session list")
        require(marker_after != marker_before and self.user.IsWindowEnabled(self.control(302)),
                "Actual CMD output did not remain active after the cancelled press")
        self.report["limitations"].append("Disabled footer state is not exercised by this connected-session case.")
        self.report["status"] = "FAIL" if self.report["behavior_failures"] else "PASS"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--prefix", required=True)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Requires actual interactive Windows desktop")
    args.mode = "desktop"
    test = ThemeActions(args)
    try:
        test.run()
    except Exception as error:
        test.report.update(status="FAIL", error=str(error), traceback=traceback.format_exc())
        with contextlib.suppress(Exception):
            test.button(False)
            test.capture("theme-failure-full-desktop")
            test.flush_frames()
        print(traceback.format_exc())
    finally:
        (test.output / f"{args.prefix}-theme.json").write_text(
            json.dumps(test.report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{test.report['status']} actual theme changes: {args.prefix}")
    return 0 if test.report["status"] == "PASS" else (2 if test.report["behavior_failures"] else 1)


if __name__ == "__main__":
    raise SystemExit(main())
