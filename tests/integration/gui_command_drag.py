"""Real desktop-mouse regression for command dragging in an isolated fixture.

The caller supplies a foreground production GUI with at least three visible
commands. This test changes their in-memory order and restores it, never clicks
Run, Edit or Save, and records desktop pixels during the held drag. It requires
an interactive Windows desktop. No program state is fabricated by this script.
"""
import argparse
import contextlib
import ctypes as c
from ctypes import wintypes as w
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback

from desktop_user_actions import DesktopActions, require

LB_GETCOUNT, LB_GETCURSEL, LB_GETITEMHEIGHT, LB_GETTOPINDEX = 0x18B, 0x188, 0x1A1, 0x18E
GHOST_CLASS = "SerialCtlCommandDragGhost"


class CommandDrag(DesktopActions):
    def __init__(self, args):
        args.mode = "desktop"
        super().__init__(args)
        self.report["scope"] = "real Windows mouse, visible production command list; in-memory fixture only"
        self.report["limitations"] = [
            "Desktop capture excludes the hardware cursor and can miss flicker between frames.",
            "The selected index is checked here; content ordering is independently covered by native integration tests.",
            "One desktop resolution and theme are tested per invocation.",
        ]
        self.user.EnumWindows.argtypes = (self.enum_callback, w.LPARAM)
        self.user.EnumWindows.restype = w.BOOL
        self.user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
        self.user.GetClassNameW.restype = c.c_int
        self.user.keybd_event.argtypes = (w.BYTE, w.BYTE, w.DWORD, c.c_size_t)
        self.user.keybd_event.restype = None
        self.list = self.control(112)

    def lb(self, message, wparam=0):
        result = c.c_size_t()
        require(self.user.SendMessageTimeoutW(self.list, message, wparam, 0, 2, 1000, c.byref(result)),
                "Command list did not respond to read-only inspection")
        return c.c_ssize_t(result.value).value

    def ghost(self):
        found = []
        @self.enum_callback
        def each(hwnd, _):
            pid = w.DWORD()
            self.user.GetWindowThreadProcessId(hwnd, c.byref(pid))
            name = c.create_unicode_buffer(128)
            self.user.GetClassNameW(hwnd, name, len(name))
            if pid.value == self.args.pid and name.value == GHOST_CLASS and self.user.IsWindowVisible(hwnd):
                found.append(int(hwnd))
            return True
        require(self.user.EnumWindows(each, 0), "Cannot inspect drag preview windows")
        require(len(found) <= 1, "Multiple drag previews remained visible")
        return found[0] if found else None

    def await_ghost(self, present, timeout=2):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = self.ghost()
            if bool(found) == present:
                return found
            time.sleep(.02)
        raise AssertionError("Drag preview did not " + ("appear" if present else "disappear"))

    def esc(self):
        self.user.keybd_event(0x1B, 0, 0, 0)
        self.user.keybd_event(0x1B, 0, 2, 0)
        self.trace.append({**self.timestamp(), "event": "escape"})

    def rowpoint(self, index):
        left, top, right, bottom = self.rect(self.list)
        height = self.lb(LB_GETITEMHEIGHT)
        visible_top = self.lb(LB_GETTOPINDEX)
        y = top + (index - visible_top) * height + height // 2
        x = left + round(55 * self.scale)  # body, outside Run/Edit and the old narrow handle
        require(left < x < right - round(90 * self.scale) and top < y < bottom,
                "Fixture does not expose enough room to drag command bodies")
        return x, y

    def capture_named(self, label):
        self.capture(label)
        preview = self.ghost()
        self.frames[-1]["preview_bounds"] = self.rect(preview) if preview else None

    def hold_fraction(self, frame_a, frame_b, area):
        # Compare a stable first-row portion during a stationary hold, outside
        # the preview and drop target. Frame capture deliberately excludes the
        # hardware cursor. Values are evidence; no claim is made between frames.
        (a, raw_a), (b, raw_b) = frame_a, frame_b
        require((a["width"], a["height"]) == (b["width"], b["height"]), "Desktop resolution changed")
        desktop = self.desktop_rect()
        left, top, right, bottom = area
        left, right = max(0, left - desktop[0]), min(a["width"], right - desktop[0])
        top, bottom = max(0, top - desktop[1]), min(a["height"], bottom - desktop[1])
        total = changed = 0
        for y in range(top, bottom, 2):
            for x in range(left, right, 2):
                offset = (y * a["width"] + x) * 4
                total += 1
                changed += raw_a[offset:offset+3] != raw_b[offset:offset+3]
        require(total > 100, "Stable-region sample is too small")
        return changed / total

    def drag(self, start, end, label, release=True):
        self.move_mouse(start)
        self.button(True)
        time.sleep(.12)
        require(not self.ghost(), "A stationary press incorrectly starts drag")
        self.capture_named(label + "-press")
        offsets = []
        for step in range(1, 9):
            p = (start[0] + (end[0] - start[0]) * step // 8,
                 start[1] + (end[1] - start[1]) * step // 8)
            self.move_mouse(p)
            time.sleep(.05)
            preview = self.await_ghost(True)
            bounds = self.rect(preview)
            offsets.append((p[0] - bounds[0], p[1] - bounds[1]))
            self.capture_named(label + "-move-" + str(step))
        require(max(v[0] for v in offsets) - min(v[0] for v in offsets) <= 4 and
                max(v[1] for v in offsets) - min(v[1] for v in offsets) <= 4,
                "Drag card did not follow both cursor axes")
        if release:
            self.button(False)
            self.await_ghost(False)
            time.sleep(.12)
            self.capture_named(label + "-released")
        return offsets

    def run_drag_test(self):
        require(self.user.IsWindow(self.window) and self.user.IsWindowVisible(self.window), "Window is not visible")
        pid = w.DWORD()
        self.user.GetWindowThreadProcessId(self.window, c.byref(pid))
        require(pid.value == self.args.pid, "Window belongs to another process")
        require(self.user.IsWindowVisible(self.list), "Open the command panel before this test")
        require(self.lb(LB_GETCOUNT) >= 3 and self.lb(LB_GETTOPINDEX) == 0, "Fixture needs three visible commands at top")
        require(self.user.GetForegroundWindow() == self.window, "Bring the real application to the foreground")
        self.capture_named("initial")
        first, third = self.rowpoint(0), self.rowpoint(2)
        row_height = self.lb(LB_GETITEMHEIGHT)
        target = (third[0] + round(28 * self.scale), third[1] + row_height // 4)
        try:
            offsets = self.drag(first, target, "body", release=False)
            time.sleep(.2)
            self.capture_named("hold-a")
            time.sleep(.12)
            self.capture_named("hold-b")
            time.sleep(.12)
            self.capture_named("hold-c")
            bounds = self.rect(self.list)
            row_h = self.lb(LB_GETITEMHEIGHT)
            stable = (bounds[0] + 2, bounds[1] + 2, bounds[2] - 2, bounds[1] + row_h - 2)
            hold = self.pending_pixels[-3:]
            fractions = [self.hold_fraction(hold[i], hold[i+1], stable) for i in range(2)]
            self.report["stationary_first_row_changed_fraction"] = fractions
            require(max(fractions) < .02, "Stationary drag repeatedly erased or changed the unaffected first row")
            # During the move to row three, row five is outside both the moving
            # card and insertion marker. Compare it with the pre-drag capture:
            # a blank/erased list frame must not be mistaken for smooth motion.
            # Sampled frames cannot exclude a flash between captures.
            if self.lb(LB_GETCOUNT) >= 5 and bounds[1] + row_h * 5 <= bounds[3]:
                stable_during_move = (bounds[0] + 2, bounds[1] + row_h * 4 + 2,
                                      bounds[2] - 2, bounds[1] + row_h * 5 - 2)
                baseline = self.pending_pixels[0]
                moves = [entry for entry in self.pending_pixels if '-body-move-' in entry[0]['path']]
                require(len(moves) == 8, "Missing frames from moving drag")
                move_changes = [self.hold_fraction(baseline, entry, stable_during_move) for entry in moves]
                self.report["moving_unaffected_row_changed_fraction"] = move_changes
                require(max(move_changes) < .02, "An unaffected command row changed during the drag")
            else:
                self.report["moving_unaffected_row_changed_fraction"] = "NOT RUN: fifth row not fully visible"
            self.button(False)
            self.await_ghost(False)
            time.sleep(.12)
            self.capture_named("body-released")
            require(self.lb(LB_GETCURSEL) == 2, "Body drag did not commit at the third row")
            self.report["body_preview_mouse_offsets"] = offsets

            # Restore original fixture order; no save is performed.
            first = self.rowpoint(0)
            self.drag(self.rowpoint(2), (first[0], first[1] - row_height // 4), "restore")
            require(self.lb(LB_GETCURSEL) == 0, "Cannot restore initial fixture order")

            self.drag(self.rowpoint(0), self.rowpoint(1), "escape", release=False)
            self.esc()
            self.await_ghost(False)
            self.button(False)
            time.sleep(.12)
            self.capture_named("escape-released")
            require(self.lb(LB_GETCURSEL) == 0, "Escape unexpectedly committed drag")

            left, _, _, _ = self.rect(self.list)
            p = self.rowpoint(1)
            self.drag(self.rowpoint(0), (left - round(35 * self.scale), p[1]), "outside")
            require(self.lb(LB_GETCURSEL) == 0, "Outside release unexpectedly committed drag")
            if self.args.edge_scroll:
                initial_top = self.lb(LB_GETTOPINDEX)
                left, top, right, bottom = self.rect(self.list)
                visible_rows = math.ceil((bottom - top) / row_height)
                require(self.lb(LB_GETCOUNT) >= max(12, visible_rows + 6),
                        "Edge test needs at least six additional off-screen fixture commands")
                edge = (left + round(55 * self.scale), bottom - round(12 * self.scale))
                self.drag(self.rowpoint(0), edge, "edge", release=False)
                before = self.lb(LB_GETTOPINDEX)
                hold_start = time.monotonic()
                samples = []
                for i in range(6):
                    time.sleep(.1)
                    self.capture_named("edge-hold-" + str(i))
                    samples.append({"elapsed_seconds": round(time.monotonic() - hold_start, 3),
                                    "top_index": self.lb(LB_GETTOPINDEX)})
                after = self.lb(LB_GETTOPINDEX)
                elapsed = time.monotonic() - hold_start
                self.report["edge_top_indices"] = {"initial": initial_top, "before_hold": before,
                                                   "after_hold": after, "samples": samples}
                require(after > before and after - before <= max(7, math.ceil(elapsed / .16) + 2),
                        "Edge scrolling was absent or advanced too quickly during a 600 ms stationary hold")
                self.move_mouse((left - round(35 * self.scale), edge[1]))
                self.button(False)
                self.await_ghost(False)
                require(self.lb(LB_GETCURSEL) == 0, "Outside edge release unexpectedly committed drag")
                # Restore only the scroll position using normal mouse wheel input.
                self.move_mouse((left + round(55 * self.scale), top + round(55 * self.scale)))
                for _ in range(30):
                    if self.lb(LB_GETTOPINDEX) == initial_top:
                        break
                    self.user.mouse_event(0x0800, 0, 0, 120, 0)
                    time.sleep(.03)
                require(self.lb(LB_GETTOPINDEX) == initial_top, "Could not restore the fixture scroll position")
            else:
                self.report["edge_scroll"] = "NOT RUN: invoke with --edge-scroll and a long isolated fixture"
            self.report["status"] = "PASS"
        finally:
            with contextlib.suppress(Exception):
                self.button(False)
            self.flush_frames()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--edge-scroll", action="store_true", help="requires an isolated fixture with at least 12 commands")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This test requires an interactive Windows desktop")
    test = CommandDrag(args)
    try:
        test.run_drag_test()
    except Exception as error:
        test.report["status"] = "FAIL"
        test.report["error"] = str(error)
        test.report["traceback"] = traceback.format_exc()
        with contextlib.suppress(Exception):
            test.capture_named("failure")
            test.flush_frames()
        test.save()
        print(test.report["traceback"], file=sys.stderr)
        return 1
    test.save()
    print(json.dumps({"status": "PASS", "frames": len(test.frames),
                      "stationary_first_row_changed_fraction": test.report["stationary_first_row_changed_fraction"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
