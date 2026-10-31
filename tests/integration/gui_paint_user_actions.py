"""Record actual SerialCtl sidebar input and screen pixels on Windows.

The caller owns startup, real CMD output or an explicitly isolated SFTP child
process fixture, the current theme, and cleanup. This script does not replace
application windows, synthesize paint, or send button/command notifications.
Its video is a lossless RGB recording of a declared screen-DC rectangle. Video
playback uses a nominal frame rate; the JSONL timestamps, including missed
sampling deadlines and unencoded samples, are the actual measurement clock.
"""

import argparse
import contextlib
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
import queue
import struct
import subprocess
import threading
import time
import traceback
import zlib

from desktop_user_actions import (BITMAPINFO, BITMAPINFOHEADER, DesktopActions,
                                  POINT, require)


def write_png(path, raw, width, height):
    """Store the unchanged screen RGB bytes; the unused DIB alpha is omitted."""
    rgb = bytearray(width * height * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
    stride = width * 3
    scanlines = b"".join(b"\0" + rgb[row * stride:(row + 1) * stride]
                         for row in range(height))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(scanlines, 1)) + chunk(b"IEND", b""))
    path.write_bytes(png)
    return hashlib.sha256(png).hexdigest()


def bright_rectangles(raw, width, height, scale=1.0):
    """Find large neutral light patches, not text, in a dark sidebar.

    A candidate requires three adjacent sampled rows each containing at least
    eight consecutive neutral >=190 RGB samples. At 96 DPI this covers 32x12
    pixels on a 4px sampling grid. It is a deliberately narrow regression oracle
    for #31's flat light rectangles, not a general assertion of visual quality.
    Reported rectangles are candidate grid cells, not a measured patch boundary.
    """
    step = max(4, round(4 * scale))
    columns = (width - 1) // step + 1
    vertical = [0] * columns
    found = []
    for y in range(0, height, step):
        run = 0
        for column, x in enumerate(range(0, width, step)):
            offset = (y * width + x) * 4
            blue, green, red = raw[offset:offset + 3]
            light = min(blue, green, red) >= 190 and max(blue, green, red) - min(blue, green, red) <= 16
            run = run + 1 if light else 0
            vertical[column] = vertical[column] + 1 if run >= 8 else 0
            if vertical[column] >= 3:
                found.append([max(0, x - 7 * step), max(0, y - 2 * step),
                              min(width, x + step), min(height, y + step)])
                if len(found) == 16:
                    return found
    return found


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))], 3)


