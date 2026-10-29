"""Run the released V2.0.9 updater against the exact signed candidate on Windows."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import functools
import hashlib
import http.server
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import threading
import time
import zipfile


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--old-zip', type=Path, required=True)
    parser.add_argument('--new-zip', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert os.name == 'nt', 'Actual released Windows binaries are required'
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    install = root / 'installation'
    install.mkdir()
    report = {'status': 'RUNNING', 'old_release': 'v2.0.9', 'requests': [],
              'scope': 'Real released V2.0.9 GUI/updater and exact signed V2.0.10 candidate; loopback HTTP; no real intranet or Win7 hardware'}
    payload = json.loads(json.loads(args.metadata.read_bytes())['payload'])
    assert payload['version'] == 'V2.0.10'
    assert digest(args.old_zip) == 'e065555b4d10132d66fa59443a0aa883199c37bdeb61242213470efed3f4f2df'
    assert digest(args.new_zip) == payload['archive']['sha256']
    with zipfile.ZipFile(args.old_zip) as archive:
        for member in archive.infolist():
            parts = PurePosixPath(member.filename.replace('\\', '/')).parts
            if len(parts) == 1 and not member.is_dir():
                (install / parts[0]).write_bytes(archive.read(member))
    report['old_exe_sha256'] = digest(install / 'serialctl.exe')
    assert report['old_exe_sha256'] == '8e6cf1ba3d528935ba3417216a753536e0c4f4b129eaa3892c86ae9df07f1104'
    report['new_archive_sha256'] = digest(args.new_zip)
    site = root / 'site'
    site.mkdir()
    shutil.copyfile(args.metadata, site / 'update.json')
    shutil.copyfile(args.new_zip, site / payload['archive']['url'])

    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            report['requests'].append({'path': self.path, 'time': time.time()})
            super().do_GET()

        def log_message(self, *unused):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(site)))
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    config = ('[update]\r\nurl=http://127.0.0.1:' + str(server.server_port) + '/update.json\r\n').encode('ascii')
    (install / 'update.ini').write_bytes(config)
    protected = {'keep-user.txt': b'preserve existing user file\n', 'drivers/keep-driver.bin': b'preserve existing driver\n'}
    for name, data in protected.items():
        target = install / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    env = os.environ.copy()
    profile = root / 'profile'
    profile.mkdir()
    env['LOCALAPPDATA'] = str(profile)
    env['APPDATA'] = str(profile)

    user, kernel = c.WinDLL('user32'), c.WinDLL('kernel32')
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = (callback, w.LPARAM)
    user.GetDlgItem.argtypes, user.GetDlgItem.restype = (w.HWND, c.c_int), w.HWND
    user.IsWindowEnabled.argtypes = (w.HWND,)
    user.GetWindowTextW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.IsWindowVisible.argtypes = (w.HWND,)
    user.GetWindowThreadProcessId.argtypes = (w.HWND, c.POINTER(w.DWORD))
    user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.PostMessageW.argtypes = (w.HWND, w.UINT, w.WPARAM, w.LPARAM)
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = (w.DWORD, w.BOOL, w.DWORD), w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = (w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD))
    kernel.CloseHandle.argtypes = (w.HANDLE,)

    def windows():
        found = []
        @callback
        def each(hwnd, unused):
            pid = w.DWORD()
            user.GetWindowThreadProcessId(hwnd, c.byref(pid))
            handle = kernel.OpenProcess(0x1000, False, pid.value)
            if handle:
                try:
                    path, length = c.create_unicode_buffer(32768), w.DWORD(32768)
                    if kernel.QueryFullProcessImageNameW(handle, 0, path, c.byref(length)):
                        actual = Path(path.value).resolve()
                        if actual == install / 'serialctl.exe' and user.IsWindowVisible(hwnd):
                            name = c.create_unicode_buffer(128)
                            user.GetClassNameW(hwnd, name, len(name))
                            found.append({'window': int(hwnd), 'pid': pid.value, 'class': name.value})
                finally:
                    kernel.CloseHandle(handle)
            return True
        user.EnumWindows(each, 0)
        return found

    process = None
    try:
        process = subprocess.Popen([str(install / 'serialctl.exe')], cwd=install, env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        report['old_pid'] = process.pid
        confirmed = False
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            if not confirmed:
                for item in windows():
                    title = c.create_unicode_buffer(128)
                    user.GetWindowTextW(item['window'], title, len(title))
                    button = user.GetDlgItem(item['window'], 1)
                    if item['pid'] == process.pid and title.value == '软件更新' and button and user.IsWindowEnabled(button):
                        time.sleep(.5)
                        assert not any(x['path'] == '/' + payload['archive']['url'] for x in report['requests']), 'ZIP downloaded before confirmation'
                        report['confirmation'] = {'shown': True, 'no_zip_before_confirm': True, 'method': 'native button BM_CLICK', 'time': time.time()}
                        assert user.PostMessageW(button, 0x00F5, 0, 0), 'Cannot activate confirmation'
                        confirmed = True
                        break
            logs = list((install / '.serialctl-update').glob('*/update.log'))
            evidence = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in logs)
            if 'COMMITTED V2.0.10' in evidence:
                break
            time.sleep(.25)
        else:
            raise AssertionError('Released V2.0.9 did not commit the signed V2.0.10 update within 150 seconds')
        assert confirmed, 'Update bypassed mandatory confirmation'
        report['update_logs'] = evidence
        report['old_exit_code'] = process.wait(timeout=15)
        assert report['old_exit_code'] == 0
        for item in payload['files']:
            target = install / item['name']
            assert target.stat().st_size == item['size'] and digest(target) == item['sha256'], item['name']
        assert (install / 'update.ini').read_bytes() == config, 'Updater replaced existing update.ini'
        for name, data in protected.items():
            assert (install / name).read_bytes() == data, name
        finalization = time.monotonic() + 5
        while (install / '.serialctl-update/transaction.txt').exists() and time.monotonic() < finalization:
            time.sleep(.05)
        assert not (install / '.serialctl-update/transaction.txt').exists(), 'Committed journal not finalized'
        assert not (install / '.serialctl-update/failed-version.txt').exists(), 'Upgrade was quarantined'
        actual = windows()
        assert any(x['class'] == 'SerialCtlWin7NativeWindowV3' and x['pid'] != process.pid for x in actual), 'Replacement GUI is not running'
        replacement = next(x for x in actual if x['class'] == 'SerialCtlWin7NativeWindowV3')
        user.ShowWindow.argtypes = (w.HWND, c.c_int)
        user.IsIconic.argtypes = (w.HWND,)
        user.GetWindow.argtypes, user.GetWindow.restype = (w.HWND, c.c_uint), w.HWND
        user.GetWindowLongPtrW.argtypes, user.GetWindowLongPtrW.restype = (w.HWND, c.c_int), c.c_ssize_t
        user.ShowWindow(replacement['window'], 6)
        time.sleep(.2)
        second = subprocess.Popen([str(install / 'serialctl.exe')], cwd=install, env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert second.wait(timeout=10) == 0, 'Repeated launch did not exit normally'
        deadline = time.monotonic() + 5
        while user.IsIconic(replacement['window']) and time.monotonic() < deadline:
            time.sleep(.05)
        assert not user.IsIconic(replacement['window']), 'Repeated launch did not restore the minimized main window'
        assert len([x for x in windows() if x['class'] == 'SerialCtlWin7NativeWindowV3']) == 1, 'Repeated launch created another GUI'
        report['single_instance'] = {'second_pid': second.pid, 'existing_pid': replacement['pid'],
                                     'second_exit': 0, 'same_gui_restored': True}
        # Actual packaged AI page and unowned companion, without connecting a
        # model/server or sending any device operation.
        assert user.PostMessageW(replacement['window'], 0x0111, 304, 0)
        deadline = time.monotonic() + 10
        cancelled = False
        pet = None
        while time.monotonic() < deadline:
            current = windows()
            for entry in current:
                if entry['pid'] == replacement['pid'] and entry['class'] == '#32770' and user.GetDlgItem(entry['window'], 200):
                    user.PostMessageW(entry['window'], 0x0111, 2, 0)
                    cancelled = True
            pet = next((x for x in current if x['pid'] == replacement['pid'] and x['class'] == 'SerialCtlAiPetPane'), None)
            if pet and cancelled:
                break
            time.sleep(.1)
        assert pet and cancelled, 'Packaged AI did not expose its desktop companion after canceling setup'
        assert not user.GetWindow(pet['window'], 4), 'Pet still has a minimizing owner'
        assert not (user.GetWindowLongPtrW(pet['window'], -16) & 0x40000000), 'Desktop pet is a child window'
        time.sleep(.5)
        def capture(name):
            script = """Add-Type -AssemblyName System.Drawing
