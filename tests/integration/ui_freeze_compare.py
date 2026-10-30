"""Compare the exact baseline/candidate GUI on one ephemeral Windows desktop.

Captures actual desktop pixels at the measured system DPI. It does not simulate
other DPI values or replace the separate actual mouse/keyboard audit.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
from types import SimpleNamespace
import uuid
import zipfile
from desktop_user_actions import DesktopActions, RECT, POINT, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(os.name == 'nt' and os.environ.get('GITHUB_ACTIONS') == 'true' and
            os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted', 'Only ephemeral hosted Windows runners are supported')
    args.output.mkdir(parents=True, exist_ok=True)
    user = c.WinDLL('user32', use_last_error=True)
    shell = c.WinDLL('shell32', use_last_error=True)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = (callback, w.LPARAM)
    user.EnumChildWindows.argtypes = (w.HWND, callback, w.LPARAM)
    user.SetProcessDPIAware()
    user.GetWindowThreadProcessId.argtypes = (w.HWND, c.POINTER(w.DWORD))
    user.GetClassNameW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.GetWindowTextW.argtypes = (w.HWND, w.LPWSTR, c.c_int)
    user.SetWindowTextW.argtypes = (w.HWND, w.LPCWSTR)
    user.RedrawWindow.argtypes = (w.HWND, c.c_void_p, c.c_void_p, w.UINT)
    user.GetDlgItem.argtypes, user.GetDlgItem.restype = (w.HWND, c.c_int), w.HWND
    user.GetDlgCtrlID.argtypes = (w.HWND,)
    user.GetDlgItemTextW.argtypes = (w.HWND, c.c_int, w.LPWSTR, c.c_int)
    user.IsWindowVisible.argtypes = user.IsWindowEnabled.argtypes = (w.HWND,)
    user.GetClientRect.argtypes = user.GetWindowRect.argtypes = (w.HWND, c.POINTER(RECT))
    user.ClientToScreen.argtypes = (w.HWND, c.POINTER(POINT))
    user.MoveWindow.argtypes = (w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL)
    user.SetForegroundWindow.argtypes = (w.HWND,)
    user.PostMessageW.argtypes = (w.HWND, w.UINT, c.c_size_t, c.c_ssize_t)
    user.SendMessageW.argtypes, user.SendMessageW.restype = user.PostMessageW.argtypes, c.c_ssize_t
    user.SetFocus.argtypes = (w.HWND,)
    user.IsChild.argtypes = (w.HWND,w.HWND)
    shell.SHGetFolderPathW.argtypes = (w.HWND, c.c_int, w.HANDLE, w.DWORD, w.LPWSTR)
    profile = c.create_unicode_buffer(260)
    require(shell.SHGetFolderPathW(None, 0x1A, None, 0, profile) == 0, 'Cannot resolve test profile')
    config = Path(profile.value) / 'SerialCtl'
    backup = config.with_name('SerialCtl.freeze-backup-' + uuid.uuid4().hex)
    require(not config.is_symlink(), 'Refuse linked configuration')

    def windows(pid, classname):
        result = []
        @callback
        def each(hwnd, _):
            owner = w.DWORD(); user.GetWindowThreadProcessId(hwnd, c.byref(owner))
            name = c.create_unicode_buffer(256); user.GetClassNameW(hwnd, name, len(name))
            if (pid is None or owner.value == pid) and name.value == classname and user.IsWindowVisible(hwnd): result.append(int(hwnd))
            return True
        require(user.EnumWindows(each, 0), 'EnumWindows failed')
        return result

    require(not windows(None, 'SerialCtlWin7NativeWindowV3'), 'Existing GUI instance; refusing fixture')

    def wait(predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value: return value
            time.sleep(.02)
        raise AssertionError('GUI state did not become ready')

    def geometry(hwnd, main=False):
        rows = []
        origin = POINT(); user.ClientToScreen(hwnd, c.byref(origin))
        @callback
        def each(child, _):
            text = c.create_unicode_buffer(128); user.GetWindowTextW(child,text,len(text))
            approved = main and (user.GetDlgCtrlID(child) in (104,112,113,114,115,116,132,303,308,309) or
                                 text.value == '连接' or user.IsChild(user.GetDlgItem(hwnd,112),child))
            if user.IsWindowVisible(child) and not approved:
                rect = RECT(); user.GetWindowRect(child, c.byref(rect))
                name = c.create_unicode_buffer(128); user.GetClassNameW(child, name, 128)
                rows.append([user.GetDlgCtrlID(child), name.value, rect.left-origin.x, rect.top-origin.y,
                             rect.right-origin.x, rect.bottom-origin.y, bool(user.IsWindowEnabled(child))])
            return True
        user.EnumChildWindows(hwnd, each, 0)
        return rows

    def remote_query_completed(dialog):
        status = c.create_unicode_buffer(2048)
        user.GetDlgItemTextW(dialog, 1025, status, len(status))
        # Owner-drawn CBS_DROPDOWNLIST has no edit text: WM_GETTEXT is empty.
        # The isolated instance has no serial devices, so compare its exact
        # completed service response, disabled Connect and selected result row.
        ports = user.GetDlgItem(dialog, 1002)
        return (status.value == '该实例没有已打开的本地串口' and
                not user.IsWindowEnabled(user.GetDlgItem(dialog, 1)) and
                user.SendMessageW(ports, 0x147, 0, 0) == 0)

    def extract(archive, destination):
        destination.mkdir()
        with zipfile.ZipFile(archive) as package:
            versions = dict(line.split('=', 1) for line in package.read('VERSION').decode('utf-8').splitlines() if line)
            version = versions.get('CPP_VERSION', '')
            require(re.fullmatch(r'V\d+\.\d+\.\d+', version) and versions.get('PROJECT_VERSION') == version,
                    'Archive version records disagree')
            for name in ('serialctl.exe', 'plink.exe', 'psftp.exe'):
                (destination / name).write_bytes(package.read(name))
        (destination / 'update.ini').write_text('[update]\r\nurl=\r\n', encoding='ascii')
        return version

    def version_footer(dialog, expected):
        matches = []
        @callback
        def each(child, _):
            name, text = c.create_unicode_buffer(128), c.create_unicode_buffer(128)
            user.GetClassNameW(child, name, len(name)); user.GetWindowTextW(child, text, len(text))
            if user.IsWindowVisible(child) and name.value == 'Static' and text.value.startswith('SerialCtl V'):
                matches.append((int(child), text.value))
            return True
        user.EnumChildWindows(dialog, each, 0)
        require(len(matches) == 1 and matches[0][1] == 'SerialCtl ' + expected,
                'Settings must display the version from its exact archive: ' + repr(matches))
        control, text = matches[0]
        bounds, origin = RECT(), POINT()
        user.GetWindowRect(control, c.byref(bounds)); user.ClientToScreen(dialog, c.byref(origin))
        return control, {'expected': expected, 'displayed': text,
                         'rect': [bounds.left-origin.x, bounds.top-origin.y, bounds.right-origin.x, bounds.bottom-origin.y]}

    baseline, candidate = {}, {}
    report = {'status': 'RUNNING', 'baselineSha256': hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
              'candidateSha256': hashlib.sha256(args.candidate.read_bytes()).hexdigest(), 'cases': [],
              'scope': 'same Windows desktop, actual measured DPI; blank main and SSH/serial/Telnet/remote-serial/Settings dialogs in both palettes',
              'unexecuted': ['other system DPIs', 'hardware terminal and SFTP content', 'all modal dialogs outside the enumerated cases']}
    process = None
    moved = False
    fixture_owned = False
    try:
        if config.exists(): os.replace(config, backup); moved = True
        for label, archive, captures in [('baseline', args.baseline, baseline), ('candidate', args.candidate, candidate)]:
            if config.exists():
                require(fixture_owned, 'Unknown configuration appeared during fixture')
                shutil.rmtree(config); fixture_owned = False
            config.mkdir()
            fixture_owned = True
            (config / 'commands.txt').write_text('#Fixture\necho FIXTURE\n', encoding='utf-8')
            (config / 'settings.ini').write_text('[Pet]\nEnabled=0\n[Layout]\nRightPanelWidth=320\n', encoding='ascii')
            app = args.output / (label + '-app'); version = extract(archive, app)
            process = subprocess.Popen([str(app / 'serialctl.exe')], cwd=app)
            main_window = wait(lambda: (windows(process.pid, 'SerialCtlWin7NativeWindowV3') or [None])[0])
            user.MoveWindow(main_window, 0, 0, min(1200, user.GetSystemMetrics(0)), min(850, user.GetSystemMetrics(1)-30), True)
            user.SetForegroundWindow(main_window)
            desktop = DesktopActions(SimpleNamespace(window=main_window, pid=process.pid, output=args.output,
                                                     prefix='freeze-'+label, mode='desktop'))
            report['dpi'] = desktop.dpi
            for theme in ['light', 'dark']:
                if theme == 'dark': user.PostMessageW(main_window, 0x111, 104, 0)
                for name, command in [('main', None), ('ssh', 100), ('serial', 101), ('telnet', 102), ('remote', 103), ('settings', 303)]:
                    target = main_window
                    if command:
                        user.PostMessageW(main_window, 0x111, command, 0)
                        target = wait(lambda: (windows(process.pid, '#32770') or [None])[0])
                        # Select Cancel through native focus navigation to remove
                        # any text caret from the deterministic pixel comparison.
                        user.PostMessageW(target, 0x28, user.GetDlgItem(target, 2), 1)
                        # Compare resting fields/content, not the deliberately
                        # revised keyboard focus cue. Native input tests cover
                        # focus, Space, Enter and retained focus independently.
                        user.SendMessageW(target, 0x127, 1 | (1 << 16), 0)
                    time.sleep(2 if name in ('serial', 'remote') else .4)
                    if name == 'remote':
                        # Compare the same completed asynchronous state, rather
                        # than racing a fixed two-second capture against discovery.
                        wait(lambda: remote_query_completed(target), seconds=20)
                        user.PostMessageW(target, 0x28, user.GetDlgItem(target, 2), 1)
                        time.sleep(.2)
                    user.SetCursorPos(user.GetSystemMetrics(0)-2, user.GetSystemMetrics(1)-2)
                    desktop.capture(theme+'-'+name)
                    frame, raw = desktop.pending_pixels[-1]
                    rect, point = RECT(), POINT()
                    user.GetClientRect(target, c.byref(rect)); user.ClientToScreen(target, c.byref(point))
                    width, height, stride = rect.right, rect.bottom, frame['width']*4
                    pixels = b''.join(raw[(point.y+y)*stride+point.x*4:(point.y+y)*stride+(point.x+width)*4] for y in range(height))
                    rgb = bytearray(pixels); rgb[3::4] = bytes(len(rgb)//4)
                    capture = {'size': [width, height], 'geometry': geometry(target, name=='main'),
                               'pixels': bytes(rgb), 'rawPixels': bytes(rgb), 'frame': frame['path']}
                    if name == 'main':
                        # Approved UI polish: compact detached utility controls,
                        # command notebook, connection heading and panel contours.
                        # All terminal pixels and remaining dialog geometry stay frozen.
                        def bounds(identifier):
                            box=RECT();user.GetWindowRect(user.GetDlgItem(target,identifier),c.byref(box))
                            return [box.left-point.x,box.top-point.y,box.right-point.x,box.bottom-point.y]
                        listing,tabs,toggle,settings,themebox = [bounds(i) for i in (112,118,125,303,104)]
                        scale=report['dpi']/96
                        capture['approvedRegions']=[
                            [listing[0],listing[1],width-round(12*scale),height-round(12*scale)],
                            [bounds(117)[0],tabs[1]-round(4*scale),toggle[0],toggle[3]+round(4*scale)],
                            [0,0,width,round(72*scale)],
                            [round(24*scale),round(80*scale),round(110*scale),round(120*scale)]]
                        pad=round(16*scale); band=round(12*scale)
                        terminal,leftlist = bounds(107),bounds(106)
                        frames=[
                            [leftlist[0]-pad,terminal[1]-pad,leftlist[2]+pad,height-round(12*scale)],
                            [terminal[0]-pad,terminal[1]-pad,terminal[2]+pad,terminal[3]+pad],
                            [listing[0]-pad,terminal[1]-pad,width-round(12*scale),height-round(12*scale)]]
                        for left,top,right,bottom in frames:
                            capture['approvedRegions'].extend([
                                [left,top,right,top+band], [left,bottom-band,right,bottom],
                                [left,top,left+band,bottom], [right-band,top,right,bottom]])
                    desktop.flush_frames()
                    if name == 'settings':

                        control, capture['version'] = version_footer(target, version)
                        # Keep the original real screenshot. For the second
                        # comparison frame normalize only the verified version
                        # text, so its font, colors, bounds and background are
                        # still compared pixel for pixel across releases.
                        require(user.SetWindowTextW(control, 'SerialCtl V0.0.0'), 'Cannot normalize fixture version text')
                        user.RedrawWindow(control, None, None, 0x105)
                        time.sleep(.1)
                        desktop.capture(theme+'-'+name+'-version-normalized')
                        normalized, raw = desktop.pending_pixels[-1]
                        stride = normalized['width']*4
                        pixels = b''.join(raw[(point.y+y)*stride+point.x*4:(point.y+y)*stride+(point.x+width)*4] for y in range(height))
                        rgb = bytearray(pixels); rgb[3::4] = bytes(len(rgb)//4)
                        capture['pixels'] = bytes(rgb)
                        capture['normalizedFrame'] = normalized['path']
                        desktop.flush_frames()
                    captures[(theme, name)] = capture
                    if command:
                        user.PostMessageW(target, 0x10, 0, 0)
                        wait(lambda: user.IsWindowEnabled(main_window))
            user.PostMessageW(main_window, 0x10, 0, 0)
            process.wait(timeout=15); process = None
        for key, before in baseline.items():
            after = candidate[key]
            require(before['size'] == after['size'], 'Client dimensions changed: '+str(key))
            approved = before.get('approvedRegions',[])+after.get('approvedRegions',[])
            def exempt(p):
                x,y=p%before['size'][0],p//before['size'][0]
                return any(left<=x<right and top<=y<bottom for left,top,right,bottom in approved)
            if approved:
                for capture in (before,after):
                    normalized=bytearray(capture['pixels'])
                    for p in range(len(normalized)//4):
                        if exempt(p):normalized[p*4:p*4+4]=b'\0'*4
                    capture['pixels']=bytes(normalized)
            changed = sum(before['pixels'][i:i+3] != after['pixels'][i:i+3] for i in range(0, len(before['pixels']), 4))
            raw_changes = [i//4 for i in range(0, len(before['rawPixels']), 4)
                           if before['rawPixels'][i:i+3] != after['rawPixels'][i:i+3]]
            unexempt = sum(not exempt(p) for p in raw_changes)
            if key[1] == 'settings' and before['version']['expected'] != after['version']['expected']:
                require(before['version']['rect'] == after['version']['rect'], 'Version footer bounds changed')
                left, top, right, bottom = before['version']['rect']
                unexempt = sum(not (left <= p % before['size'][0] < right and top <= p // before['size'][0] < bottom)
                               for p in raw_changes)
            report['cases'].append({'theme': key[0], 'view': key[1], 'changedPixels': changed,
                'rawChangedPixels': len(raw_changes), 'unexemptRawChangedPixels': unexempt,
                'approvedChangeRegions':approved,'geometryEqual': before['geometry'] == after['geometry'], 'baselineFrame': before['frame'], 'candidateFrame': after['frame'],
                'baselineVersion': before.get('version'), 'candidateVersion': after.get('version'),
                'baselineNormalizedFrame': before.get('normalizedFrame'), 'candidateNormalizedFrame': after.get('normalizedFrame')})
        require(all(case['changedPixels'] == 0 and case['unexemptRawChangedPixels'] == 0 and
                    case['geometryEqual'] for case in report['cases']), 'UI freeze pixel/geometry mismatch')
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'; report['error'] = str(error)
    finally:
        if process and process.poll() is None:
            process.terminate(); process.wait(timeout=10)
        if fixture_owned and config.exists(): shutil.rmtree(config)
        if moved:
            require(not config.exists(), 'Configuration destination occupied; backup retained')
            os.replace(backup, config)
        (args.output / 'ui-freeze-comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('::notice title=UI freeze comparison::'+json.dumps(report, separators=(',', ':')))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
