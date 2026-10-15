"""Exercise the real SerialCtl desktop using system mouse input (Windows only).

The caller opens the production window and starts CMD output, or establishes the
isolated SSH/SFTP process fixture. This script never substitutes a window, paints
an imitation UI, sends a command notification, or changes application settings.
Screenshots come from the visible desktop DC, not PrintWindow. JSON includes the
actual input timestamps and observed geometry, including intermediate frames.
"""

import argparse
import contextlib
import ctypes as c
from ctypes import wintypes as w
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import sys
import time
import traceback
import zlib


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class SidebarBehaviorFailure(AssertionError):
    """Observed input outcome; retain failure while exercising other controls."""


class POINT(c.Structure):
    _fields_ = [("x", w.LONG), ("y", w.LONG)]


class RECT(c.Structure):
    _fields_ = [("left", w.LONG), ("top", w.LONG),
                ("right", w.LONG), ("bottom", w.LONG)]


class BITMAPINFOHEADER(c.Structure):
    _fields_ = [("size", w.DWORD), ("width", w.LONG), ("height", w.LONG),
                ("planes", w.WORD), ("bit_count", w.WORD),
                ("compression", w.DWORD), ("image_size", w.DWORD),
                ("x_pixels", w.LONG), ("y_pixels", w.LONG),
                ("colors_used", w.DWORD), ("colors_important", w.DWORD)]


class BITMAPINFO(c.Structure):
    _fields_ = [("header", BITMAPINFOHEADER), ("color", w.DWORD * 1)]


