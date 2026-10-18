"""Real desktop regression for shared rounded controls and terminal mouse input.

Only read-only Win32 queries inspect state. User actions use system mouse events
and SendInput; screen evidence uses DesktopActions' desktop BitBlt, never
PrintWindow. Run in the dedicated account used by scripts/smoke-ui.ps1.
"""
import argparse
import base64
import contextlib
import ctypes as c
from ctypes import wintypes as w
import datetime
import hashlib
import json
import os
from pathlib import Path
import queue
import sys
import time
import traceback
import uuid

from desktop_user_actions import DesktopActions, POINT, RECT, require


class ComboInfo(c.Structure):
    _fields_ = [('size', w.DWORD), ('item', RECT), ('button', RECT),
                ('button_state', w.DWORD), ('combo', w.HWND),
                ('edit', w.HWND), ('list', w.HWND)]


class GuiThreadInfo(c.Structure):
    _fields_ = [('size', w.DWORD), ('flags', w.DWORD), ('active', w.HWND),
                ('focus', w.HWND), ('capture', w.HWND), ('menu_owner', w.HWND),
                ('move_size', w.HWND), ('caret', w.HWND), ('caret_rect', RECT)]


class MouseInput(c.Structure):
    _fields_ = [('dx', w.LONG), ('dy', w.LONG), ('data', w.DWORD),
                ('flags', w.DWORD), ('time', w.DWORD), ('extra', c.c_size_t)]


class KeyboardInput(c.Structure):
    _fields_ = [('vk', w.WORD), ('scan', w.WORD), ('flags', w.DWORD),
                ('time', w.DWORD), ('extra', c.c_size_t)]


class InputData(c.Union):
    _fields_ = [('mouse', MouseInput), ('key', KeyboardInput)]


class Input(c.Structure):
    _fields_ = [('type', w.DWORD), ('data', InputData)]


