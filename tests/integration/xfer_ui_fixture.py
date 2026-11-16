"""Actual packaged GUI + controlled native remote serial peer; no physical UART."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
from types import SimpleNamespace
import uuid
import zipfile

from desktop_user_actions import require
from gui_global_user_actions import GlobalActions


class XferActions(GlobalActions):
    pane = None

    def key(self, vk, unicode_character=None):
        foreground = self.user.GetForegroundWindow()
        owner = w.DWORD()
        self.user.GetWindowThreadProcessId(foreground, c.byref(owner))
        require(owner.value == self.args.pid, "Keyboard left the actual application/owned dialog")
        self.args.root = foreground
        super().key(vk, unicode_character)

    def chord(self, modifier, vk):
        foreground = self.user.GetForegroundWindow()
        owner = w.DWORD()
        self.user.GetWindowThreadProcessId(foreground, c.byref(owner))
        require(owner.value == self.args.pid, "Chord left the actual application/owned dialog")
        self.args.root = foreground
        super().chord(modifier, vk)

    def check_layout(self, expanded=True):
        if not self.pane or not self.user.IsWindowVisible(self.pane):
            return super().check_layout(expanded)
        client, pane = self.client_rect(), self.rect(self.pane)
        require(pane[0] >= client[0] and pane[1] >= client[1] and
                pane[2] <= client[2] and pane[3] <= client[3], "Xfer pane outside main client")
        body = self.find_class("SerialCtlXferBody")[0]
        start = self.user.GetDlgItem(self.pane, 15)
        cancel = self.user.GetDlgItem(self.pane, 16)
        require(self.rect(start)[1] >= self.rect(body)[3] and self.rect(cancel)[1] >= self.rect(body)[3],
                "Xfer scrolling content overlaps fixed actions")
        tabs = [self.rect(self.control(i)) for i in (117, 118, 310)]
        require(len({(r[1], r[3]) for r in tabs}) == 1 and
                tabs[0][2] <= tabs[1][0] and tabs[1][2] <= tabs[2][0] and
                tabs[2][2] <= self.rect(self.control(125))[0], "Command / SFTP / Xfer tabs overlap")
        fields = [self.user.GetDlgItem(body, i) for i in (10, 11, 12, 14)]
        fields = [self.user.GetPropW(h, "SerialCtl.FieldFrame") or h for h in fields]
        heights = [self.rect(h)[3] - self.rect(h)[1] for h in fields]
        require(max(heights)-min(heights) <= 1, "Xfer visible fields have inconsistent heights: " + repr(heights))
        terminal = self.rect(self.control(107))
        require(terminal[2] < pane[0], "Xfer pane overlaps terminal")
        return {"xfer_embedded": "PASS", "fixed_footer": "PASS", "aligned_tabs": "PASS",
                "field_heights": heights}

    def find_class(self, wanted, top=False):
        matches = []
        @self.enum_callback
        def visit(h, _):
            owner = w.DWORD()
            self.user.GetWindowThreadProcessId(h, c.byref(owner))
            name = c.create_unicode_buffer(128)
            self.user.GetClassNameW(h, name, len(name))
            if owner.value == self.args.pid and name.value == wanted and self.user.IsWindowVisible(h):
                matches.append(int(h))
            return True
        if top:
            self.user.EnumWindows(visit, 0)
        else:
            self.user.EnumChildWindows(self.window, visit, 0)
        return matches

    def choose(self, combo, index):
        self.click_window(combo)
        self.wait(lambda: self.query(combo, 0x157), "native combo opened")
        self.key(0x24)
        for _ in range(index):
            self.key(0x28)
        self.key(0x0d)
        self.wait(lambda: self.query(combo, 0x147) == index and not self.query(combo, 0x157),
                  "native combo selection committed")
        time.sleep(.08)  # permit the documented deferred close-up layout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("archive", "output", "gateway"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    require(os.name == "nt" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted",
            "Requires isolated hosted Windows")
    output = args.output.resolve()
    temp = Path(os.environ["RUNNER_TEMP"]).resolve()
    require(temp in output.parents and output != temp, "Output must be a new RUNNER_TEMP child")
    output.mkdir(exist_ok=False, parents=True)
    fixture = json.loads((args.gateway.resolve().parent/"fixture-source.json").read_text(encoding="utf-8-sig"))
    source_commit = subprocess.check_output(["git","rev-parse","HEAD"], text=True).strip()
    require(fixture["source_commit"]==source_commit and
            fixture["sha256"]==hashlib.sha256(args.gateway.read_bytes()).hexdigest(),
            "Native test helper differs from the verified source/hash")
    user = c.WinDLL("user32", use_last_error=True)
    shell = c.WinDLL("shell32", use_last_error=True)
    shell.SHGetFolderPathW.argtypes = (w.HWND, c.c_int, w.HANDLE, w.DWORD, w.LPWSTR)
    config_path = c.create_unicode_buffer(260)
    require(shell.SHGetFolderPathW(None, 0x1a, None, 0, config_path) == 0, "Cannot resolve actual profile")
    config = Path(config_path.value) / "SerialCtl"
    backup = config.with_name("SerialCtl.xfer-backup-" + uuid.uuid4().hex)
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = (callback, w.LPARAM)
    user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.GetWindowThreadProcessId.argtypes = (w.HWND, c.POINTER(w.DWORD))
    user.GetDlgItem.argtypes, user.GetDlgItem.restype = (w.HWND, c.c_int), w.HWND
    user.IsWindowVisible.argtypes = user.IsWindowEnabled.argtypes = (w.HWND,)
    user.MoveWindow.argtypes = (w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL)
    user.PostMessageW.argtypes = (w.HWND, w.UINT, c.c_size_t, c.c_ssize_t)
    def mains(pid=None):
        found = []
        @callback
        def visit(h, _):
            owner = w.DWORD()
            user.GetWindowThreadProcessId(h, c.byref(owner))
            name = c.create_unicode_buffer(128)
            user.GetClassNameW(h, name, len(name))
            if name.value == "SerialCtlWin7NativeWindowV3" and (pid is None or pid == owner.value):
                found.append(int(h))
            return True
        user.EnumWindows(visit, 0)
        return found
    require(not mains(), "Existing GUI; refusing profile changes")
    process = peer = desktop = peer_log = None
    moved = created = False
    original_display = None
    report = {"status": "RUNNING", "cases": [], "scope": "Actual ZIP executable, mouse/SendInput, "
              "native file dialogs, actual WebSocket remote serial with controlled native protocol peer "
              "(10ms fixture pacing per peer output/save for visible in-progress interaction, not a UART throughput measurement)",
              "notRun": ["physical UART/board", "real Win7", "device power", "field SSH"],
              "fixtureSource":fixture,
              "releaseQualification":"Determined by the parent workflow; diagnostic ZIPs cannot qualify a formal release"}
    try:
        # This fixture also runs independently of smoke-ui. Ensure the actual
        # hosted screen can contain the declared one-shot border drags.
        user.EnumDisplaySettingsW.argtypes=(w.LPCWSTR,w.DWORD,c.c_void_p)
        user.EnumDisplaySettingsW.restype=w.BOOL
        user.ChangeDisplaySettingsW.argtypes=(c.c_void_p,w.DWORD)
        user.ChangeDisplaySettingsW.restype=w.LONG
        user.GetSystemMetrics.argtypes=(c.c_int,)
        mode=c.create_string_buffer(220)
        struct.pack_into("<H",mode,68,220)
        require(user.EnumDisplaySettingsW(None,0xffffffff,mode),"Cannot read actual hosted display")
        original_display=mode.raw
        report["displayBefore"]=[user.GetSystemMetrics(0),user.GetSystemMetrics(1)]
        for width,height in ((1920,1080),(1280,1024)):
            struct.pack_into("<I",mode,72,0x00180000)
            struct.pack_into("<II",mode,172,width,height)
            if user.ChangeDisplaySettingsW(mode,0)==0:
                break
        report["displayDuring"]=[user.GetSystemMetrics(0),user.GetSystemMetrics(1)]
        require(report["displayDuring"][0]>=1280 and report["displayDuring"][1]>=900,
                "Hosted screen cannot contain the actual Xfer resize test")
        app = output / "app"
        app.mkdir()
        with zipfile.ZipFile(args.archive) as archive:
            for name in ("serialctl.exe", "plink.exe", "psftp.exe"):
                (app / name).write_bytes(archive.read(name))
        report["archiveSha256"] = hashlib.sha256(args.archive.read_bytes()).hexdigest()
        report["programSha256"] = hashlib.sha256((app / "serialctl.exe").read_bytes()).hexdigest()
        (app / "update.ini").write_text("[update]\nurl=\n", encoding="ascii")
        if config.exists():
            require(not config.is_symlink() and not (os.lstat(config).st_file_attributes & 0x400),
                    "Linked configuration is unsupported")
            os.replace(config, backup)
            moved = True
        config.mkdir()
        created = True
        (config / "settings.ini").write_text("[Pet]\nEnabled=0\n[Layout]\nRightPanelCollapsed=0\n", encoding="ascii")
        process = subprocess.Popen([str(app / "serialctl.exe")], cwd=app)
        deadline = time.monotonic() + 15
        while not mains(process.pid):
            require(time.monotonic() < deadline, "Packaged GUI startup deadline")
            time.sleep(.02)
        hwnd = mains(process.pid)[0]
        user.MoveWindow(hwnd, 0, 0, 1044, 760, True)  # initial fixture placement only
        desktop = XferActions(SimpleNamespace(window=hwnd, root=hwnd, pid=process.pid,
                    output=str(output), mode="desktop", prefix="v219-xfer"))
        desktop.user.SetForegroundWindow(hwnd)
        desktop.wait(lambda: desktop.user.IsWindowVisible(desktop.control(310)), "Xfer tab is always present")
        require(not desktop.user.IsWindowEnabled(desktop.control(310)), "Disconnected Xfer must be disabled")
        desktop.capture("disconnected-disabled")
        desktop.click_window(desktop.control(300))
        desktop.wait(lambda: desktop.user.IsWindowVisible(desktop.control(105)), "real CMD opens")
        require(not desktop.user.IsWindowEnabled(desktop.control(310)), "CMD Xfer must be disabled")
        desktop.capture("cmd-disabled")
        desktop.click_window(desktop.control(105))
        payload = bytes((i * 173 + i // 7) & 255 for i in range(65539))
        source = output / "中文文件.bin"
        source.write_bytes(payload)
        for protocol in range(4):
            for direction in range(2):
                label = f"p{protocol}-d{direction}"
                peer_report = output / (label + "-peer.json")
                peer_log = (output / (label + "-peer.log")).open("wb")
                peer = subprocess.Popen([str(args.gateway.resolve()), "7014", str(protocol),
                       "receive" if direction == 0 else "send", str(source), str(peer_report), "10"],
                       stdout=peer_log, stderr=subprocess.STDOUT)
                desktop.wait(lambda: peer_report.exists() and peer_report.stat().st_size > 0,
                             "controlled peer listening")
                desktop.click_window(desktop.control(101))
                dialogs = []
                def active_dialog():
                    dialogs[:] = [h for h in desktop.find_class("#32770", top=True)
                                  if desktop.user.IsWindowEnabled(h)]
                    return bool(dialogs)
                desktop.wait(active_dialog, "remote serial form")
                connection = dialogs[-1]
                desktop.choose(desktop.user.GetDlgItem(connection, 1036), 1)
                desktop.click_window(desktop.user.GetDlgItem(connection, 1014))
                desktop.text("127.0.0.1")
                desktop.wait(lambda: desktop.user.IsWindowEnabled(desktop.user.GetDlgItem(connection, 1)),
                             "real discovery selects controlled COM77", 15)
                desktop.capture(label + "-remote-ready")
                desktop.click_window(desktop.user.GetDlgItem(connection, 1))
                desktop.wait(lambda: desktop.user.IsWindowEnabled(desktop.control(310)), "serial Xfer enabled")
                desktop.click_window(desktop.control(310))
                desktop.wait(lambda: desktop.find_class("SerialCtlXferPane"), "embedded Xfer")
                desktop.pane = desktop.find_class("SerialCtlXferPane")[0]
                body = desktop.find_class("SerialCtlXferBody")[0]
                desktop.choose(desktop.user.GetDlgItem(body, 10), direction)
                desktop.choose(desktop.user.GetDlgItem(body, 11), protocol)
                if direction == 1 and protocol < 2:
                    desktop.click_window(desktop.user.GetDlgItem(body, 14))
                    desktop.chord(0x11, ord("A"))
                    desktop.text(str(len(payload)))
                target = source if direction == 0 else output / (label + "-接收.bin")
                desktop.click_window(desktop.user.GetDlgItem(body, 13))
                desktop.wait(lambda: desktop.user.GetForegroundWindow() != hwnd, "native file picker")
                desktop.chord(0x12, ord("N"))  # English hosted Windows filename accelerator
                desktop.chord(0x11, ord("A"))
                desktop.text(str(target))
                desktop.key(0x0d)
                desktop.wait(lambda: desktop.user.GetForegroundWindow() == hwnd, "file picker returns to Xfer")
                desktop.wait(lambda: desktop.native_control_text(desktop.user.GetDlgItem(body, 12)) == str(target),
                             "chosen file path retained")
                layout = desktop.check_layout()
                desktop.capture(label + "-ready")
                desktop.click_window(desktop.user.GetDlgItem(desktop.pane, 15))
                response_samples=[]
                if protocol==1 and direction==0:
                    desktop.wait(lambda: desktop.user.IsWindowEnabled(desktop.user.GetDlgItem(desktop.pane,16)),
                                 "controlled paced transfer is in progress")
                    desktop.capture(label+"-in-progress")
                    button=desktop.rect(desktop.user.GetDlgItem(desktop.pane,15))
                    for _ in range(5):
                        # Actual repeated mouse presses on the disabled action
                        # must not create a second file operation or send again.
                        desktop.click(((button[0]+button[2])//2,(button[1]+button[3])//2))
                        response_samples.append(desktop.responsive())
                    desktop.resize(980,680,"xfer-active-resize")
                    response_samples.append(desktop.responsive())
                    desktop.resize(1044,760,"xfer-active-restore")
                    require(max(response_samples)<100,"Xfer blocked GUI responsiveness during repeated input")
                def completed():
                    if not peer_report.exists():
                        return False
                    try:
                        result = json.loads(peer_report.read_text())
                    except (ValueError, OSError):
                        return False
                    require(result["status"] != "FAIL", "Controlled peer failed: " + str(result))
                    return result["status"] == "PASS" and not desktop.user.IsWindowEnabled(
                        desktop.user.GetDlgItem(desktop.pane, 16))
                desktop.wait(completed, "actual GUI protocol completion", 30)
                status = desktop.native_control_text(desktop.user.GetDlgItem(body, 18))
                require("失败" not in status and ("已完成" in status or "已保存" in status or "已写入" in status),
                        "GUI did not report actual successful completion: " + status)
                if direction == 1:
                    require(target.read_bytes() == payload, "GUI received file bytes differ")
                desktop.capture(label + "-complete")
                if protocol == 0 and direction == 0:
                    desktop.resize(980, 680, "xfer-narrow")
                    desktop.capture("narrow")
                    desktop.resize(1044, 760, "xfer-restored")
                report["cases"].append({"protocol": protocol, "direction": direction, "status": "PASS",
                    "bytes": len(payload), "peer": json.loads(peer_report.read_text()), "uiStatus": status, "layout": layout,
                    "activeWmNullSamplesMs":response_samples})
                desktop.click_window(desktop.control(105))
                desktop.wait(lambda: not desktop.user.IsWindowEnabled(desktop.control(310)), "disconnect disables Xfer")
                desktop.pane = None
                peer.terminate()
                peer.wait(timeout=10)
                peer = None
                peer_log.close()
            if protocol == 1:
                desktop.click_window(desktop.control(104))
                time.sleep(.15)
        user.PostMessageW(hwnd, 0x10, 0, 0)
        process.wait(timeout=15)
        require(process.returncode == 0, "Program did not exit cleanly")
        process = None
        report.update(status="PASS", measuredDpi=desktop.dpi, frames=desktop.frames)
        desktop.report["status"] = "PASS"
    except BaseException as exc:
        report.update(status="FAIL", error=str(exc))
        if desktop:
            desktop.report.update(status="FAIL", error=str(exc))
            try:
                desktop.capture("failure")
            except Exception:
                pass
        raise
    finally:
        for child in (process, peer):
            if child and child.poll() is None:
                child.terminate()
                child.wait(timeout=10)
        if peer_log:
            peer_log.close()
        if desktop:
            desktop.flush_frames()
        if created and config.exists():
            shutil.rmtree(config)
        if moved:
            os.replace(backup, config)
        if original_display is not None:
            restored=c.create_string_buffer(original_display,len(original_display))
            require(user.ChangeDisplaySettingsW(restored,0)==0,"Cannot restore hosted display mode")
        (output / "xfer-ui-result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({k:v for k,v in report.items() if k != "frames"}, ensure_ascii=True), flush=True)


if __name__ == "__main__":
    main()