class DesktopActions:
    def __init__(self, args):
        self.args = args
        self.window = args.window
        self.output = Path(args.output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.user = c.WinDLL("user32", use_last_error=True)
        self.gdi = c.WinDLL("gdi32", use_last_error=True)
        self._declare()
        # Coordinate reads, mouse input and captured desktop pixels must use the
        # same physical coordinate space, including on a non-96-DPI machine.
        self.user.SetProcessDPIAware()
        self.started = time.perf_counter()
        self.frames = []
        self.pending_pixels = []
        self.trace = []
        self.operations = []
        self.drag_full_before = None
        self.drag_evidence = []
        self.report = {
            "status": "RUNNING", "mode": args.mode, "prefix": args.prefix,
            "pid": args.pid, "window": self.window,
            "system": platform.platform(), "python": platform.python_version(),
            "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "input": "SetCursorPos + mouse_event, dispatched by Windows",
            "capture": "visible desktop GetDC(NULL) + BitBlt; no PrintWindow; hardware cursor excluded",
            "scope": ("production GUI with real CMD stdout" if args.mode == "desktop"
                      else "production GUI; caller substitutes SSH/SFTP child processes, not UI or layout; no real server assertion"),
            "limitations": ["Captured frames do not prove absence of flicker between frames.",
                            "Measured DPI only; this run does not establish other DPI values or Win7 compatibility."],
            "operations": self.operations, "input_trace": self.trace, "frames": self.frames,
            "drag_evidence": self.drag_evidence,
            "behavior_failures": [],
        }
        dpi_function = getattr(self.user, "GetDpiForWindow", None)
        if dpi_function:
            dpi_function.argtypes = (w.HWND,)
            dpi_function.restype = w.UINT
            self.dpi = dpi_function(self.window)
        else:
            dc = self.user.GetDC(None)
            self.dpi = self.gdi.GetDeviceCaps(dc, 88)
            self.user.ReleaseDC(None, dc)
        require(self.dpi > 0, "Cannot determine actual desktop DPI")
        self.scale = self.dpi / 96.0
        self.report["dpi"] = self.dpi
        self.report["desktop_bounds"] = self.desktop_rect()

    def _declare(self):
        functions = {
            "GetWindowRect": ((w.HWND, c.POINTER(RECT)), w.BOOL),
            "GetClientRect": ((w.HWND, c.POINTER(RECT)), w.BOOL),
            "ClientToScreen": ((w.HWND, c.POINTER(POINT)), w.BOOL),
            "GetDlgItem": ((w.HWND, c.c_int), w.HWND),
            "GetDlgCtrlID": ((w.HWND,), c.c_int),
            "GetWindowThreadProcessId": ((w.HWND, c.POINTER(w.DWORD)), w.DWORD),
            "GetForegroundWindow": ((), w.HWND),
            "WindowFromPoint": ((POINT,), w.HWND),
            "GetWindowTextW": ((w.HWND, w.LPWSTR, c.c_int), c.c_int),
            "IsWindow": ((w.HWND,), w.BOOL),
            "IsWindowVisible": ((w.HWND,), w.BOOL),
            "IsWindowEnabled": ((w.HWND,), w.BOOL),
            "IsZoomed": ((w.HWND,), w.BOOL),
            "SetCursorPos": ((c.c_int, c.c_int), w.BOOL),
            "GetCursorPos": ((c.POINTER(POINT),), w.BOOL),
            "mouse_event": ((w.DWORD, w.DWORD, w.DWORD, w.DWORD, c.c_size_t), None),
            "GetSystemMetrics": ((c.c_int,), c.c_int),
            "GetDoubleClickTime": ((), w.UINT),
            "SetProcessDPIAware": ((), w.BOOL),
            "SystemParametersInfoW": ((w.UINT, w.UINT, c.c_void_p, w.UINT), w.BOOL),
            "GetPropW": ((w.HWND, w.LPCWSTR), w.HANDLE),
            "SendMessageTimeoutW": ((w.HWND, w.UINT, c.c_size_t, c.c_ssize_t,
                                     w.UINT, w.UINT, c.POINTER(c.c_size_t)), c.c_ssize_t),
            "GetDC": ((w.HWND,), w.HDC),
            "ReleaseDC": ((w.HWND, w.HDC), c.c_int),
        }
        for name, (arguments, result) in functions.items():
            function = getattr(self.user, name)
            function.argtypes, function.restype = arguments, result
        self.enum_callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.user.EnumChildWindows.argtypes = (w.HWND, self.enum_callback, w.LPARAM)
        self.user.EnumChildWindows.restype = w.BOOL
        functions = {
            "CreateCompatibleDC": ((w.HDC,), w.HDC),
            "DeleteDC": ((w.HDC,), w.BOOL),
            "CreateDIBSection": ((w.HDC, c.POINTER(BITMAPINFO), w.UINT,
                                  c.POINTER(c.c_void_p), w.HANDLE, w.DWORD), w.HBITMAP),
            "SelectObject": ((w.HDC, w.HGDIOBJ), w.HGDIOBJ),
            "DeleteObject": ((w.HGDIOBJ,), w.BOOL),
            "BitBlt": ((w.HDC, c.c_int, c.c_int, c.c_int, c.c_int,
                         w.HDC, c.c_int, c.c_int, w.DWORD), w.BOOL),
            "GetDeviceCaps": ((w.HDC, c.c_int), c.c_int),
        }
        for name, (arguments, result) in functions.items():
            function = getattr(self.gdi, name)
            function.argtypes, function.restype = arguments, result

    def timestamp(self):
        return {"seconds": round(time.perf_counter() - self.started, 6),
                "utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}

    def prepare_full_window_drag(self):
        enabled = w.BOOL()
        require(self.user.SystemParametersInfoW(0x26, 0, c.byref(enabled), 0),
                "Cannot read SPI_GETDRAGFULLWINDOWS")
        self.drag_full_before = bool(enabled.value)
        self.report["system_window_drag"] = {
            "before": self.drag_full_before,
            "scope": "Temporary test-desktop setting; no profile persistence or product change",
        }
        if not self.drag_full_before:
            require(self.user.SystemParametersInfoW(0x25, 1, None, 0),
                    "Cannot enable actual content movement/resizing on test desktop")
        require(self.user.SystemParametersInfoW(0x26, 0, c.byref(enabled), 0),
                "Cannot verify SPI_GETDRAGFULLWINDOWS")
        self.report["system_window_drag"]["during_test"] = bool(enabled.value)
        require(enabled.value, "Full-window drag must be enabled for continuous resize evidence")

    def restore_full_window_drag(self):
        if self.drag_full_before is not None:
            require(self.user.SystemParametersInfoW(0x25, int(self.drag_full_before), None, 0),
                    "Cannot restore original test-desktop full-window drag setting")
            enabled = w.BOOL()
            require(self.user.SystemParametersInfoW(0x26, 0, c.byref(enabled), 0),
                    "Cannot read restored full-window drag setting")
            self.report["system_window_drag"]["restored"] = bool(enabled.value)
            require(bool(enabled.value) == self.drag_full_before, "Desktop drag setting was not restored")

    def desktop_rect(self):
        u = self.user
        left, top = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
        return [left, top, left + u.GetSystemMetrics(78), top + u.GetSystemMetrics(79)]

    def rect(self, window):
        rect = RECT()
        require(self.user.GetWindowRect(window, c.byref(rect)), "GetWindowRect failed")
        return [rect.left, rect.top, rect.right, rect.bottom]

    def client_rect(self):
        rect, point = RECT(), POINT()
        require(self.user.GetClientRect(self.window, c.byref(rect)), "GetClientRect failed")
        require(self.user.ClientToScreen(self.window, c.byref(point)), "ClientToScreen failed")
        return [point.x, point.y, point.x + rect.right, point.y + rect.bottom]

    def control(self, identifier):
        result = self.user.GetDlgItem(self.window, identifier)
        require(result, "Missing real control " + str(identifier))
        return result

    def visible(self, identifier):
        return bool(self.user.IsWindowVisible(self.control(identifier)))

    def snapshot(self):
        controls = {}
        for identifier in (106, 107, 112, 115, 117, 118, 119, 120, 125, 126, 127, 128, 130):
            window = self.control(identifier)
            controls[str(identifier)] = {"bounds": self.rect(window),
                                         "visible": bool(self.user.IsWindowVisible(window))}
        return {**self.timestamp(), "window_bounds": self.rect(self.window),
                "client_bounds": self.client_rect(),
                "foreground": int(self.user.GetForegroundWindow() or 0),
                "maximized": bool(self.user.IsZoomed(self.window)),
                "terminal_paint_sample": int(self.user.GetPropW(self.control(107), "SerialCtl.PaintSample") or 0),
                "terminal_paint_probe": int(self.user.GetPropW(self.control(107), "SerialCtl.PaintProbe") or 0),
                "controls": controls}

    def read_message(self, message, lparam=0):
        result = c.c_size_t()
        start = time.perf_counter()
        ok = self.user.SendMessageTimeoutW(self.window, message, 0, lparam, 2, 1000, c.byref(result))
        require(ok, "Native message timed out or window disappeared: " + hex(message))
        return result.value, round((time.perf_counter() - start) * 1000, 3)

    def responsive(self):
        _, elapsed = self.read_message(0)  # WM_NULL observes responsiveness only.
        return elapsed

    def hit_test(self, x, y):
        result, _ = self.read_message(0x84, (x & 0xffff) | ((y & 0xffff) << 16))
        return c.c_ssize_t(result).value

    def check_layout(self, expanded=True):
        client = self.client_rect()
        offending = []

        @self.enum_callback
        def inspect(window, _):
            if self.user.IsWindowVisible(window):
                rect = self.rect(window)
                if rect[2] > rect[0] and rect[3] > rect[1] and (
                        rect[0] < client[0] - 1 or rect[1] < client[1] - 1 or
                        rect[2] > client[2] + 1 or rect[3] > client[3] + 1):
                    offending.append({"id": self.user.GetDlgCtrlID(window), "bounds": rect})
            return True

        self.user.EnumChildWindows(self.window, inspect, 0)
        require(not offending, "Visible children outside main client: " + repr(offending))
        terminal = self.rect(self.control(107))
        # The documented 400 px minimum is the center card; terminal has 16 px
        # padding on each side. Do not incorrectly require 400 px of text area.
        minimum = round((400 - 32) * self.scale)
        require(terminal[2] - terminal[0] >= minimum - 2,
                "Terminal usable width below center-card minimum: " + repr(terminal))
        require(terminal[3] - terminal[1] >= round(200 * self.scale), "Terminal usable height collapsed")
        toggle = self.rect(self.control(125))
        require(toggle[2] <= client[2] - round(12 * self.scale) + 1, "Toggle exceeds right card margin")
        if expanded:
            active = 112 if self.args.mode == "desktop" else 119
            require(self.visible(active), "Expected sidebar content is hidden")
            panel = self.rect(self.control(active))
            require(terminal[2] < panel[0], "Terminal and sidebar overlap")
        else:
            for identifier in range(112, 133):
                if identifier != 125:
                    require(not self.visible(identifier), "Collapsed sidebar exposes control " + str(identifier))
            require(toggle[0] >= client[2] - round(60 * self.scale) - 1, "Collapsed toggle exceeds 48 px rail")
        return {"terminal_minimum_content_width": minimum, "visible_child_bounds": "PASS"}

    def move_mouse(self, point):
        x, y = map(int, point)
        require(self.user.SetCursorPos(x, y), "SetCursorPos failed: interactive desktop unavailable")
        actual = POINT()
        require(self.user.GetCursorPos(c.byref(actual)), "GetCursorPos failed")
        require(abs(actual.x - x) <= 1 and abs(actual.y - y) <= 1,
                "Cursor did not reach requested desktop position")
        self.trace.append({**self.timestamp(), "event": "mouse_move", "x": x, "y": y})

    def button(self, down):
        self.user.mouse_event(2 if down else 4, 0, 0, 0, 0)
        self.trace.append({**self.timestamp(), "event": "left_down" if down else "left_up"})

    def click(self, point):
        self.move_mouse(point)
        self.button(True)
        time.sleep(.008)
        self.button(False)

    def click_control(self, identifier):
        window = self.control(identifier)
        require(self.user.IsWindowVisible(window) and self.user.IsWindowEnabled(window),
                "Control cannot receive user click: " + str(identifier))
        rect = self.rect(window)
        self.click(((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2))

    def caption(self, window):
        text = c.create_unicode_buffer(256)
        self.user.GetWindowTextW(window, text, len(text))
        return text.value

    def capture(self, label):
        left, top, right, bottom = self.desktop_rect()
        width, height = right - left, bottom - top
        require(width >= 980 and height >= 620, "No usable visible desktop")
        dc = self.user.GetDC(None)
        require(dc, "No visible desktop DC")
        memory, bitmap, previous = None, None, None
        try:
            memory = self.gdi.CreateCompatibleDC(dc)
            require(memory, "CreateCompatibleDC failed")
            info = BITMAPINFO()
            info.header.size = c.sizeof(BITMAPINFOHEADER)
            info.header.width, info.header.height = width, -height
            info.header.planes, info.header.bit_count = 1, 32
            pixels = c.c_void_p()
            bitmap = self.gdi.CreateDIBSection(dc, c.byref(info), 0, c.byref(pixels), None, 0)
            require(bitmap and pixels.value, "CreateDIBSection failed")
            previous = self.gdi.SelectObject(memory, bitmap)
            require(previous, "SelectObject failed")
            require(self.gdi.BitBlt(memory, 0, 0, width, height, dc, left, top, 0x40CC0020),
                    "BitBlt failed: desktop may be locked or inaccessible")
            raw = c.string_at(pixels, width * height * 4)
        finally:
            if previous:
                self.gdi.SelectObject(memory, previous)
            if bitmap:
                self.gdi.DeleteObject(bitmap)
            if memory:
                self.gdi.DeleteDC(memory)
            self.user.ReleaseDC(None, dc)
        require(len(set(raw[::997])) > 4, "Desktop capture is blank or uniform")
        file_name = f"{self.args.prefix}-{self.args.mode}-{len(self.frames):04d}-{label}.png"
        frame = {**self.timestamp(), "path": file_name, "width": width, "height": height,
                 "window_bounds": self.rect(self.window),
                 "sidebar_toggle_bounds": self.rect(self.control(125)),
                 "terminal_paint_sample": int(self.user.GetPropW(self.control(107), "SerialCtl.PaintSample") or 0),
                 "terminal_paint_probe": int(self.user.GetPropW(self.control(107), "SerialCtl.PaintProbe") or 0)}
        self.frames.append(frame)
        # Defer PNG conversion/disk writes until mouse-up: capturing an animation
        # must not slow the input interval beyond the application's 200 ms timer.
        self.pending_pixels.append((frame, raw))

    def flush_frames(self):
        while self.pending_pixels:
            frame, raw = self.pending_pixels.pop(0)
            width, height = frame["width"], frame["height"]
            rgb = bytearray(width * height * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
            stride = width * 3
            scanlines = b"".join(b"\0" + rgb[row * stride:(row + 1) * stride] for row in range(height))

            def chunk(kind, data):
                return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

            png = (b"\x89PNG\r\n\x1a\n" +
                   chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                   chunk(b"IDAT", zlib.compress(scanlines, 1)) + chunk(b"IEND", b""))
            (self.output / frame["path"]).write_bytes(png)
            frame["sha256"] = hashlib.sha256(png).hexdigest()
        self.save()

    def save(self):
        report_path = self.output / f"{self.args.prefix}-{self.args.mode}-user-actions.json"
        report_path.write_text(json.dumps(self.report, ensure_ascii=False, indent=2), encoding="utf-8")

    @contextlib.contextmanager
    def operation(self, name):
        entry = {"name": name, "status": "RUNNING", "before": self.snapshot()}
        self.operations.append(entry)
        try:
            yield entry
            entry["wm_null_ms"] = self.responsive()
            entry["after"] = self.snapshot()
            entry["status"] = "PASS"
        except Exception as error:
            entry["status"], entry["error"] = "FAIL", str(error)
            with contextlib.suppress(Exception):
                entry["after"] = self.snapshot()
            raise
        finally:
            self.flush_frames()

    def caption_point(self):
        rect = self.rect(self.window)
        x = rect[0] + min(round(220 * self.scale), (rect[2] - rect[0]) // 3)
        candidates = [y for y in range(max(0, rect[1]) + 2,
                                       max(0, rect[1]) + round(55 * self.scale))
                      if self.hit_test(x, y) == 2]  # HTCAPTION, read-only.
        require(candidates, "Cannot locate real caption hit-test region")
        return (x, candidates[len(candidates) // 2])

    def drag(self, name, start, end, steps=8):
        first_frame = len(self.frames)
        self.move_mouse(start)
        self.button(True)
        try:
            for index in range(1, steps + 1):
                point = (round(start[0] + (end[0] - start[0]) * index / steps),
                         round(start[1] + (end[1] - start[1]) * index / steps))
                self.move_mouse(point)
                time.sleep(.018)
                self.capture(f"{name}-held-{index:02d}")
        finally:
            self.button(False)
        held = self.frames[first_frame:]
        evidence = {
            "name": name, "held_frames": [frame["path"] for frame in held],
            "paint_sample_first": held[0]["terminal_paint_sample"],
            "paint_sample_last": held[-1]["terminal_paint_sample"],
            "stdout_markers_while_held": sorted({frame["terminal_paint_probe"] for frame in held
                                                 if frame["terminal_paint_probe"] > 0}),
        }
        evidence["stdout_changed_while_held"] = len(evidence["stdout_markers_while_held"]) > 1
        self.drag_evidence.append(evidence)
        time.sleep(.08)
        self.capture(name + "-released")

    def move_window_to(self, left, top, label):
        with self.operation(label) as entry:
            before = self.rect(self.window)
            start = self.caption_point()
            self.drag(label, start, (start[0] + left - before[0], start[1] + top - before[1]))
            after = self.rect(self.window)
            require(abs(after[0] - left) <= 3 and abs(after[1] - top) <= 3,
                    "Caption drag did not move window to target: " + repr(after))
            require(after[2] - after[0] == before[2] - before[0] and
                    after[3] - after[1] == before[3] - before[1], "Caption drag unexpectedly resized window")
            require(after[:2] != before[:2], "Caption drag did not change window position")
            entry["assertions"] = self.check_layout()

    def resize(self, width, height, label):
        with self.operation(label) as entry:
            before = self.rect(self.window)
            corners = [(before[2] - offset, before[3] - offset)
                       for offset in range(1, round(12 * self.scale) + 1)]
            start = next((point for point in corners if self.hit_test(*point) == 17), None)
            require(start is not None, "Cannot locate real HTBOTTOMRIGHT resize border")
            end = (start[0] + width - (before[2] - before[0]),
                   start[1] + height - (before[3] - before[1]))
            self.drag(label, start, end)
            after = self.rect(self.window)
            require(abs(after[2] - after[0] - width) <= 3 and abs(after[3] - after[1] - height) <= 3,
                    f"Native border drag target {width}x{height}, actual {after}")
            require(after[:2] == before[:2], "Bottom-right drag unexpectedly moved top-left")
            entry["target_outer_size"] = [width, height]
            entry["assertions"] = self.check_layout()

    def maximize_restore(self):
        restored = self.rect(self.window)
        for maximize in (True, False):
            label = "caption-double-click-" + ("maximize" if maximize else "restore")
            with self.operation(label) as entry:
                point = self.caption_point()
                self.click(point)
                time.sleep(.06)
                self.click(point)
                time.sleep(.25)
                self.capture(label)
                require(bool(self.user.IsZoomed(self.window)) == maximize,
                        "Caption double-click did not change maximize state")
                if not maximize:
                    require(all(abs(a - b) <= 3 for a, b in zip(restored, self.rect(self.window))),
                            "Restore did not retain normal window bounds")
                entry["assertions"] = self.check_layout()

    def sidebar_drag(self):
        active = 112 if self.args.mode == "desktop" else 119
        # The caller may leave the panel at its 260 px minimum. Enlarge first;
        # trying to shrink it first would test the clamp, not successful dragging.
        for index, delta in enumerate((-85, 65, -45)):
            label = f"sidebar-splitter-{index + 1}"
            with self.operation(label) as entry:
                before = self.rect(self.control(active))
                terminal_before = self.rect(self.control(107))
                start = (before[0] - round(22 * self.scale), before[1] + round(95 * self.scale))
                self.drag(label, start, (start[0] + round(delta * self.scale), start[1]))
                after = self.rect(self.control(active))
                require(abs(after[0] - before[0]) >= round(10 * self.scale),
                        "Sidebar splitter did not change actual panel bounds")
                require(self.rect(self.control(107))[2] != terminal_before[2],
                        "Sidebar drag did not resize terminal")
                entry["assertions"] = self.check_layout()

    def sidebar_toggle(self):
        initial_toggle_bounds = self.rect(self.control(125))

        def check_anchor(bounds, phase):
            if bounds != initial_toggle_bounds:
                raise SidebarBehaviorFailure(
                    f"Sidebar toggle moved during {phase}: "
                    f"expected {initial_toggle_bounds}, observed {bounds}")

        def capture_frame(label):
            self.capture(label)
            check_anchor(self.frames[-1]["sidebar_toggle_bounds"], label)

        for collapse in (True, False):
            label = "sidebar-" + ("collapse" if collapse else "expand")
            with self.operation(label) as entry:
                entry["initial_toggle_bounds"] = initial_toggle_bounds
                self.click_control(125)
                for frame in range(8):
                    time.sleep(.022)
                    capture_frame(f"{label}-animation-{frame:02d}")
                time.sleep(.1)
                entry["assertions"] = self.check_layout(expanded=not collapse)
                capture_frame(label + "-settled")
                entry["assertions"]["fixed_sidebar_toggle"] = "PASS"
        for stationary in (False, True):
            operation = "sidebar-rapid-same-point-reversal" if stationary else "sidebar-rapid-reversal"
            frame_prefix = "sidebar-same-point-reversal" if stationary else "sidebar-reversal"
            with self.operation(operation) as entry:
                clicks = []
                click_observations = []
                entry["initial_toggle_bounds"] = initial_toggle_bounds
                entry["stationary_pointer"] = stationary
                entry["single_click_observations"] = click_observations
                entry["double_click_ambiguity"] = (
                    "This sequence uses one fixed point and verifies the target caption after every "
                    "released click, including input eligible for native double-click handling. "
                    "It does not assert which native notifications were delivered."
                    if stationary else
                    "Prior center-only rapid input could be coalesced by the native Button class into "
                    "BN_DOUBLECLICKED. This sequence alternates separated points and verifies the toggle "
                    "caption after every released click. Prior attempt evidence is retained unchanged.")
                double_click_width = self.user.GetSystemMetrics(36)  # SM_CXDOUBLECLK.
                double_click_time = self.user.GetDoubleClickTime()
                entry["system_double_click_width_px"] = double_click_width
                entry["system_double_click_time_ms"] = double_click_time
                previous_point = None
                for index in range(10):
                    toggle = self.control(125)
                    bounds = self.rect(toggle)
                    check_anchor(bounds, f"{operation} click {index} target selection")
                    inset = max(4, round(5 * self.scale))
                    y = (bounds[1] + bounds[3]) // 2
                    candidates = [(bounds[0] + inset, y), (bounds[2] - inset - 1, y)]
                    point = ((initial_toggle_bounds[0] + initial_toggle_bounds[2]) // 2,
                             (initial_toggle_bounds[1] + initial_toggle_bounds[3]) // 2) if stationary else candidates[index % 2]
                    if not stationary:
                        require(previous_point is None or abs(point[0] - previous_point[0]) > double_click_width,
                                "Toggle is too narrow to disambiguate consecutive single clicks")
                    target = self.user.WindowFromPoint(POINT(*point))
                    require(target == toggle, "Rapid-click point is obstructed or outside the actual toggle")
                    before_caption = self.caption(toggle)
                    require(before_caption, "Cannot read native toggle caption before single click")
                    observation = {**self.timestamp(), "index": index, "bounds": bounds,
                                   "point": list(point), "window_from_point": int(target),
                                   "caption_before": before_caption,
                                   "distance_from_previous_x": (abs(point[0] - previous_point[0])
                                                                if previous_point else None)}
                    click_observations.append(observation)
                    clicks.append(time.perf_counter())
                    self.move_mouse(point)
                    observation["bounds_before_down"] = self.rect(toggle)
                    observation["hit_before_down"] = int(self.user.WindowFromPoint(POINT(*point)) or 0)
                    self.button(True)
                    time.sleep(.008)
                    observation["bounds_before_up"] = self.rect(toggle)
                    observation["hit_before_up"] = int(self.user.WindowFromPoint(POINT(*point)) or 0)
                    self.button(False)
                    observation["bounds_after_up"] = self.rect(toggle)
                    after_caption = self.caption(toggle)
                    deadline = time.perf_counter() + .06
                    while after_caption == before_caption and time.perf_counter() < deadline:
                        time.sleep(.003)
                        after_caption = self.caption(toggle)
                    observation["caption_after_mouse_up"] = after_caption
                    observation["caption_observed_seconds"] = self.timestamp()["seconds"]
                    observation["caption_flipped"] = bool(after_caption and after_caption != before_caption)
                    for phase in ("bounds_before_down", "bounds_before_up", "bounds_after_up"):
                        check_anchor(observation[phase], f"{operation} click {index} {phase}")
                    if not observation["caption_flipped"]:
                        raise SidebarBehaviorFailure(
                            "Mouse click at sidebar toggle did not change its target state; "
                            "see down/up bounds and hit targets before attributing message delivery")
                    previous_point = point
                    time.sleep(.033)
                    capture_frame(f"{frame_prefix}-{index:02d}")
                time.sleep(.3)
                intervals = [round((b - a) * 1000, 3) for a, b in zip(clicks, clicks[1:])]
                entry["click_intervals_ms"] = intervals
                require(any(interval < 200 for interval in intervals),
                        "Input/capture was too slow to test reversal within 200 ms animation")
                if stationary:
                    require(any(interval < double_click_time for interval in intervals),
                            "Same-point input was too slow to exercise native double-click timing")
                active = 112 if self.args.mode == "desktop" else 119
                entry["expected_final_expanded"] = True  # Started expanded, ten toggles.
                entry["actual_final_expanded"] = self.visible(active)
                if not entry["actual_final_expanded"]:
                    raise SidebarBehaviorFailure("Ten rapid toggle attempts did not restore expanded sidebar")
                entry["assertions"] = self.check_layout()
                capture_frame(frame_prefix + "-settled")
                entry["assertions"]["fixed_sidebar_toggle"] = "PASS"

    def check_sidebar_and_continue(self):
        try:
            self.sidebar_toggle()
        except SidebarBehaviorFailure as error:
            self.report["behavior_failures"].append(str(error))
            self.capture("sidebar-behavior-failure")
            # Recovery is a separate operation and cannot change the failed
            # operation or overall FAIL result. Other cases need an open pane.
            with self.operation("recovery-after-sidebar-failure") as entry:
                time.sleep(.65)
                active = 112 if self.args.mode == "desktop" else 119
                entry["needed_click"] = not self.visible(active)
                if entry["needed_click"]:
                    self.click_control(125)
                    time.sleep(.35)
                entry["assertions"] = self.check_layout()
                self.capture("sidebar-recovery-for-remaining-tests")

    def sftp_columns(self):
        for index, delta in enumerate((-24, 32, -16)):
            label = f"sftp-column-boundary-{index + 1}"
            with self.operation(label) as entry:
                before = self.rect(self.control(126))
                size_before = self.rect(self.control(127))
                # The existing smoke may already have narrowed Name. Ensure the
                # requested direction has room above the native 80 px minimum.
                if delta < 0 and before[2] - before[0] <= round(105 * self.scale):
                    delta = abs(delta)
                start = (before[2] - 2, (before[1] + before[3]) // 2)
                self.drag(label, start, (start[0] + round(delta * self.scale), start[1]))
                after = self.rect(self.control(126))
                size_after = self.rect(self.control(127))
                difference = (after[2] - after[0]) - (before[2] - before[0])
                require(abs(difference) >= round(5 * self.scale), "Actual Name column width did not change")
                require(size_after[0] - size_before[0] == difference,
                        "Size column did not follow Name boundary")
                require(after[2] == size_after[0], "SFTP header gap/overlap after user drag")
                entry["name_width_change"] = difference
                entry["assertions"] = self.check_layout()
        for index, delta in enumerate((14, -14)):
            label = f"sftp-size-boundary-{index + 1}"
            with self.operation(label) as entry:
                name_before = self.rect(self.control(126))
                before = self.rect(self.control(127))
                # This boundary inversely adjusts Name to preserve the total
                # table width; ensure Name can shrink above its native minimum.
                if delta > 0 and name_before[2] - name_before[0] <= round(96 * self.scale):
                    delta = -delta
                start = (before[2] - 2, (before[1] + before[3]) // 2)
                self.drag(label, start, (start[0] + round(delta * self.scale), start[1]))
                after = self.rect(self.control(127))
                name_after = self.rect(self.control(126))
                difference = (after[2] - after[0]) - (before[2] - before[0])
                require(abs(difference) >= round(5 * self.scale), "Actual Size column width did not change")
                require(name_after[2] == after[0], "Name/Size overlap after Size boundary drag")
                require(after[2] == before[2], "Size drag unexpectedly changed overall table extent")
                entry["size_width_change"] = difference
                entry["assertions"] = self.check_layout()

    def run(self):
        self.prepare_full_window_drag()
        require(self.user.IsWindow(self.window), "The supplied HWND is not a live production window")
        process_id = w.DWORD()
        self.user.GetWindowThreadProcessId(self.window, c.byref(process_id))
        require(process_id.value == self.args.pid, "HWND is owned by a different process")
        require(self.user.IsWindowVisible(self.window), "The real application window is hidden")
        require(not self.user.IsZoomed(self.window), "Caller must provide a restored window")
        self.capture("initial")
        with self.operation("activate-by-caption-click"):
            caption = self.caption_point()
            # The next operation starts a caption drag. Activate at a distinct
            # point so its mouse-down cannot become an accidental double-click.
            activation = (caption[0] + round(80 * self.scale), caption[1])
            require(self.hit_test(*activation) == 2, "Activation point is outside caption")
            self.click(activation)
            time.sleep(.12)
            require(self.user.GetForegroundWindow() == self.window,
                    "Real caption click did not activate target window; desktop may be obstructed")
        initial_paint = int(self.user.GetPropW(self.control(107), "SerialCtl.PaintSample") or 0)
        require(initial_paint > 0, "Caller must enable SERIALCTL_UI_METRICS and start actual terminal output")
        if self.args.mode == "desktop":
            self.move_window_to(40, 40, "caption-drag-to-40-40")
            for round_number in range(1, 4):
                self.resize(round(980 * self.scale), round(620 * self.scale), f"resize-round-{round_number}-minimum")
                self.resize(round(1180 * self.scale), round(760 * self.scale), f"resize-round-{round_number}-large")
            self.maximize_restore()
            self.sidebar_drag()
            self.check_sidebar_and_continue()
        else:
            require(self.visible(119) and self.visible(126), "Caller must open the actual SFTP panel")
            self.sftp_columns()
            self.check_sidebar_and_continue()
        final_paint = int(self.user.GetPropW(self.control(107), "SerialCtl.PaintSample") or 0)
        require(final_paint > initial_paint, "No terminal painting occurred during real user operations")
        self.report["paint_samples"] = {"before": initial_paint, "after": final_paint}
        # PaintSample can advance because of resizing alone. Distinct producer
        # markers establish that real child-process stdout also reached paint.
        probes = {frame["terminal_paint_probe"] for frame in self.frames if frame["terminal_paint_probe"] > 0}
        require(len(probes) >= 4, "Fewer than four distinct stdout markers were painted during user actions")
        self.report["distinct_painted_stdout_markers"] = len(probes)
        overlapping = [item["name"] for item in self.drag_evidence if item["stdout_changed_while_held"]]
        require(overlapping, "No real stdout marker changed while a drag was actually held")
        self.report["drags_overlapping_painted_stdout"] = overlapping
        self.report["final_layout"] = self.check_layout()
        self.report["final_state"] = self.snapshot()
        self.capture("final")
        self.report["status"] = "FAIL" if self.report["behavior_failures"] else "PASS"
        self.report["completed_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.flush_frames()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--mode", required=True, choices=("desktop", "sftp"))
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This test requires an actual interactive Windows desktop; it cannot pass on Linux.")
    test = DesktopActions(args)
    exit_code = 0
    try:
        test.run()
        if test.report["behavior_failures"]:
            exit_code = 2
            print("FAIL: " + "; ".join(test.report["behavior_failures"]), file=sys.stderr)
    except Exception as error:
        test.report["status"] = "FAIL"
        test.report["error"] = str(error)
        test.report["traceback"] = traceback.format_exc()
        with contextlib.suppress(Exception):
            test.button(False)
            test.capture("failure")
        test.flush_frames()
        print(test.report["traceback"], file=sys.stderr)
        exit_code = 1
    finally:
        try:
            test.restore_full_window_drag()
        except Exception as error:
            test.report["status"] = "FAIL"
            test.report["desktop_restore_error"] = str(error)
            exit_code = 1
        test.save()
    if exit_code == 0:
        print(f"PASS {args.prefix} {args.mode}: {len(test.operations)} actual user operations, {len(test.frames)} desktop frames, {test.dpi} DPI")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
