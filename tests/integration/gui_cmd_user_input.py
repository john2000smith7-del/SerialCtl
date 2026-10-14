"""Exercise the packaged GUI's CMD through desktop SendInput only.

Run on a dedicated Windows desktop with the main HWND and an idle active CMD.
WebSocket operations only inspect resources/state and subscribe to raw output.
"""
import argparse
import base64
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
import pathlib
import platform
import queue
import re
import subprocess
import sys
import time
import traceback
import uuid


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-dir', type=pathlib.Path, required=True)
    parser.add_argument('--hwnd', type=lambda value: int(value, 0), required=True,
                        help='Main application HWND; terminal child 107 is resolved by the test')
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output-json', type=pathlib.Path, required=True)
    return parser.parse_args()


def run(args, report):
    if os.name != 'nt':
        raise RuntimeError('A real Windows desktop is required; Linux cannot execute this test')
    native = args.native_dir.resolve()
    if not (native / 'serialctl_ws.py').is_file():
        raise RuntimeError('Packaged serialctl_ws.py is missing from --native-dir')
    sys.path.insert(0, str(native))
    from serialctl_ws import Client
    u = ctypes.WinDLL('user32', use_last_error=True)
    u.SetProcessDPIAware()
    u.GetDlgItem.argtypes = (w.HWND, ctypes.c_int)
    u.GetDlgItem.restype = w.HWND
    u.GetDlgCtrlID.argtypes = (w.HWND,)
    u.GetWindowThreadProcessId.argtypes = (w.HWND, ctypes.POINTER(w.DWORD))
    u.GetWindowThreadProcessId.restype = w.DWORD
    u.GetWindowRect.argtypes = (w.HWND, ctypes.POINTER(w.RECT))
    u.IsWindow.argtypes = u.IsWindowVisible.argtypes = u.IsWindowEnabled.argtypes = (w.HWND,)
    u.SetForegroundWindow.argtypes = (w.HWND,)
    u.GetForegroundWindow.restype = w.HWND
    u.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
    u.WindowFromPoint.argtypes = (w.POINT,)
    u.WindowFromPoint.restype = w.HWND

    class MouseInput(ctypes.Structure):
        _fields_ = [('dx', w.LONG), ('dy', w.LONG), ('mouseData', w.DWORD),
                    ('flags', w.DWORD), ('time', w.DWORD), ('extra', ctypes.c_size_t)]

    class KeyboardInput(ctypes.Structure):
        _fields_ = [('vk', w.WORD), ('scan', w.WORD), ('flags', w.DWORD),
                    ('time', w.DWORD), ('extra', ctypes.c_size_t)]

    class HardwareInput(ctypes.Structure):
        _fields_ = [('message', w.DWORD), ('paramLow', w.WORD), ('paramHigh', w.WORD)]

    class InputData(ctypes.Union):
        _fields_ = [('mouse', MouseInput), ('key', KeyboardInput), ('hardware', HardwareInput)]

    class Input(ctypes.Structure):
        _fields_ = [('type', w.DWORD), ('data', InputData)]

    class GuiThreadInfo(ctypes.Structure):
        _fields_ = [('size', w.DWORD), ('flags', w.DWORD), ('active', w.HWND),
                    ('focus', w.HWND), ('capture', w.HWND), ('menuOwner', w.HWND),
                    ('moveSize', w.HWND), ('caret', w.HWND), ('caretRect', w.RECT)]

    u.SendInput.argtypes = (w.UINT, ctypes.POINTER(Input), ctypes.c_int)
    u.SendInput.restype = w.UINT
    u.GetGUIThreadInfo.argtypes = (w.DWORD, ctypes.POINTER(GuiThreadInfo))
    terminal = u.GetDlgItem(args.hwnd, 107)
    assert terminal and u.GetDlgCtrlID(terminal) == 107, 'Terminal child 107 is missing'
    owner = w.DWORD()
    thread = u.GetWindowThreadProcessId(terminal, ctypes.byref(owner))
    assert thread and owner.value == args.pid, 'Terminal PID does not match the requested process'
    u.GetWindowThreadProcessId(args.hwnd, ctypes.byref(owner))
    assert owner.value == args.pid, 'Main HWND PID does not match'
    assert u.IsWindow(terminal) and u.IsWindowVisible(terminal) and u.IsWindowEnabled(terminal)
    report['terminal_hwnd'] = int(terminal)
    report['input_structure_bytes'] = ctypes.sizeof(Input)
    assert ctypes.sizeof(Input) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)

    def wait(predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError('Timed out waiting for real desktop/CMD state')
            time.sleep(.01)

    def focus():
        info = GuiThreadInfo()
        info.size = ctypes.sizeof(info)
        assert u.GetGUIThreadInfo(thread, ctypes.byref(info)), 'GetGUIThreadInfo failed'
        return {'focus': int(info.focus or 0), 'foreground': int(u.GetForegroundWindow() or 0)}

    def require_focus():
        state = focus()
        assert state == {'focus': int(terminal), 'foreground': args.hwnd}, state
        return state

    def inject(down, up):
        events = (Input * 2)(down, up)
        sent = u.SendInput(2, events, ctypes.sizeof(Input))
        assert sent == 2, ('SendInput failed', sent, ctypes.get_last_error())

    def key(vk, name):
        state = require_focus()
        extended = 1 if vk in (0x23, 0x24, 0x25, 0x27, 0x2e) else 0
        down, up = Input(), Input()
        down.type = up.type = 1
        down.data.key.vk = up.data.key.vk = vk
        down.data.key.flags, up.data.key.flags = extended, extended | 2
        inject(down, up)
        report['input_events'].append({'action': 'key', 'key': name, 'vk': vk,
                                       'focus_before': state, 'monotonic': time.monotonic()})
        time.sleep(.025)

    def type_ascii(value):
        assert value.isascii(), 'This test intentionally covers ASCII typing only'
        for character in value:
            state = require_focus()
            down, up = Input(), Input()
            down.type = up.type = 1
            down.data.key.scan = up.data.key.scan = ord(character)
            down.data.key.flags, up.data.key.flags = 4, 6  # KEYEVENTF_UNICODE + KEYUP.
            inject(down, up)
            report['input_events'].append({'action': 'character', 'character': character,
                                           'focus_before': state, 'monotonic': time.monotonic()})
            time.sleep(.004)

    def command(value):
        type_ascii(value)
        key(0x0d, 'Enter')

    def capture(label):
        destination = args.output_json.resolve().with_name(args.output_json.stem + '-' + label + '.png')
        literal = str(destination).replace("'", "''")
        script = ("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
                  "$s=[Windows.Forms.SystemInformation]::VirtualScreen;"
                  "$b=New-Object Drawing.Bitmap($s.Width,$s.Height);$g=[Drawing.Graphics]::FromImage($b);"
                  "$g.CopyFromScreen($s.Left,$s.Top,0,0,$s.Size);$b.Save('" + literal +
                  "',[Drawing.Imaging.ImageFormat]::Png);$g.Dispose();$b.Dispose()")
        subprocess.run(['powershell.exe', '-NoProfile', '-Command', script], check=True, timeout=15)
        report['screenshots'].append({'path': str(destination), 'method': 'desktop CopyFromScreen',
                                      'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()})

    if u.GetForegroundWindow() != args.hwnd:
        u.SetForegroundWindow(args.hwnd)
        wait(lambda: u.GetForegroundWindow() == args.hwnd, 2)
    rect = w.RECT()
    assert u.GetWindowRect(terminal, ctypes.byref(rect))
    assert rect.right > rect.left and rect.bottom > rect.top
    x, y = (rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2
    assert u.SetCursorPos(x, y)
    assert u.WindowFromPoint(w.POINT(x, y)) == terminal, 'Terminal is obscured'
    down, up = Input(), Input()
    down.data.mouse.flags, up.data.mouse.flags = 2, 4
    inject(down, up)
    report['input_events'].append({'action': 'mouse-click', 'point': [x, y],
                                   'hwnd': int(terminal), 'monotonic': time.monotonic()})
    wait(lambda: focus() == {'focus': int(terminal), 'foreground': args.hwnd}, 2)
    report['focus_after_click'] = require_focus()
    capture('before')
    token = uuid.uuid4().hex[:12]
    basename = 'SerialCtl-keys-' + token
    fixture = pathlib.Path(os.environ['TEMP']) / basename
    fixture.mkdir()
    report['fixture_directory'] = str(fixture)
    variable = 'SERIALCTL_KEYS_' + token.upper()
    marker = 'KEY_EXEC_' + token.upper() + '_OK'
    state_marker = 'STATE_EXEC_' + token.upper() + '_OK'
    cd_marker = 'CD_EXEC_' + token.upper() + '_OK'
    report['fixtures'] = ['An empty unique TEMP directory is created by Python; all command/file content is produced by real GUI keyboard input']
    raw = bytearray()

    with Client('127.0.0.1') as client:
        resources = client.resources()['resources']
        candidates = [r for r in resources if r['kind'] == 'cmd' and r['connected']]
        assert candidates, 'No connected idle CMD session is available'
        resource = candidates[-1]['id']
        report['resource'] = resource
        report['state_before'] = client.request('session.get', resource)
        cursor = client.events_since(resource, 0)['cursor']
        client.subscribe(resource, cursor)

        def output_line(value, timeout=10):
            pattern = re.compile(rb'(?:^|[\r\n])' + re.escape(value.encode('ascii')) + rb'\r?\n')
            deadline = time.monotonic() + timeout
            while not pattern.search(raw):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError('Missing standalone executed output line: ' + value)
                try:
                    event = client.next_event(resource, timeout=remaining)
                except queue.Empty as error:
                    raise AssertionError('Missing standalone executed output line: ' + value) from error
                report['websocket_events'].append(event)
                if event.get('kind') == 'output':
                    raw.extend(base64.b64decode(event['data'], validate=True))
            report['checks'].append({'check': 'raw executed output line', 'expected': value, 'passed': True})

        def file_matches(name, expected):
            path = fixture / name
            expected_bytes = expected.encode('ascii') + b'\r\n'
            wait(lambda: path.is_file() and path.read_bytes() == expected_bytes)
            actual = path.read_bytes()
            report['files'].append({'path': str(path), 'base64': base64.b64encode(actual).decode('ascii'),
                                    'sha256': hashlib.sha256(actual).hexdigest(), 'passed': True})

        try:
            # Correct an invalid executable using real editing keys. Left/Right are
            # interposed; Delete and Backspace must remove distinct deliberate errors.
            type_ascii('echX ' + marker + '>"%TEMP%\\' + basename + '\\edited.txt"')
            key(0x24, 'Home')
            for _ in range(3):
                key(0x27, 'Right')
            key(0x25, 'Left')
            key(0x27, 'Right')
            key(0x2e, 'Delete')
            type_ascii('o')
            key(0x23, 'End')
            type_ascii('X')
            key(0x08, 'Backspace')
            key(0x0d, 'Enter')
            file_matches('edited.txt', marker)
            command('type "%TEMP%\\' + basename + '\\edited.txt"')
            output_line(marker)
            report['checks'].append({'check': 'Home/Left/Right/Delete/End/Backspace editing executes corrected command', 'passed': True})

            command('set "' + variable + '=' + state_marker + '"')
            command('pushd "%TEMP%\\' + basename + '"')
            command('echo ' + cd_marker + '>relative.txt')
            file_matches('relative.txt', cd_marker)
            command('echo %' + variable + '%>state.txt')
            file_matches('state.txt', state_marker)
            command('type relative.txt state.txt')
            output_line(cd_marker)
            output_line(state_marker)
            report['checks'].append({'check': 'set value and changed working directory persist across keyboard-submitted commands', 'passed': True})
            command('popd')
            command('set "' + variable + '="')
            command('type "%TEMP%\\' + basename + '\\edited.txt"')
            # Force a fresh observation for the final post-cleanup command.
            raw.clear()
            output_line(marker)
            report['state_after'] = client.request('session.get', resource)
            assert report['state_after']['connected'] is True
            report['focus_after_commands'] = require_focus()
            capture('after')
        finally:
            report['observed_output_base64'] = base64.b64encode(raw).decode('ascii')
            report['fixture_cleanup'] = 'Successful run restores directory with popd and clears its unique environment variable; evidence files retained in the unique TEMP directory'


def main():
    args = arguments()
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    report = {'status': 'FAIL', 'main_hwnd': args.hwnd, 'pid': args.pid,
              'environment': platform.platform(), 'input_events': [], 'websocket_events': [],
              'screenshots': [], 'checks': [], 'files': [],
              'input_method': 'Actual terminal mouse click and SendInput keyboard down/up; characters use KEYEVENTF_UNICODE one at a time; no WM_CHAR/WM_KEYDOWN messages or WebSocket command input',
              'limitations': ['ASCII keyboard characters only; Chinese IME is not covered',
                              'Requires an existing active idle CMD with an empty local draft; latest connected CMD resource is observed',
                              'Screenshots are finite desktop samples, not a video or physical monitor latency measurement']}
    code = 1
    try:
        run(args, report)
        report['status'] = 'PASS'
        code = 0
    except Exception as error:
        report.update({'exception': repr(error), 'traceback': traceback.format_exc()})
        print(report['traceback'], file=sys.stderr)
    finally:
        report['exit_code'] = code
        args.output_json.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(report['status'] + ': real CMD SendInput execution; report ' + str(args.output_json))
    return code


if __name__ == '__main__':
    sys.exit(main())
