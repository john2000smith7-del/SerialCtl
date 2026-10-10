"""Against the packaged GUI's real persistent CMD, not an echo substitute."""
import base64
import ctypes
import pathlib
import json
import platform
import sys
import time
sys.path.insert(0, sys.argv[1])
from serialctl_ws import Client, ProtocolError
window = int(sys.argv[2])
user32 = ctypes.windll.user32
user32.GetDlgItem.restype = ctypes.c_void_p
user32.GetDlgItem.argtypes = (ctypes.c_void_p, ctypes.c_int)
user32.GetPropW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p)
user32.GetPropW.restype = ctypes.c_void_p
user32.SetForegroundWindow.argtypes = (ctypes.c_void_p,)
user32.SendMessageW.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)
with Client('127.0.0.1') as c:
    resources = c.resources()['resources']
    cmd = [r for r in resources if r['kind'] == 'cmd' and r['connected']][-1]['id']
    if '--start-stream' in sys.argv:
        c.input(cmd, b'powershell -NoProfile -Command "1..2000 | ForEach-Object {Write-Output (\'SERIALCTL_DYNAMIC_\'+$_); Start-Sleep -Milliseconds 10}"\r\n')
        print('Started sustained real CMD child stdout on ' + cmd)
        raise SystemExit(0)
    c.subscribe(cmd)
    power = c.request('power.get', 'power-1')
    assert not power['connected'] and power['selectedChannels'] == [1]
    for op, params in [('connect', {}), ('disconnect', {}), ('power.parameters', {}),
                       ('power.scpi', {}), ('power.task', {}), ('power.output', {'channels': [2], 'enabled': True})]:
        try:
            c.request(op, 'power-1', params)
        except ProtocolError:
            pass
        else:
            raise AssertionError('Unexpected API capability: ' + op)
    c.input(cmd, b'echo SERIALCTL_REMOTE_VISIBLE\r\n')
    output = bytearray()
    def until(marker, timeout=10):
        deadline = time.monotonic() + timeout
        while marker not in output and time.monotonic() < deadline:
            e = c.next_event(cmd, timeout=timeout)
            if e['kind'] == 'output':
                output.extend(base64.b64decode(e['data'], validate=True))
        assert marker in output, (marker, output)
    until(b'SERIALCTL_REMOTE_VISIBLE')
    terminal = user32.GetDlgItem(window, 107)
    assert terminal
    # Sequential native GUI key editing: Home, Right, Delete, insertion,
    # Backspace, End. No direct whole-line script substitutes for these keys.
    for character in 'echX SERIALCTL_GUI_EDITED':
        user32.SendMessageW(terminal, 0x102, ord(character), 0)
    for key in (0x24, 0x27, 0x27, 0x27, 0x2e):
        user32.SendMessageW(terminal, 0x100, key, 0)
    user32.SendMessageW(terminal, 0x102, ord('o'), 0)
    user32.SendMessageW(terminal, 0x100, 0x23, 0)
    user32.SendMessageW(terminal, 0x102, ord('X'), 0)
    user32.SendMessageW(terminal, 0x102, 8, 0)
    user32.SendMessageW(terminal, 0x102, 13, 0)
    until(b'SERIALCTL_GUI_EDITED')
    c.input(cmd, b'set SERIALCTL_GUI_STATE=retained\r\necho SERIALCTL_STATE-%SERIALCTL_GUI_STATE%\r\n')
    until(b'SERIALCTL_STATE-retained')
    c.input(cmd, b'chcp 65001\r\necho SERIALCTL_CODEPAGE_READY\r\n')
    until(b'SERIALCTL_CODEPAGE_READY')
    state=c.request('session.get',cmd)
    assert state['inputCodePage']==65001 and state['outputCodePage']==65001, state
    for character in 'echo SERIALCTL_GUI_中文':
        user32.SendMessageW(terminal,0x102,ord(character),0)
    user32.SendMessageW(terminal,0x102,13,0)
    until('SERIALCTL_GUI_中文'.encode('utf-8'))
    # Recall and resubmit the local Unicode history through native key messages.
    user32.SendMessageW(terminal,0x100,0x26,0)
    user32.SendMessageW(terminal,0x102,13,0)
    c.input(cmd,b'echo SERIALCTL_HISTORY_DONE\r\n')
    until(b'SERIALCTL_HISTORY_DONE')
    user32.SetForegroundWindow(window)
    delays=[]
    for sample in range(100):
        previous=user32.GetPropW(terminal,'SerialCtl.PaintSample')
        marker=('SERIALCTL_PAINT_%03d'%sample).encode('ascii')
        c.input(cmd,b'echo '+marker+b'\r\n')
        until(marker)
        deadline=time.monotonic()+5
        while user32.GetPropW(terminal,'SerialCtl.PaintSample')==previous and time.monotonic()<deadline:
            time.sleep(.001)
        assert user32.GetPropW(terminal,'SerialCtl.PaintSample')!=previous, 'No real WM_PAINT observation'
        delays.append(user32.GetPropW(terminal,'SerialCtl.PaintUsec')/1000)
    ordered=sorted(delays)
    report={'cmd':cmd,'environment':platform.platform(),'network':'loopback','physical_serial_rate':'not applicable: real CMD pipes',
            'clients':1,'samples':delays,'P50_ms':ordered[50],'P95_ms':ordered[95],'P99_ms':ordered[99],
            'measurement':'PostData raw output callback QPC -> actual packaged terminal WM_PAINT BitBlt completion; excludes physical monitor presentation'}
    if len(sys.argv)>3:
        pathlib.Path(sys.argv[3]).write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('CMD display P50=%.3f ms P95=%.3f ms P99=%.3f ms; 100 samples'%(ordered[50],ordered[95],ordered[99]))
    print('PASS: packaged GUI unified WebSocket scope, CMD stdout, GUI sequential line editing, persistent state; outputs visible/local logged')
