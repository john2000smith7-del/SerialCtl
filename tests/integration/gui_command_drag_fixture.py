"""Run the real packaged GUI's drag test on an ephemeral GitHub-hosted runner.

This fixture backs up the actual CSIDL_APPDATA SerialCtl directory, because
setting APPDATA does not reliably affect SHGetFolderPathW. It refuses to run on
self-hosted runners or when the current user's GUI mutex already exists.
"""
import argparse
import csv
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time
import traceback
import uuid
import zipfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(os.name == 'nt', 'This fixture needs interactive Windows')
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and
            os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted',
            'Refusing to change configuration outside an ephemeral GitHub-hosted runner')
    runner_temp = Path(os.environ['RUNNER_TEMP']).resolve()
    output = args.output.resolve()
    require(output != runner_temp and runner_temp in output.parents,
            'Output must be a new child of RUNNER_TEMP')
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'RUNNING', 'runner_environment': 'github-hosted',
              'scope': 'actual packaged program, system mouse, temporary CSIDL_APPDATA fixture',
              'existing_configuration_restored': False}
    app = output / 'app'
    app.mkdir()
    user = c.WinDLL('user32', use_last_error=True)
    shell = c.WinDLL('shell32', use_last_error=True)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    shell.SHGetFolderPathW.argtypes = (w.HWND, c.c_int, w.HANDLE, w.DWORD, w.LPWSTR)
    shell.SHGetFolderPathW.restype = c.c_long
    appdata = c.create_unicode_buffer(260)
    require(shell.SHGetFolderPathW(None, 0x1A, None, 0, appdata) == 0 and appdata.value,
            'Unable to resolve the actual roaming profile directory')
    actual_config = Path(appdata.value) / 'SerialCtl'
    backup = actual_config.with_name('SerialCtl.fixture-backup-' + uuid.uuid4().hex)
    marker = actual_config / '.serialctl-ci-fixture-marker'
    report['actual_config'] = str(actual_config)
    report['appdata_environment'] = os.environ.get('APPDATA', '')
    report['profile_matches_environment'] = Path(os.environ.get('APPDATA', appdata.value)).resolve() == Path(appdata.value).resolve()
    report['existing_configuration'] = actual_config.exists()
    sid_rows = list(csv.reader(subprocess.check_output(['whoami', '/user', '/fo', 'csv', '/nh'], text=True).splitlines()))
    sid = next((part.strip() for row in sid_rows for part in row if part.strip().startswith('S-1-')), None)
    require(sid, 'Cannot determine the user identity needed to check the GUI instance mutex')
    kernel.OpenMutexW.argtypes, kernel.OpenMutexW.restype = (w.DWORD, w.BOOL, w.LPCWSTR), w.HANDLE
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    c.set_last_error(0)
    mutex = kernel.OpenMutexW(0x00100000, False, 'Local\\SerialCtl.Gui.v1.' + sid)
    if mutex:
        kernel.CloseHandle(mutex)
        raise RuntimeError('Existing SerialCtl GUI instance detected; refusing fixture')
    require(c.get_last_error() == 2, 'Cannot safely determine whether a SerialCtl instance is running')
    if actual_config.exists():
        attributes = getattr(os.lstat(actual_config), 'st_file_attributes', 0)
        require(not actual_config.is_symlink() and not (attributes & 0x400),
                'Refusing to move a linked/reparse-point configuration directory')
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes, user.EnumWindows.restype = (callback, w.LPARAM), w.BOOL
    user.GetWindowThreadProcessId.argtypes = (w.HWND, c.POINTER(w.DWORD))
    user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.IsWindowVisible.argtypes = (w.HWND,)
    user.GetDlgItem.argtypes, user.GetDlgItem.restype = (w.HWND, c.c_int), w.HWND
    user.SetForegroundWindow.argtypes = (w.HWND,)
    user.GetForegroundWindow.restype = w.HWND
    user.GetSystemMetrics.argtypes = (c.c_int,)
    user.GetSystemMetrics.restype = c.c_int
    user.MoveWindow.argtypes = (w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL)
    user.MoveWindow.restype = w.BOOL
    def windows(pid=None):
        matches = []
        @callback
        def each(hwnd, _):
            owner = w.DWORD()
            user.GetWindowThreadProcessId(hwnd, c.byref(owner))
            name = c.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, name, len(name))
            if name.value == 'SerialCtlWin7NativeWindowV3' and (pid is None or owner.value == pid):
                matches.append(int(hwnd))
            return True
        require(user.EnumWindows(each, 0), 'Cannot enumerate GUI instances')
        return matches
    require(not windows(), 'A SerialCtl main window already exists; refusing fixture')
    backup_taken = created = False
    process = None
    failure = None
    try:
        with zipfile.ZipFile(args.archive) as archive:
            for item in archive.infolist():
                name = item.filename.replace('\\', '/')
                parts = PurePosixPath(name).parts
                require(name and not name.startswith('/') and all(p not in ('..', '.') and ':' not in p for p in parts),
                        'Unsafe ZIP path')
            archive.extractall(app)
        require(all((app / n).is_file() for n in ('serialctl.exe', 'plink.exe', 'psftp.exe', 'update.ini')),
                'Verified package does not contain root executables and update.ini')
        (app / 'update.ini').write_text('[update]\r\nurl=\r\n', encoding='ascii')
        if actual_config.exists():
            os.replace(actual_config, backup)
            backup_taken = True
        actual_config.mkdir()
        created = True
        marker.write_text('Temporary isolated GitHub-hosted CI fixture\n', encoding='ascii')
        commands = ''.join(f'#Item {i:02d}\r\necho ITEM_{i:02d}\r\n' for i in range(1, 21)).encode('ascii')
        (actual_config / 'commands.txt').write_bytes(commands)
        (actual_config / 'settings.ini').write_text('[Layout]\r\nRightPanelWidth=320\r\nRightPanelCollapsed=0\r\n[Pet]\r\nEnabled=0\r\n', encoding='ascii')
        process = subprocess.Popen([str(app / 'serialctl.exe')], cwd=app,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 25
        main_window = None
        while time.monotonic() < deadline:
            require(process.poll() is None, 'Packaged process exited before creating a window')
            items = windows(process.pid)
            if items and user.IsWindowVisible(items[0]) and user.GetDlgItem(items[0], 112):
                main_window = items[0]
                break
            time.sleep(.1)
        require(main_window, 'Packaged program did not expose its main window')
        width = user.GetSystemMetrics(0)
        height = user.GetSystemMetrics(1)
        require(width >= 980 and height >= 620, 'Interactive CI desktop is too small')
        require(user.MoveWindow(main_window, 0, 0, min(width, 1200), min(height - 30, 850), True),
                'Could not size production window within the visible desktop')
        user.SetForegroundWindow(main_window)
        time.sleep(.3)
        require(user.GetForegroundWindow() == main_window, 'Production window cannot be foregrounded')
        script = Path(__file__).resolve().with_name('gui_command_drag.py')
        command = [sys.executable, str(script), '--window', str(main_window), '--pid', str(process.pid),
                   '--output', str(output), '--prefix', 'real-command', '--edge-scroll']
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace', timeout=240)
        (output / 'drag-test-output.txt').write_text(result.stdout, encoding='utf-8')
        print(result.stdout, flush=True)
        require(result.returncode == 0, 'Real desktop command drag regression failed')
        require((actual_config / 'commands.txt').read_bytes() == commands,
                'Real mouse test unexpectedly modified persistent commands')
        report['mouse_test'] = 'PASS'
        report['persisted_command_bytes_unchanged'] = True
    except Exception as error:
        failure = error
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        try:
            if created:
                require(marker.is_file(), 'Fixture marker missing; refusing to delete unknown configuration')
                shutil.rmtree(actual_config)
            if backup_taken:
                require(not actual_config.exists(), 'Configuration destination is occupied; backup retained')
                os.replace(backup, actual_config)
            report['existing_configuration_restored'] = True
            report['status'] = 'FAIL' if failure else 'PASS'
        except Exception as cleanup_error:
            report['cleanup_error'] = str(cleanup_error)
            report['backup_location'] = str(backup) if backup_taken else None
            report['status'] = 'FAIL'
            failure = failure or cleanup_error
        (output / 'fixture-report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
        print(json.dumps(report, ensure_ascii=True), flush=True)
    return 1 if failure else 0


if __name__ == '__main__':
    raise SystemExit(main())