class ScreenRecording:
    """Independent screen sampler and bounded, lossless encoding consumer."""

    def __init__(self, actions, bounds, ffmpeg, fps, light_patch_oracle=True):
        self.actions = actions
        self.bounds = bounds
        self.width, self.height = bounds[2] - bounds[0], bounds[3] - bounds[1]
        self.ffmpeg, self.fps = ffmpeg, fps
        self.light_patch_oracle = light_patch_oracle
        self.stem = f"{actions.args.prefix}-{actions.args.mode}-paint"
        self.video = actions.output / (self.stem + ".mkv")
        self.log = actions.output / (self.stem + "-ffmpeg.log")
        self.frames = []
        self.samples = []
        self.failures = []
        self.queue = queue.Queue(maxsize=48)
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.phase = "initial"
        self.sample_thread = None
        self.encode_thread = None
        self.sampling_error = None
        self.encoder_error = None
        self.missed_deadlines = 0
        self.queue_drops = 0
        self.counterexamples = 0
        self.saved_phases = set()
        self.started_at = None
        self.finished_at = None

    def start(self):
        self.encode_thread = threading.Thread(target=self._encode, name="screen-ffv1", daemon=True)
        self.sample_thread = threading.Thread(target=self._sample, name="screen-bitblt", daemon=True)
        self.encode_thread.start()
        self.sample_thread.start()
        require(self.ready.wait(8), "Screen sampler did not become ready")
        require(self.sampling_error is None, str(self.sampling_error))
        self.ensure_alive()

    def ensure_alive(self):
        require(self.sampling_error is None, "Screen capture failed: " + str(self.sampling_error))
        require(self.encoder_error is None, "Lossless encoder failed: " + str(self.encoder_error))

    def _sample(self):
        a = self.actions
        dc = memory = bitmap = previous = None
        try:
            dc = a.user.GetDC(None)
            require(dc, "Cannot acquire actual visible desktop DC")
            memory = a.gdi.CreateCompatibleDC(dc)
            require(memory, "Cannot allocate screen capture memory DC")
            info = BITMAPINFO()
            info.header.size = c.sizeof(BITMAPINFOHEADER)
            info.header.width, info.header.height = self.width, -self.height
            info.header.planes, info.header.bit_count = 1, 32
            pixels = c.c_void_p()
            bitmap = a.gdi.CreateDIBSection(dc, c.byref(info), 0, c.byref(pixels), None, 0)
            require(bitmap and pixels.value, "Cannot allocate screen capture DIB")
            previous = a.gdi.SelectObject(memory, bitmap)
            require(previous, "Cannot select screen capture DIB")
            self.started_at = time.perf_counter()
            self.ready.set()
            deadline = self.started_at
            while not self.stop_event.is_set():
                now = time.perf_counter()
                if now < deadline:
                    self.stop_event.wait(deadline - now)
                    continue
                skipped = max(0, int((now - deadline) * self.fps))
                self.missed_deadlines += skipped
                deadline += (skipped + 1) / self.fps
                phase = self.phase
                started = time.perf_counter()
                require(a.gdi.BitBlt(memory, 0, 0, self.width, self.height,
                                    dc, self.bounds[0], self.bounds[1], 0x40CC0020),
                        "Actual desktop BitBlt failed")
                require(a.gdi.GdiFlush(), "Cannot synchronize captured DIB pixels")
                raw = c.string_at(pixels, self.width * self.height * 4)
                captured = time.perf_counter()
                frame = {
                    "sample_index": len(self.frames), "phase": phase,
                    "seconds": round(started - a.started, 6),
                    "capture_end_seconds": round(captured - a.started, 6),
                    "capture_ms": round((captured - started) * 1000, 3),
                    "missed_deadlines_before_sample": skipped,
                    "encoded_index": None,
                    "terminal_stdout_marker": int(a.user.GetPropW(a.control(107), "SerialCtl.PaintProbe") or 0),
                    "sidebar_toggle_bounds": a.rect(a.control(125)),
                }
                self.frames.append(frame)
                try:
                    self.queue.put_nowait((frame, raw))
                except queue.Full:
                    frame["not_encoded"] = "bounded encoder queue full"
                    self.queue_drops += 1
        except Exception:
            self.sampling_error = traceback.format_exc()
            self.ready.set()
        finally:
            self.finished_at = time.perf_counter()
            if previous:
                a.gdi.SelectObject(memory, previous)
            if bitmap:
                a.gdi.DeleteObject(bitmap)
            if memory:
                a.gdi.DeleteDC(memory)
            if dc:
                a.user.ReleaseDC(None, dc)

    def _encode(self):
        process = None
        try:
            command = [self.ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
                       "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size",
                       f"{self.width}x{self.height}", "-framerate", str(self.fps),
                       "-i", "pipe:0", "-an", "-c:v", "ffv1", "-level", "3",
                       "-g", "1", "-slicecrc", "1", "-pix_fmt", "bgr0", "-threads", "2",
                       str(self.video)]
            self.command = command
            with self.log.open("wb") as log:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                           stderr=log, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                encoded = 0
                while True:
                    item = self.queue.get()
                    try:
                        if item is None:
                            break
                        frame, raw = item
                        process.stdin.write(raw)
                        frame["encoded_index"] = encoded
                        encoded += 1
                        candidates = (bright_rectangles(raw, self.width, self.height, self.actions.scale)
                                      if self.light_patch_oracle and self.actions.args.theme == "dark" else [])
                        frame["neutral_light_patch_candidates"] = candidates
                        if candidates:
                            self.counterexamples += 1
                            self.failures.append({"sample_index": frame["sample_index"],
                                                  "encoded_index": frame["encoded_index"],
                                                  "seconds": frame["seconds"], "phase": frame["phase"],
                                                  "candidate_bounds_in_capture": candidates})
                        sample_phase = (frame["phase"] == "initial" or frame["phase"] == "settled-final" or
                                        any(frame["phase"] == f"normal-{direction}-{cycle:02d}"
                                            for direction in ("collapse", "expand") for cycle in (0, 9, 19)))
                        if ((candidates and self.counterexamples <= 32) or
                                (sample_phase and frame["phase"] not in self.saved_phases)):
                            name = f"{self.stem}-{frame['sample_index']:05d}-{frame['phase']}"
                            if candidates:
                                name += "-light-patch-candidate"
                            name += ".png"
                            digest = write_png(self.actions.output / name, raw, self.width, self.height)
                            entry = {"path": name, "sha256": digest, "sample_index": frame["sample_index"],
                                     "encoded_index": frame["encoded_index"], "phase": frame["phase"],
                                     "capture_bounds": self.bounds, "seconds": frame["seconds"]}
                            self.samples.append(entry)
                            self.saved_phases.add(frame["phase"])
                    finally:
                        self.queue.task_done()
                process.stdin.close()
                require(process.wait(timeout=30) == 0, "FFV1 encoder exited unsuccessfully; see encoder log")
        except Exception:
            self.encoder_error = traceback.format_exc()
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    def stop(self):
        self.stop_event.set()
        if self.sample_thread:
            self.sample_thread.join(timeout=8)
            require(not self.sample_thread.is_alive(), "Screen capture worker failed to stop")
        if self.encode_thread and self.encode_thread.is_alive():
            try:
                self.queue.put(None, timeout=15)
            except queue.Full:
                self.encoder_error = self.encoder_error or "Encoder queue remained full at shutdown"
            self.encode_thread.join(timeout=45)
            require(not self.encode_thread.is_alive(), "FFV1 encoder failed to finish")
        frame_path = self.actions.output / (self.stem + "-frames.jsonl")
        frame_path.write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in self.frames),
                              encoding="utf-8")
        intervals = [(b["seconds"] - a["seconds"]) * 1000 for a, b in zip(self.frames, self.frames[1:])]
        encoded_count = sum(row["encoded_index"] is not None for row in self.frames)
        duration = ((self.finished_at - self.started_at) if self.started_at and self.finished_at else 0)
        return {
            "capture": "GetDC(NULL) + BitBlt from actual visible desktop; no PrintWindow; cursor excluded",
            "capture_bounds": self.bounds, "width": self.width, "height": self.height,
            "requested_fps": self.fps, "sampled_frames": len(self.frames), "encoded_frames": encoded_count,
            "sampled_duration_seconds": round(duration, 6),
            "effective_sample_fps": round(len(self.frames) / duration, 3) if duration else None,
            "sampling_deadlines_missed": self.missed_deadlines, "encoder_queue_drops": self.queue_drops,
            "interval_ms": {"p50": percentile(intervals, .5), "p95": percentile(intervals, .95),
                            "p99": percentile(intervals, .99), "maximum": round(max(intervals), 3) if intervals else None},
            "video": self.video.name, "video_sha256": file_sha256(self.video) if self.video.exists() else None,
            "codec": "FFV1 level 3, RGB lossless (bgr0), intra frames, CRC; nominal-rate playback",
            "actual_timeline": frame_path.name,
            "video_timing_limitation": "Playback spacing is nominal, not proof of uniform real sampling. Use JSONL seconds/capture_end_seconds and encoded_index for actual timing; missed slots are not duplicated.",
            "ffmpeg_command": getattr(self, "command", None), "ffmpeg_log": self.log.name,
            "sampling_error": self.sampling_error, "encoder_error": self.encoder_error,
            "neutral_light_patch_oracle_enabled": self.light_patch_oracle,
            "neutral_light_patch_candidate_frames": self.counterexamples,
            "neutral_light_patch_candidates": self.failures, "saved_screen_samples": self.samples,
        }


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class PaintActions(DesktopActions):
    def __init__(self, args):
        super().__init__(args)
        self.report["theme"] = args.theme
        self.report["purpose"] = "#31 actual-screen light-block regression and #15 repeated mouse reversal"
        self.report["rapid_count"] = args.rapid_count
        self.report["normal_collapse_expand_cycles"] = args.normal_cycles
        self.report["caption_observation_budget_ms"] = 1000
        self.report["pixel_oracle"] = {
            "applies_to": "dark theme declared sidebar capture only",
            "rule": "at least 8x3 adjacent samples on a 4px-at-96-DPI grid, RGB each >=190 and max-min <=16",
            "interpretation": "large neutral light-patch candidate; retain original RGB frame for visual review",
            "limitations": "Does not detect every color artifact, text clipping, stale icons, or anomalies between samples. Light theme requires visual review.",
        }
        self.recording = None
        self.clicks = []
        self.report["sidebar_clicks"] = self.clicks

    def save(self):
        path = self.output / f"{self.args.prefix}-{self.args.mode}-paint.json"
        path.write_text(json.dumps(self.report, ensure_ascii=False, indent=2), encoding="utf-8")

    def click_sidebar(self, label, point, anchor):
        self.recording.ensure_alive()
        self.recording.phase = label
        toggle = self.control(125)
        before = self.caption(toggle)
        row = {**self.timestamp(), "phase": label, "point": list(point), "caption_before": before,
               "bounds_before_down": self.rect(toggle),
               "hit_before_down": int(self.user.WindowFromPoint(POINT(*point)) or 0)}
        self.clicks.append(row)
        require(row["bounds_before_down"] == anchor, "Sidebar toggle anchor moved before click")
        require(row["hit_before_down"] == toggle, "Sidebar target is obstructed or moved")
        self.move_mouse(point)
        self.button(True)
        time.sleep(.008)
        row["bounds_before_up"] = self.rect(toggle)
        row["hit_before_up"] = int(self.user.WindowFromPoint(POINT(*point)) or 0)
        self.button(False)
        released = time.perf_counter()
        # Observe this single release for up to one second; never re-click or
        # replace it with a command. Hosted desktops can delay input delivery.
        # The separate >=90% within-200ms reversal requirement remains intact.
        deadline = released + 1.0
        after = self.caption(toggle)
        while after == before and time.perf_counter() < deadline:
            time.sleep(.002)
            after = self.caption(toggle)
        row.update({"caption_after": after, "caption_flipped": bool(after and after != before),
                    "observed_seconds": self.timestamp()["seconds"], "bounds_after_up": self.rect(toggle),
                    "caption_latency_ms": round((time.perf_counter() - released) * 1000, 3)})
        require(row["bounds_before_up"] == anchor and row["bounds_after_up"] == anchor,
                "Sidebar toggle moved during mouse press")
        require(row["caption_flipped"], "Released sidebar click did not change target state: " + label)

    def run(self):
        require(self.user.IsWindow(self.window) and self.user.IsWindowVisible(self.window),
                "Caller must provide a visible production HWND")
        pid = w.DWORD()
        self.user.GetWindowThreadProcessId(self.window, c.byref(pid))
        require(pid.value == self.args.pid, "Window belongs to a different process")
        require(self.user.GetForegroundWindow() == self.window, "Production window must be foreground")
        expected_theme_caption = "浅色" if self.args.theme == "dark" else "深色"
        require(self.caption(self.control(104)) == expected_theme_caption,
                "Declared theme does not match actual theme button state")
        self.report["initial_layout"] = self.check_layout()
        anchor = self.rect(self.control(125))
        point = ((anchor[0] + anchor[2]) // 2, (anchor[1] + anchor[3]) // 2)
        active = 112 if self.args.mode == "desktop" else 119
        content = self.rect(self.control(active))
        client = self.client_rect()
        bounds = [max(client[0], content[0] - round(24 * self.scale)),
                  max(client[1], anchor[1] - round(16 * self.scale)),
                  client[2] - round(4 * self.scale), client[3] - round(4 * self.scale)]
        desktop = self.desktop_rect()
        require(bounds[0] >= desktop[0] and bounds[1] >= desktop[1] and
                bounds[2] <= desktop[2] and bounds[3] <= desktop[3], "Sidebar capture lies outside actual desktop")
        self.report["fixed_toggle_bounds"] = anchor
        self.report["double_click_time_ms"] = self.user.GetDoubleClickTime()
        self.capture("paint-before-full-desktop")
        self.flush_frames()
        self.recording = ScreenRecording(self, bounds, self.args.ffmpeg, self.args.fps)
        self.recording.start()
        try:
            time.sleep(.15)
            for index in range(self.args.rapid_count):
                self.click_sidebar(f"rapid-{index:03d}", point, anchor)
                time.sleep(.025)
            time.sleep(.30)
            self.report["rapid_final_layout"] = self.check_layout()
            rapid = self.clicks[:self.args.rapid_count]
            intervals = [(b["seconds"] - a["seconds"]) * 1000 for a, b in zip(rapid, rapid[1:])]
            self.report["rapid_click_interval_ms"] = {
                "p50": percentile(intervals, .5), "p95": percentile(intervals, .95),
                "minimum": round(min(intervals), 3), "maximum": round(max(intervals), 3)}
            require(sum(interval < 200 for interval in intervals) >= len(intervals) * .9,
                    "Fewer than 90% of click intervals actually reversed within 200ms animation")
            for index in range(self.args.normal_cycles):
                for collapse in (True, False):
                    label = f"normal-{'collapse' if collapse else 'expand'}-{index:02d}"
                    self.click_sidebar(label, point, anchor)
                    time.sleep(.28)
                    self.check_layout(expanded=not collapse)
            self.recording.phase = "settled-final"
            time.sleep(.15)
            self.report["final_layout"] = self.check_layout()
        finally:
            self.report["recording"] = self.recording.stop()
        recording = self.report["recording"]
        self.recording.ensure_alive()
        require(recording["encoded_frames"] > 100, "Insufficient real screen frames recorded")
        require(recording["encoder_queue_drops"] == 0, "Encoder dropped captured frames; evidence incomplete")
        frames = self.recording.frames
        require(all(frame["sidebar_toggle_bounds"] == anchor for frame in frames),
                "Independent screen sampling observed a moving toggle")
        probes = {frame["terminal_stdout_marker"] for frame in frames if frame["terminal_stdout_marker"] > 0}
        self.report["distinct_stdout_markers_during_recording"] = len(probes)
        require(len(probes) >= 4, "Caller did not supply independently changing terminal stdout markers")
        self.report["pixel_result"] = ("FAIL: neutral light patches recorded; review original counterexamples"
                                       if self.recording.counterexamples else
                                       ("No matching light patches in captured frames; visual review still required"
                                        if self.args.theme == "dark" else "Light theme: visual review required"))
        if self.recording.counterexamples:
            self.report["behavior_failures"].append("Dark sidebar contains captured neutral light-patch candidates")
        self.report["final_state"] = self.snapshot()
        self.capture("paint-after-full-desktop")
        self.report["status"] = "FAIL" if self.report["behavior_failures"] else "PASS"
        self.flush_frames()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--mode", required=True, choices=("desktop", "sftp"))
    parser.add_argument("--theme", required=True, choices=("light", "dark"))
    parser.add_argument("--ffmpeg")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--rapid-count", type=int, default=100)
    parser.add_argument("--normal-cycles", type=int, default=20)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Requires an actual interactive Windows desktop; Linux cannot satisfy this test")
    if args.rapid_count < 100 or args.rapid_count % 2 or args.normal_cycles < 20 or not 20 <= args.fps <= 120:
        parser.error("Require even rapid-count >=100, normal-cycles >=20, and fps in 20..120")
    if not args.ffmpeg:
        import imageio_ffmpeg
        args.ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    test = PaintActions(args)
    try:
        test.run()
    except Exception as error:
        test.report.update({"status": "FAIL", "error": str(error), "traceback": traceback.format_exc()})
        with contextlib.suppress(Exception):
            test.button(False)
            test.capture("paint-failure-full-desktop")
            test.flush_frames()
        print(test.report["traceback"], file=__import__("sys").stderr)
    finally:
        test.save()
    recording = test.report.get("recording", {})
    print(f"{test.report['status']} {args.prefix} {args.mode}: {len(test.clicks)} released clicks, "
          f"{recording.get('encoded_frames', 0)} lossless screen frames, {test.dpi} DPI")
    return 0 if test.report["status"] == "PASS" else (2 if test.report["behavior_failures"] else 1)


if __name__ == "__main__":
    raise SystemExit(main())