+Add-Type -AssemblyName System.Windows.Forms
+$r=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
+$b=New-Object System.Drawing.Bitmap($r.Width,$r.Height)
+$g=[System.Drawing.Graphics]::FromImage($b)
+try {$g.CopyFromScreen(0,0,0,0,$r.Size);$b.Save($env:SERIALCTL_CAPTURE_FILE,[System.Drawing.Imaging.ImageFormat]::Png)} finally {$g.Dispose();$b.Dispose()}
+""".replace('\n+', '\n')
            script_path = root / 'capture-desktop.ps1'
            script_path.write_text(script, encoding='utf-8-sig')
            capture_env = os.environ.copy()
            capture_env['SERIALCTL_CAPTURE_FILE'] = str(root / name)
            subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-File', str(script_path)],
                           env=capture_env, check=True, timeout=20)
        capture('v210-packaged-ai-desktop.png')
        user.ShowWindow(replacement['window'], 6)
        time.sleep(.5)
        assert user.IsIconic(replacement['window']) and user.IsWindowVisible(pet['window']), 'Minimizing GUI hid the desktop companion'
        capture('v210-packaged-minimized-desktop.png')
        report['desktop_pet'] = {'window': pet['window'], 'unowned': True, 'minimized_main_stays_visible': True}
        user.ShowWindow(replacement['window'], 9)
        paths = [x['path'] for x in report['requests']]
        assert '/update.json' in paths and '/' + payload['archive']['url'] in paths
        assert all(path in ('/update.json', '/' + payload['archive']['url']) for path in paths), paths
        report.update(status='PASS', new_exe_sha256=digest(install / 'serialctl.exe'),
                      installed_files_verified=len(payload['files']), config_preserved=True,
                      user_and_driver_markers_preserved=True, replacement_windows=actual)
    except Exception as error:
        report.update(status='FAIL', error=str(error), windows=windows())
        report['update_logs'] = '\n'.join(p.read_text(encoding='utf-8', errors='replace')
                                         for p in (install / '.serialctl-update').glob('*/update.log'))
        raise
    finally:
        (root / 'upgrade-result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(report, ensure_ascii=True), flush=True)
        for item in windows():
            user.PostMessageW(item['window'], 0x10, 0, 0)
        if process is not None and process.poll() is None:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