class GlobalActions(DesktopActions):
    def __init__(self, args):
        super().__init__(args)
        self.kernel = c.WinDLL('kernel32', use_last_error=True)
        self.report['scope'] = ('Actual production controls/terminal; no physical serial, power, '
                                'SSH server, Chinese IME or unmeasured DPI assertion')
        self.report['input'] = 'SetCursorPos + mouse_event and SendInput keyboard; read-only native state queries'
        self.report['checks'] = []
        self.report['clipboard_observations'] = []
        self.report['websocket_events'] = []
        self.report['files'] = []
        self.report['skipped_controls'] = []
        self.raw = bytearray()
        self._declare_extra()
        require(c.sizeof(Input) == (40 if c.sizeof(c.c_void_p) == 8 else 28),
                'SendInput ABI layout does not match Windows pointer width')
        self.report['input_structure_bytes'] = c.sizeof(Input)
        pid = w.DWORD()
        self.thread = self.user.GetWindowThreadProcessId(self.window, c.byref(pid))
        require(pid.value == args.pid, 'Main HWND does not belong to requested production PID')
        dc = self.user.GetDC(self.window)
        try:
            self.report['device_caps_dpi'] = self.gdi.GetDeviceCaps(dc, 88)
        finally:
            self.user.ReleaseDC(self.window, dc)
        self.report['unverified_dpi_percent'] = [n for n in (100, 125, 150, 200)
                                                if self.dpi != round(n * 96 / 100)]

    def _declare_extra(self):
        declarations = {
            'GetComboBoxInfo': ((w.HWND, c.POINTER(ComboInfo)), w.BOOL),
            'GetClassNameW': ((w.HWND, w.LPWSTR, c.c_int), c.c_int),
            'GetWindowRgn': ((w.HWND, w.HRGN), c.c_int),
            'GetGUIThreadInfo': ((w.DWORD, c.POINTER(GuiThreadInfo)), w.BOOL),
            'GetWindow': ((w.HWND, w.UINT), w.HWND),
            'GetParent': ((w.HWND,), w.HWND),
            'IsChild': ((w.HWND, w.HWND), w.BOOL),
            'GetWindowLongPtrW': ((w.HWND, c.c_int), c.c_ssize_t),
            'FindWindowW': ((w.LPCWSTR, w.LPCWSTR), w.HWND),
            'SendInput': ((w.UINT, c.POINTER(Input), c.c_int), w.UINT),
            'OpenClipboard': ((w.HWND,), w.BOOL),
            'CloseClipboard': ((), w.BOOL),
            'EmptyClipboard': ((), w.BOOL),
            'GetClipboardData': ((w.UINT,), w.HANDLE),
            'SetClipboardData': ((w.UINT, w.HANDLE), w.HANDLE),
            'GetClipboardSequenceNumber': ((), w.DWORD),
            'IsClipboardFormatAvailable': ((w.UINT,), w.BOOL),
        }
        for name, (arguments, result) in declarations.items():
            function = getattr(self.user, name)
            function.argtypes, function.restype = arguments, result
        for name, arguments, result in (
                ('GlobalAlloc', (w.UINT, c.c_size_t), w.HGLOBAL),
                ('GlobalLock', (w.HGLOBAL,), c.c_void_p),
                ('GlobalUnlock', (w.HGLOBAL,), w.BOOL),
                ('GlobalFree', (w.HGLOBAL,), w.HGLOBAL)):
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result
        self.user.EnumWindows.argtypes = (self.enum_callback, w.LPARAM)
        self.user.EnumWindows.restype = w.BOOL
        self.gdi.CreateRectRgn.argtypes = (c.c_int, c.c_int, c.c_int, c.c_int)
        self.gdi.CreateRectRgn.restype = w.HRGN
        self.gdi.PtInRegion.argtypes = (w.HRGN, c.c_int, c.c_int)
        self.gdi.PtInRegion.restype = w.BOOL

    def query(self, window, message, wp=0, lp=0):
        result = c.c_size_t()
        require(self.user.SendMessageTimeoutW(window, message, wp, lp, 2, 2000, c.byref(result)),
                'Read-only state query timed out: ' + hex(message))
        return c.c_ssize_t(result.value).value

    @staticmethod
    def wait(predicate, description, timeout=5):
        deadline = time.monotonic() + timeout
        while not predicate():
            require(time.monotonic() < deadline, 'Timed out: ' + description)
            time.sleep(.01)

    def check(self, name, **observations):
        self.report['checks'].append({'name': name, 'status': 'PASS', **observations})

    def key(self, vk, unicode_character=None):
        require(self.user.GetForegroundWindow() in (self.window, self.args.root),
                'Keyboard input target is no longer foreground')
        events = (Input * 2)()
        events[0].type = events[1].type = 1
        if unicode_character is not None:
            events[0].data.key.scan = events[1].data.key.scan = ord(unicode_character)
            events[0].data.key.flags, events[1].data.key.flags = 4, 6
        else:
            events[0].data.key.vk = events[1].data.key.vk = vk
            flag = 1 if vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2d, 0x2e, 0x5d) else 0
            events[0].data.key.flags, events[1].data.key.flags = flag, flag | 2
        require(self.user.SendInput(2, events, c.sizeof(Input)) == 2, 'SendInput rejected keyboard input')
        self.trace.append({**self.timestamp(), 'event': 'key', 'vk': vk, 'character': unicode_character})
        time.sleep(.015)

    def chord(self, modifier, vk):
        require(self.user.GetForegroundWindow() in (self.window, self.args.root),
                'Keyboard chord target is no longer foreground')
        events = (Input * 4)()
        for event, code, flags in zip(events, (modifier, vk, vk, modifier), (0, 0, 2, 2)):
            event.type = 1
            event.data.key.vk, event.data.key.flags = code, flags
        require(self.user.SendInput(4, events, c.sizeof(Input)) == 4, 'SendInput rejected keyboard chord')
        self.trace.append({**self.timestamp(), 'event': 'chord', 'modifier': modifier, 'vk': vk})
        time.sleep(.03)

    def text(self, value):
        for character in value:
            require(ord(character) <= 0xffff, 'Fixture currently uses BMP Unicode only')
            self.key(0, character)

    def command(self, value):
        self.text(value)
        self.key(0x0d)

    def focus_state(self):
        info = GuiThreadInfo()
        info.size = c.sizeof(info)
        require(self.user.GetGUIThreadInfo(self.thread, c.byref(info)), 'Cannot read actual GUI focus')
        return {'focus': int(info.focus or 0), 'foreground': int(self.user.GetForegroundWindow() or 0)}

    def click_window(self, window, point=None):
        require(self.user.IsWindowVisible(window) and self.user.IsWindowEnabled(window),
                'Actual mouse target is hidden or disabled')
        bounds = self.rect(window)
        point = point or ((bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2)
        require(self.user.WindowFromPoint(POINT(*point)) == window,
                'Actual mouse target is covered at ' + repr(point))
        self.click(point)
        time.sleep(.08)

    def screenshot(self, label):
        self.capture(label)
        self.flush_frames()

    def clipboard(self):
        self.wait(lambda: self.user.OpenClipboard(self.window), 'read clipboard')
        try:
            memory = self.user.GetClipboardData(13)
            if not memory:
                return None
            pointer = self.kernel.GlobalLock(memory)
            require(pointer, 'Cannot lock clipboard data')
            try:
                return c.wstring_at(pointer)
            finally:
                self.kernel.GlobalUnlock(memory)
        finally:
            self.user.CloseClipboard()

    def set_clipboard(self, value):
        raw = (value + '\0').encode('utf-16-le')
        memory = self.kernel.GlobalAlloc(2, len(raw))
        require(memory, 'GlobalAlloc clipboard failed')
        transferred = False
        try:
            pointer = self.kernel.GlobalLock(memory)
            require(pointer, 'Cannot lock clipboard allocation')
            try:
                c.memmove(pointer, raw, len(raw))
            finally:
                self.kernel.GlobalUnlock(memory)
            self.wait(lambda: self.user.OpenClipboard(self.window), 'write fixture clipboard')
            try:
                require(self.user.EmptyClipboard(), 'Cannot empty fixture clipboard')
                require(self.user.SetClipboardData(13, memory), 'Cannot transfer clipboard allocation')
                transferred = True
            finally:
                self.user.CloseClipboard()
        finally:
            if not transferred:
                self.kernel.GlobalFree(memory)

    def assert_clipboard(self, expected, label):
        self.wait(lambda: self.clipboard() == expected, label)
        actual = self.clipboard()
        self.report['clipboard_observations'].append({**self.timestamp(), 'label': label,
            'expected': expected, 'actual': actual, 'sequence': self.user.GetClipboardSequenceNumber()})
        self.check(label, expected=expected, actual=actual)

    def combo(self, window):
        before = self.query(window, 0x147)  # CB_GETCURSEL
        count = self.query(window, 0x146)  # CB_GETCOUNT
        identifier = self.user.GetDlgCtrlID(window)
        if count <= 0:
            self.report['skipped_controls'].append({'id': identifier, 'reason': 'no available options; no hardware/resource is fabricated'})
            return
        bounds = self.rect(window)
        point = (bounds[2] - max(8, round(16 * self.scale)), (bounds[1] + bounds[3]) // 2)
        self.click_window(window, point)
        self.wait(lambda: self.query(window, 0x157) != 0, 'actual dropdown opens')
        info = ComboInfo()
        info.size = c.sizeof(info)
        require(self.user.GetComboBoxInfo(window, c.byref(info)), 'GetComboBoxInfo failed')
        require(self.user.IsWindowVisible(info.list), 'Native combo popup is not visible')
        popup_bounds = self.rect(info.list)
        region = self.gdi.CreateRectRgn(0, 0, 0, 0)
        require(region, 'Cannot allocate region for read-only popup inspection')
        try:
            kind = self.user.GetWindowRgn(info.list, region)
            center_inside = bool(self.gdi.PtInRegion(region, (popup_bounds[2] - popup_bounds[0]) // 2,
                                                     (popup_bounds[3] - popup_bounds[1]) // 2))
            corner_inside = bool(self.gdi.PtInRegion(region, 0, 0))
        finally:
            self.gdi.DeleteObject(region)
        require(kind != 0 and center_inside and not corner_inside,
                'Expanded dropdown does not have an actual rounded popup region')
        self.screenshot('combo-' + str(identifier) + '-opened')
        # Keyboard selects a genuinely different item where possible; native
        # selection, not merely a delivered key or visible popup, is asserted.
        target = 0 if before != 0 else min(1, count - 1)
        self.key(0x24)
        for _ in range(target):
            self.key(0x28)
        self.key(0x0d)
        self.wait(lambda: self.query(window, 0x157) == 0, 'Enter closes dropdown')
        self.wait(lambda: self.query(window, 0x147) == target, 'keyboard changes actual combo selection')
        self.check('rounded combo and actual keyboard selection', id=identifier, count=count,
                   before=before, after=target, popup_bounds=popup_bounds, rounded_region_type=kind)
        # Restore the user's original form state through actual keyboard input.
        if before >= 0 and before != target:
            time.sleep(self.user.GetDoubleClickTime() / 1000 + .02)
            self.click_window(window, point)
            self.wait(lambda: self.query(window, 0x157) != 0, 'reopen dropdown')
            self.key(0x24)
            for _ in range(before):
                self.key(0x28)
            self.key(0x0d)
            self.wait(lambda: self.query(window, 0x147) == before, 'restore original selection')
        self.screenshot('combo-' + str(identifier) + '-closed')

    def select_control_option(self):
        root = self.args.root or self.window
        require(self.args.control_id is not None and self.args.selection is not None,
                '--control-id and --selection are required for select-combo')
        window = self.user.GetDlgItem(root, self.args.control_id)
        require(window, 'Requested native dropdown does not exist')
        before = self.query(window, 0x147)
        self.select_combo(window, self.args.selection)
        self.check('actual mouse and keyboard choose dropdown option',
                   id=self.args.control_id, before=before, after=self.args.selection)
        self.screenshot('selected-combo-' + str(self.args.control_id))

    def controls(self):
        root = self.args.root or self.window
        require(self.user.IsWindowVisible(root), 'Requested dialog/pane is not visible')
        children = []
        @self.enum_callback
        def inspect(window, _):
            name = c.create_unicode_buffer(64)
            self.user.GetClassNameW(window, name, len(name))
            if name.value.lower() == 'combobox':
                children.append(window)
            return True
        self.user.EnumChildWindows(root, inspect, 0)
        for window in children:
            if not self.user.IsWindowVisible(window) or not self.user.IsWindowEnabled(window):
                self.report['skipped_controls'].append({'id': self.user.GetDlgCtrlID(window),
                    'reason': 'hidden/disabled; no device state is fabricated to enable this field'})
                continue
            self.combo(window)
        self.screenshot('controls-final')
        self.report['combo_count_examined'] = len(children)


    def settings_dialog(self):
        found = []
        @self.enum_callback
        def inspect(window, _):
            pid = w.DWORD()
            self.user.GetWindowThreadProcessId(window, c.byref(pid))
            name = c.create_unicode_buffer(64)
            self.user.GetClassNameW(window, name, len(name))
            if (pid.value == self.args.pid and self.user.IsWindowVisible(window)
                    and name.value == '#32770' and '设置' in self.caption(window)):
                found.append(window)
            return True
        self.user.EnumWindows(inspect, 0)
        require(len(found) <= 1, 'Multiple visible Settings dialogs for one application')
        return found[0] if found else 0

    def open_settings(self, keyboard=False):
        if keyboard:
            self.click_window(self.control(107))
            self.key(0x5d)  # VK_APPS must open Settings, never paste.
        else:
            self.click_window(self.control(303))
        self.wait(lambda: self.settings_dialog(), 'top Settings opens the actual native dialog')
        dialog = self.settings_dialog()
        self.args.root = dialog
        require(self.user.GetWindow(dialog, 4) == self.window, 'Settings owner must be the main window')
        require(not self.user.IsWindowEnabled(self.window), 'Modal Settings did not disable its owner')
        require(not (self.user.GetWindowLongPtrW(dialog, -20) & 8), 'Settings must not become globally topmost')
        self.wait(lambda: self.user.GetForegroundWindow() == dialog, 'Settings obtains initial focus')
        self.screenshot('settings-opened')
        return dialog

    def close_settings(self, dialog, accept=False):
        button = self.user.GetDlgItem(dialog, 1 if accept else 2)
        require(button, 'Missing Settings save/cancel footer action')
        self.click_window(button)
        self.wait(lambda: not self.user.IsWindow(dialog), 'Settings closes after footer action')
        self.wait(lambda: self.user.IsWindowEnabled(self.window), 'Settings owner is enabled again')
        self.wait(lambda: self.user.GetForegroundWindow() == self.window, 'owner regains foreground')
        require(not (self.user.GetWindowLongPtrW(self.window, -20) & 8), 'Owner became globally topmost')
        self.args.root = 0
        focus = self.focus_state()
        require(focus['focus'] == self.window or self.user.IsChild(self.window, focus['focus']),
                'Settings left focus on a destroyed window or outside its owner')
        self.check('Settings owner/foreground restored after ' + ('save' if accept else 'cancel'),
                   focus=focus)

    def settings_state(self, dialog):
        result = {}
        for identifier in (2501, 2502, 2503, 2504, 2505, 2003, 2004, 2005, 2006, 2007, 2206):
            control = self.user.GetDlgItem(dialog, identifier)
            require(control and self.user.IsWindowVisible(control),
                    'Settings lost an existing terminal capability: ' + str(identifier))
            result[str(identifier)] = {'enabled': bool(self.user.IsWindowEnabled(control)),
                                      'caption': self.caption(control)}
            if identifier <= 2503 and identifier >= 2501:
                result[str(identifier)]['value'] = self.query(control, 0xf0)  # BM_GETCHECK
            elif identifier in (2504, 2505):
                result[str(identifier)]['value'] = self.query(control, 0x147)
                result[str(identifier)]['count'] = self.query(control, 0x146)
        return result

    def select_combo(self, window, target):
        require(0 <= target < self.query(window, 0x146), 'Requested real combo option is absent')
        bounds = self.rect(window)
        self.click_window(window, (bounds[2] - max(8, round(16 * self.scale)),
                                   (bounds[1] + bounds[3]) // 2))
        self.wait(lambda: self.query(window, 0x157) != 0, 'Settings dropdown opens')
        self.key(0x24)
        for _ in range(target):
            self.key(0x28)
        self.key(0x0d)
        self.wait(lambda: self.query(window, 0x157) == 0 and self.query(window, 0x147) == target,
                  'Settings keyboard selection is committed to its draft')

    def set_settings(self, dialog, values):
        for identifier, value in values.items():
            control = self.user.GetDlgItem(dialog, identifier)
            current = self.query(control, 0xf0 if identifier <= 2503 else 0x147)
            if current != value:
                if identifier <= 2503:
                    self.click_window(control)
                    self.wait(lambda: self.query(control, 0xf0) == value, 'Settings checkbox changed')
                else:
                    self.select_combo(control, value)

    @staticmethod
    def setting_values(state, identifiers):
        return {identifier: state[str(identifier)]['value'] for identifier in identifiers}

    def settings_drafts(self, session):
        dialog = self.open_settings()
        before = self.settings_state(dialog)
        self.report['settings_initial_state'] = before
        # CMD submits complete lines with CRLF; its Enter control is deliberately
        # disabled. Do not turn a fixed protocol rule into a fake configurable test.
        identifiers = (2501, 2502, 2503, 2504) if session else (2501, 2502)
        original = self.setting_values(before, identifiers)
        changed = {identifier: (1 - value if identifier <= 2503 else
                               (value + 1) % before[str(identifier)]['count'])
                   for identifier, value in original.items()}
        self.set_settings(dialog, changed)
        self.close_settings(dialog)
        dialog = self.open_settings()
        require(self.setting_values(self.settings_state(dialog), identifiers) == original,
                'Cancel committed Settings draft changes')
        self.check('Settings Cancel discards draft changes', values=original)
        self.set_settings(dialog, changed)
        self.close_settings(dialog, accept=True)
        dialog = self.open_settings()
        require(self.setting_values(self.settings_state(dialog), identifiers) == changed,
                'Save did not commit Settings draft changes')
        self.check('Settings Save commits draft changes', values=changed)
        self.set_settings(dialog, original)
        self.close_settings(dialog, accept=True)
        return original

    def no_session(self):
        dialog = self.open_settings()
        state = self.settings_state(dialog)
        for identifier in (2503, 2504, 2505, 2003, 2004, 2005, 2006, 2007):
            require(not state[str(identifier)]['enabled'],
                    'Settings exposes a session action without a session: ' + str(identifier))
        for identifier in (2501, 2502, 2206):
            require(state[str(identifier)]['enabled'], 'Global/instance Settings action is unavailable')
        self.check('no-session Settings disables session actions and keeps global actions', controls=state)
        self.close_settings(dialog)
        self.settings_drafts(False)

    def owned_dialog(self, owner):
        found = []
        @self.enum_callback
        def inspect(window, _):
            pid = w.DWORD()
            self.user.GetWindowThreadProcessId(window, c.byref(pid))
            if (pid.value == self.args.pid and self.user.IsWindowVisible(window)
                    and self.user.GetWindow(window, 4) == owner):
                found.append(window)
            return True
        self.user.EnumWindows(inspect, 0)
        require(len(found) <= 1, 'Multiple visible modal children of Settings')
        return found[0] if found else 0

    def save_from_settings(self, settings, identifier, name, expected=None):
        self.click_window(self.user.GetDlgItem(settings, identifier))
        self.wait(lambda: self.owned_dialog(settings), 'Settings opens a native Save As dialog')
        dialog = self.owned_dialog(settings)
        self.wait(lambda: self.user.GetForegroundWindow() == dialog, 'Save As receives foreground')
        require(not self.user.IsWindowEnabled(settings) and not self.user.IsWindowEnabled(self.window),
                'Nested Save As did not disable both owner levels')
        require(not (self.user.GetWindowLongPtrW(dialog, -20) & 8), 'Save As became globally topmost')
        self.args.root = dialog
        self.screenshot(name + '-save-dialog')
        # The native common dialog uses edt1, or the Edit child of cmb13.
        # Resolve actual controls; never use CDM_SETCONTROLTEXT/WM_SETTEXT to fill it.
        edits = []
        @self.enum_callback
        def inspect(window, _):
            class_name = c.create_unicode_buffer(64)
            self.user.GetClassNameW(window, class_name, len(class_name))
            if (class_name.value.lower() == 'edit' and self.user.IsWindowVisible(window)
                    and self.user.IsWindowEnabled(window)):
                control_id = self.user.GetDlgCtrlID(window)
                parent_id = self.user.GetDlgCtrlID(self.user.GetParent(window))
                if control_id == 0x480 or (control_id == 1001 and parent_id == 0x47c):
                    edits.append(window)
            return True
        self.user.EnumChildWindows(dialog, inspect, 0)
        require(len(edits) == 1, 'Cannot uniquely identify native Save As filename edit')
        path = self.output.resolve() / (self.args.prefix + '-' + name)
        require(not path.exists(), 'Refusing to overwrite a previous Settings evidence file: ' + str(path))
        self.click_window(edits[0])
        self.chord(0x11, 0x41)  # Ctrl+A in the filename edit, never in terminal.
        self.text(str(path))
        self.key(0x0d)
        self.wait(lambda: not self.user.IsWindow(dialog), 'Native Save As closes after saving', 10)
        self.args.root = settings
        self.wait(lambda: self.user.IsWindowEnabled(settings) and
                  self.user.GetForegroundWindow() == settings, 'Settings regains nested-modal focus')
        self.wait(path.exists, 'Settings actually writes the requested file', 10)
        data = path.read_bytes()
        require(data, 'Settings created an empty evidence export')
        if expected is not None:
            require(expected in data, 'Saved log omits the observed real CMD output')
        if identifier == 2206:
            decoded = json.loads(data.decode('utf-8-sig'))
            require(decoded.get('protocol') == 'serialctl.v1' and isinstance(decoded.get('records'), list),
                    'Settings diagnostics export is not the live protocol diagnostic structure')
        self.report['files'].append({'path': str(path), 'bytes': len(data),
                                     'sha256': hashlib.sha256(data).hexdigest()})
        self.check('Settings saves actual ' + name + ' with nested owner/focus intact',
                   bytes=len(data), focus=self.focus_state())

    def settings_session(self):
        original = self.settings_drafts(True)
        dialog = self.open_settings()
        state = self.settings_state(dialog)
        require(state['2504']['count'] == 4 and state['2505']['count'] == 4,
                'CMD Settings lost an encoding or Enter option')
        require(not state['2505']['enabled'] and state['2505']['value'] == 2,
                'CMD must clearly show its fixed CRLF line-submission behavior')
        self.report['settings_capabilities'] = {
            'checkboxes': ['local echo', 'timestamp', 'raw bytes'],
            'encoding_options': ['UTF-8', 'GBK', 'GB2312', 'CMD actual code page'],
            'enter_options': ['CR', 'LF', 'CRLF', 'none'],
            'actions': ['save log', 'clear', 'copy', 'select all', 'paste', 'save diagnostics']}
        self.combo(self.user.GetDlgItem(dialog, 2504))
        self.close_settings(dialog)
        self.report['limitations'].append('CMD Enter is fixed CRLF; presence of four Enter options is inspected, '
                                          'but configurable CR/LF/CRLF/none transmission needs a non-CMD session.')
        # Commit and re-open every encoding option, then restore the caller's state.
        for option in range(4):
            dialog = self.open_settings()
            self.set_settings(dialog, {2504: option})
            self.close_settings(dialog, accept=True)
            dialog = self.open_settings()
            actual = self.setting_values(self.settings_state(dialog), (2504,))
            require(actual == {2504: option}, 'Saved encoding option was not retained')
            self.check('Settings encoding option remains selected after Save', option=option)
            self.close_settings(dialog)
        dialog = self.open_settings()
        self.set_settings(dialog, original)
        self.close_settings(dialog, accept=True)
        token = 'SETTINGS_REAL_' + uuid.uuid4().hex[:10]
        result_file = self.output.resolve() / (self.args.prefix + '-' + token + '.txt')
        self.set_clipboard('echo ' + token + '>>"' + str(result_file) + '"\r\n'
                           'type "' + str(result_file) + '"\r\n')
        dialog = self.open_settings(keyboard=True)
        time.sleep(.2)
        require(not result_file.exists(), 'Keyboard context-menu key pasted terminal input')
        self.check('keyboard context key opens Settings without pasting')
        self.click_window(self.user.GetDlgItem(dialog, 2007))
        # cmd /Q can place stdout directly after its visible '>' prompt.
        # The exact result file below proves execution and prevents an echoed
        # input token from passing this check by itself.
        self.stdout_until((token + '\r\n').encode('ascii'))
        expected = token.encode('ascii') + b'\r\n'
        self.wait(lambda: result_file.exists() and result_file.read_bytes() == expected,
                  'Settings Paste executes the actual command once')
        time.sleep(.2)
        require(result_file.read_bytes() == expected, 'Settings Paste unexpectedly duplicated the command')
        self.report['files'].append({'path': str(result_file), 'bytes': len(expected),
                                     'sha256': hashlib.sha256(expected).hexdigest()})
        require(self.user.IsWindow(dialog), 'Immediate Settings paste unexpectedly closed the dialog')
        self.check('Settings Paste executes real CMD stdout while remaining open', marker=token)
        self.click_window(self.user.GetDlgItem(dialog, 2006))
        self.wait(lambda: self.user.IsWindowEnabled(self.user.GetDlgItem(dialog, 2005)),
                  'Select All enables Copy')
        self.set_clipboard('SETTINGS_COPY_SENTINEL')
        self.click_window(self.user.GetDlgItem(dialog, 2005))
        self.wait(lambda: token in (self.clipboard() or ''), 'Settings Copy contains actual terminal output')
        self.check('Settings Select All / Copy reads real terminal text', marker=token)
        self.save_from_settings(dialog, 2003, 'settings-session.log', token.encode('ascii'))
        self.save_from_settings(dialog, 2206, 'settings-network.json')
        self.click_window(self.user.GetDlgItem(dialog, 2004))
        self.wait(lambda: not self.user.IsWindowEnabled(self.user.GetDlgItem(dialog, 2005)),
                  'Clear removes the active terminal selection')
        self.check('Settings Clear removes active selection')
        self.close_settings(dialog)

    def right_click(self, point):
        require(self.user.WindowFromPoint(POINT(*point)) == self.control(107),
                'Right-click paste point is not inside the real terminal')
        self.move_mouse(point)
        self.user.mouse_event(8, 0, 0, 0, 0)
        self.trace.append({**self.timestamp(), 'event': 'right_down'})
        time.sleep(.025)
        self.user.mouse_event(16, 0, 0, 0, 0)
        self.trace.append({**self.timestamp(), 'event': 'right_up'})
        time.sleep(.08)
        require(not self.user.FindWindowW('#32768', None), 'Terminal right click still opens an old menu')

    def select_range(self, start, end, expected, label):
        self.set_clipboard('SERIALCTL_SELECTION_SENTINEL')
        time.sleep(self.user.GetDoubleClickTime() / 1000 + .02)
        self.move_mouse(start)
        require(self.user.WindowFromPoint(POINT(*start)) == self.control(107),
                'Selection start does not hit the real terminal')
        self.button(True)
        time.sleep(.04)
        for step in range(1, 9):
            self.move_mouse((round(start[0] + (end[0] - start[0]) * step / 8),
                             round(start[1] + (end[1] - start[1]) * step / 8)))
            time.sleep(.015)
        # Copy only on selection completion, not continuously on mouse moves.
        require(self.clipboard() == 'SERIALCTL_SELECTION_SENTINEL',
                'Dragging overwrote clipboard before selection was completed')
        self.button(False)
        self.assert_clipboard(expected, label)
        self.screenshot(label)

    def stdout_until(self, marker, timeout=15):
        deadline = time.monotonic() + timeout
        while marker not in self.raw:
            remaining = deadline - time.monotonic()
            require(remaining > 0, 'Missing real CMD stdout: ' + repr(marker))
            try:
                event = self.client.next_event(self.resource, timeout=remaining)
            except queue.Empty as error:
                raise AssertionError('Missing real CMD stdout: ' + repr(marker)) from error
            self.report['websocket_events'].append(event)
            if event.get('kind') == 'output':
                self.raw.extend(base64.b64decode(event['data'], validate=True))

    def terminal(self):
        require(self.args.native_dir is not None, '--native-dir is required for terminal observations')
        sys.path.insert(0, str(self.args.native_dir.resolve()))
        from serialctl_ws import Client
        terminal = self.control(107)
        self.click_window(terminal)
        self.wait(lambda: self.focus_state()['focus'] == terminal, 'terminal acquires actual focus')
        with Client('127.0.0.1') as self.client:
            resources = self.client.resources()['resources']
            sessions = [item for item in resources if item['kind'] == 'cmd' and item['connected']]
            require(sessions, 'Caller must provide an idle real CMD with an empty draft')
            self.resource = sessions[-1]['id']
            self.report['observed_resource'] = self.resource
            self.client.subscribe(self.resource, self.client.events_since(self.resource, 0)['cursor'])
            self.command('chcp 65001')
            self.wait(lambda: self.client.request('session.get', self.resource)['inputCodePage'] == 65001,
                      'real CMD changes to UTF-8', 10)
            # A one-cell prompt prevents a runner-specific long working
            # directory from scrolling the known selection fixture off-screen.
            self.command('prompt $G')
            self.report['fixture_prompt'] = 'prompt $G (isolated CMD test session)'
            # New shared Settings must be available with an active terminal.
            self.settings_session()
            self.click_window(terminal)
            self.wait(lambda: self.focus_state()['focus'] == terminal, 'focus returns to terminal')
            metrics = {}
            for key in ('CellWidth', 'LineHeight', 'TextLeft', 'TextTop', 'VisibleColumns', 'VisibleRows'):
                value = int(self.user.GetPropW(terminal, 'SerialCtl.' + key) or 0)
                require(value > 0, 'Missing read-only measured terminal geometry: ' + key)
                metrics[key] = value
            self.report['terminal_geometry'] = metrics
            require(metrics['VisibleRows'] >= 8, 'Need at least eight visible terminal rows for fixture')
            require(metrics['VisibleColumns'] >= 24, 'Need at least 24 visible terminal columns for fixture')
            # The two real spaces end the first visual row. A copy path that
            # trims each wrapped row loses these spaces and must fail here.
            long_line = ('SOFT_' + 'q' * (metrics['VisibleColumns'] - 7) +
                         '  ' + 'AFTER_WRAP  TRAILING ')
            line0, line1 = '复制中文 COPYWORD  ', 'HARD_LINE'
            token = uuid.uuid4().hex[:10]
            ready = 'MOUSE_READY_' + token
            payload = '\x1b[3J\x1b[H' + line0 + '\r\n' + line1 + '\r\n' + long_line + '\r\n' + ready + '\r\n'
            script = "[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false);[Console]::Write('" + payload + "')"
            encoded = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
            self.command('powershell -NoProfile -EncodedCommand ' + encoded)
            self.stdout_until(ready.encode())
            time.sleep(.2)
            self.screenshot('selection-fixture-output')
            bounds = self.rect(terminal)
            def cell(column, row):
                return (bounds[0] + metrics['TextLeft'] + column * metrics['CellWidth'] + metrics['CellWidth'] // 2,
                        bounds[1] + metrics['TextTop'] + row * metrics['LineHeight'] + metrics['LineHeight'] // 2)
            self.select_range(cell(0, 0), cell(7, 0), '复制中文', 'drag-chinese-auto-copy')
            self.select_range(cell(0, 0), cell(len(line1) - 1, 1), line0 + '\r\n' + line1,
                              'drag-hard-break-auto-copy')
            end_column = (len(long_line) - 1) % metrics['VisibleColumns']
            end_row = 2 + (len(long_line) - 1) // metrics['VisibleColumns']
            self.select_range(cell(0, 2), cell(end_column, end_row), long_line,
                              'drag-soft-wrap-preserves-written-spaces')
            self.set_clipboard('SERIALCTL_DOUBLE_CLICK_SENTINEL')
            time.sleep(self.user.GetDoubleClickTime() / 1000 + .02)
            point = cell(11, 0)
            self.click(point)
            time.sleep(.06)
            self.click(point)
            self.assert_clipboard('COPYWORD', 'double-click-word-auto-copy')
            self.screenshot('double-click-word-selected')
            time.sleep(self.user.GetDoubleClickTime() / 1000 + .02)
            self.click(cell(0, 6))
            require(self.clipboard() == 'COPYWORD', 'Ordinary non-selection click overwrote clipboard')
            self.check('ordinary click preserves clipboard')
            fixture = Path(os.environ['TEMP']) / ('SerialCtl-mouse-' + token)
            fixture.mkdir()
            once = fixture / 'once.txt'
            marker = 'RIGHT_PASTE_' + token
            self.set_clipboard('echo ' + marker + '>>"' + str(once) + '"\r\n')
            self.right_click(cell(1, 6))
            expected = marker.encode('ascii') + b'\r\n'
            self.wait(lambda: once.exists() and once.read_bytes() == expected,
                      'one right-click executes one command and writes exact file bytes', 10)
            time.sleep(.35)
            require(once.read_bytes() == expected, 'One right-click duplicated the submitted command')
            self.report['files'].append({'path': str(once), 'base64': base64.b64encode(once.read_bytes()).decode(),
                                         'sha256': hashlib.sha256(once.read_bytes()).hexdigest()})
            self.command('type "' + str(once) + '"')
            self.stdout_until(expected)
            self.check('one right-click produces exactly one real command result', expected_bytes=expected.hex())
            tail = fixture / 'tail.txt'
            tail_marker = 'DEFERRED_TAIL_' + token
            self.set_clipboard('echo ' + tail_marker + '>"' + str(tail) + '"')
            self.right_click(cell(1, 6))
            time.sleep(.25)
            require(not tail.exists(), 'Right-click appended an unrequested Enter to an incomplete command')
            self.key(0x0d)
            tail_expected = tail_marker.encode('ascii') + b'\r\n'
            self.wait(lambda: tail.exists() and tail.read_bytes() == tail_expected,
                      'explicit Enter executes pending paste draft', 10)
            self.check('right-click without newline stays a draft until Enter')
            self.report['files'].append({'path': str(tail), 'base64': base64.b64encode(tail.read_bytes()).decode(),
                                         'sha256': hashlib.sha256(tail.read_bytes()).hexdigest()})
            # Reject the complete pasted batch before any prefix executes when
            # its final draft exceeds the CMD limit. Preserve the original draft.
            preserved = fixture / 'preserved.txt'
            rejected = fixture / 'must-not-execute.txt'
            draft_marker = 'PRESERVED_DRAFT_' + token
            draft = 'echo ' + draft_marker + '>"' + str(preserved) + '"'
            self.text(draft)
            draft_length = lambda: int(self.user.GetPropW(terminal, 'SerialCtl.CmdDraftLength') or 0) - 1
            self.wait(lambda: draft_length() == len(draft), 'read-only geometry reports original CMD draft')
            self.set_clipboard('\r\necho REJECTED>"' + str(rejected) + '"\r\n' + 'q' * 8192)
            self.right_click(cell(1, 6))
            time.sleep(.25)
            require(not preserved.exists() and not rejected.exists(),
                    'Oversized multiline paste executed its prefix before rejecting the tail')
            require(draft_length() == len(draft), 'Rejected multiline paste changed the original draft')
            self.key(0x0d)
            draft_expected = draft_marker.encode('ascii') + b'\r\n'
            self.wait(lambda: preserved.exists() and preserved.read_bytes() == draft_expected,
                      'original draft remains exactly executable after oversized-paste rejection', 10)
            require(not rejected.exists(), 'Rejected multiline command ran after the later explicit Enter')
            self.check('oversized multiline paste rejects before any command and preserves original draft',
                       rejected_tail_characters=8192, original_draft_characters=len(draft),
                       actual_result_base64=base64.b64encode(draft_expected).decode())
            self.report['files'].append({'path': str(preserved), 'base64': base64.b64encode(draft_expected).decode(),
                                         'sha256': hashlib.sha256(draft_expected).hexdigest()})
            self.screenshot('right-paste-executed')
            self.report['observed_output_base64'] = base64.b64encode(self.raw).decode()
            self.report['state_after'] = self.client.request('session.get', self.resource)


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--window', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--output', required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--mode', required=True, choices=('controls', 'select-combo', 'terminal', 'no-session'))
    parser.add_argument('--root', type=lambda value: int(value, 0), default=0)
    parser.add_argument('--native-dir', type=Path)
    parser.add_argument('--control-id', type=int)
    parser.add_argument('--selection', type=int)
    return parser.parse_args()


def main():
    args = arguments()
    if os.name != 'nt':
        raise SystemExit('This test requires a real Windows interactive desktop; Linux cannot pass it')
    test = GlobalActions(args)
    code = 1
    try:
        if args.mode == 'controls':
            test.controls()
        elif args.mode == 'select-combo':
            test.select_control_option()
        elif args.mode == 'terminal':
            test.terminal()
        else:
            test.no_session()
        test.report['status'] = 'PASS'
        code = 0
    except Exception as error:
        test.report.update(status='FAIL', error=str(error), traceback=traceback.format_exc())
        print(test.report['traceback'], file=sys.stderr)
        with contextlib.suppress(Exception):
            test.button(False)
            test.screenshot('failure')
    finally:
        test.report['completed_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        test.report['exit_code'] = code
        test.save()
    print(test.report['status'] + ': ' + args.prefix + ' ' + args.mode)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
