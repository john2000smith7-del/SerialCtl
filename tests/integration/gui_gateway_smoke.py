"""Against the packaged GUI's real persistent CMD, not an echo substitute."""
import base64
import ctypes
import pathlib
import sys
import time
sys.path.insert(0, sys.argv[1])
from serialctl_ws import Client, ProtocolError
window = int(sys.argv[2])
user32 = ctypes.windll.user32
user32.GetDlgItem.restype = ctypes.c_void_p
user32.GetDlgItem.argtypes = (ctypes.c_void_p, ctypes.c_int)
user32.SendMessageW.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)
with Client('127.0.0.1') as c:
    resources = c.resources()['resources']
    cmd = [r for r in resources if r['kind'] == 'cmd' and r['connected']][-1]['id']
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
    print('PASS: packaged GUI unified WebSocket scope, CMD stdout, GUI sequential line editing, persistent state; outputs visible/local logged')
