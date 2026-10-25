"""Real packaged GUI + persistent CMD: submission is not execution completion.

Uses actual mouse/SendInput through existing desktop helpers, native text reads,
raw gateway input/output events and visible-screen capture. No fake window/CMD.
"""
import argparse
import ctypes as c
import base64
import hashlib
import json
from pathlib import Path
import queue
import sys
import time
import traceback
import uuid

from gui_global_user_actions import GlobalActions
from desktop_user_actions import POINT, RECT, require


class MacroActions(GlobalActions):
    def __init__(self, args):
        super().__init__(args)
        self.user.SetForegroundWindow.argtypes = (c.c_void_p,)
        self.user.SetForegroundWindow.restype = c.c_int
        sys.path.insert(0, args.native_dir)
        from serialctl_ws import Client
        self.client = Client('127.0.0.1')
        self.cmd = [r['id'] for r in self.client.resources()['resources']
                    if r['kind'] == 'cmd' and r['connected']][-1]
        # The earlier GUI smoke produced hundreds of benchmark events. This
        # test observes new macro input, so explicitly query a gap/current cursor
        # without replaying that unrelated history into the 128-event client
        # queue while real keyboard input fills the macro dialog.
        snapshot = self.client.events_since(self.cmd, (1 << 64) - 1)
        require(snapshot['gap'] is True and not snapshot['events'], 'current-cursor snapshot is explicit')
        self.client.subscribe(self.cmd, snapshot['cursor'])
        self.report['subscribe_start_cursor'] = snapshot['cursor']
        self.raw_output = bytearray()
        self.inputs = []
        self.token = ''
        self.expect_resource_gone = self.resource_gone = False
        self.report['scope'] = 'Packaged production GUI, actual mouse/SendInput and real CMD; no device ACK claim'
        self.report['program_sha256'] = hashlib.sha256((Path(args.native_dir) / 'serialctl.exe').read_bytes()).hexdigest()
        self.report['macro_observations'] = []

    def pump(self):
        if self.resource_gone:
            return
        while True:
            try:
                event = self.client.next_event(self.cmd, timeout=.001)
            except queue.Empty:
                return
            except ConnectionError as error:
                # Explicit UI disconnect is required to remove this resource.
                # Accept exactly that terminal event, never a failed socket or
                # receive overflow. Its arrival is asserted by the test below.
                if self.expect_resource_gone and str(error) == 'TARGET_GONE: ' + self.cmd:
                    self.resource_gone = True
                    self.report.setdefault('resource_removals', []).append(
                        {**self.timestamp(), 'resource': self.cmd, 'event': str(error)})
                    return
                raise
            self.report['websocket_events'].append({**self.timestamp(), **event})
            data = base64.b64decode(event.get('data', ''))
            if event.get('kind') == 'output':
                self.raw_output.extend(data)
            if event.get('kind') == 'input' and event.get('source') == 'gui' and self.token.encode() in data:
                self.inputs.append({**self.timestamp(), 'data_base64': event['data']})

    def observe(self, predicate, description, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            self.pump()
            if predicate():
                return
            if time.monotonic() >= deadline:
                state = {'status': self.status(), 'accepted_inputs': self.inputs, 'cmd': self.cmd,
                         'selected_command': self.query(self.control(112), 0x188),
                         'clicks': self.report.get('macro_clicks', []),
                         'fields': self.report.get('macro_fields', []),
                         'completed_observations': self.report['macro_observations'],
                         'recent_events': [{**{k: e.get(k) for k in ('resource', 'kind', 'source', 'seq')},
                                            'data_prefix_base64': e.get('data', '')[:512]}
                                           for e in self.report['websocket_events'][-6:]],
                         'program_sha256': self.report['program_sha256']}
                try: state['connection'] = self.client.request('session.get', self.cmd)
                except Exception as error: state['connection_error'] = str(error)
                self.report['failed_observation'] = state
                raise AssertionError('Timed out: ' + description + '; observed=' + json.dumps(state, ensure_ascii=False))
            time.sleep(.01)

    def status(self):
        return self.native_control_text(self.control(111))

    def fill(self, dialog, identifier, text):
        edit = self.user.GetDlgItem(dialog, identifier)
        require(edit, 'Missing macro field ' + str(identifier))
        def ready():
            if not self.user.IsWindowVisible(edit) or not self.user.IsWindowEnabled(edit):
                return False
            bounds = self.rect(edit)
            center = POINT((bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2)
            return self.user.WindowFromPoint(center) == edit
        # A dialog HWND and its fields exist before ShowWindow/presentation.
        # Observe actual hit-test readiness; never re-click Add or resend input.
        self.wait(ready, 'macro field visible, enabled and exposed: ' + str(identifier))
        self.click_window(edit)
        # Classic numeric EDIT controls do not consistently implement Ctrl+A.
        # All macro fields are single-line: select their full native text with
        # Home + Shift+End, then verify the real value before accepting a draft.
        before = self.native_control_text(edit)
        self.key(0x24)
        self.chord(0x10, 0x23)
        self.text(text)
        initial = self.native_control_text(edit)
        # SendInput queues keyboard events; a synchronous WM_GETTEXT can run
        # before the last queued characters. Observe delivery without retyping.
        deadline = time.monotonic() + 5
        actual = initial
        while actual != text and time.monotonic() < deadline:
            time.sleep(.01)
            actual = self.native_control_text(edit)
        self.report.setdefault('macro_fields', []).append(
            {'id': identifier, 'before': before, 'expected': text, 'initial_read': initial, 'actual': actual})
        require(actual == text, 'Macro field ' + str(identifier) + ' differs: ' + repr(actual) + ' != ' + repr(text))

    def add_macro(self, first, second, interval):
        before = self.query(self.control(112), 0x18b)
        self.click_window(self.control(115))
        opened = []
        def dialog():
            foreground = self.user.GetForegroundWindow()
            if not foreground or foreground == self.window or not self.user.IsWindowVisible(foreground):
                return 0
            owner_pid = c.c_ulong()
            self.user.GetWindowThreadProcessId(foreground, c.byref(owner_pid))
            field = self.user.GetDlgItem(foreground, 1101)
            if (owner_pid.value == self.args.pid and field and self.user.IsWindowVisible(field)
                    and self.user.IsWindowEnabled(field)):
                opened[:] = [foreground]
                return foreground
            return 0
        self.wait(dialog, 'command editor opens')
        root = opened[-1]
        self.args.root = root
        try:
            self.fill(root, 1101, '宏提交验收 ' + self.token)
            self.fill(root, 1102, first)
            self.click_window(self.user.GetDlgItem(root, 1104))
            self.fill(root, 1301, second)
            self.fill(root, 1106, str(interval))
            self.click_window(self.user.GetDlgItem(root, 1))
        finally:
            self.args.root = self.window
        self.wait(lambda: self.query(self.control(112), 0x18b) == before + 1, 'macro added to draft')
        return before

    def run_or_stop(self, index):
        listing = self.control(112)
        row, client = RECT(), RECT()
        require(self.query(listing, 0x198, index, c.addressof(row)) != -1, 'macro row rectangle unavailable')
        require(self.user.GetClientRect(listing, c.byref(client)), 'command list client unavailable')
        require(row.top >= 0 and row.bottom <= client.bottom, 'macro row is outside visible list')
        count = self.query(listing, 0x18b)
        height = self.query(listing, 0x1a1, index)
        lane = round(16 * self.scale) if count * height > client.bottom else 0
        point = POINT(row.right - lane - round(58 * self.scale), (row.top + row.bottom) // 2)
        require(self.user.ClientToScreen(listing, c.byref(point)), 'macro action screen position unavailable')
        self.report.setdefault('macro_clicks', []).append({**self.timestamp(), 'index': index,
            'row': [row.left, row.top, row.right, row.bottom], 'client': [client.left, client.top, client.right, client.bottom],
            'lane': lane, 'screen_point': [point.x, point.y], 'dpi': self.dpi, 'status_before': self.status()})
        self.click_window(listing, (point.x, point.y))

    def remove_macro(self, index):
        before = self.query(self.control(112), 0x18b)
        require(self.query(self.control(112), 0x188) == index, 'macro remains selected for removal')
        self.click_window(self.control(116))
        self.wait(lambda: self.query(self.control(112), 0x18b) == before - 1, 'test draft removed')

    def begin_case(self):
        self.token = uuid.uuid4().hex[:8]
        self.raw_output.clear()
        self.inputs.clear()
        self.expect_resource_gone = self.resource_gone = False

    def completed_submission(self):
        self.begin_case()
        slow = ('SCM_' + self.token + '_SLOW').encode()
        done = ('SCM_' + self.token + '_DONE').encode()
        index = self.add_macro('ping -n 7 127.0.0.1 >nul & echo SCM_' + self.token + '_^SLOW',
                               'echo SCM_' + self.token + '_^DONE', 100)
        self.run_or_stop(index)
        self.observe(lambda: '已提交 2/2' in self.status() and len(self.inputs) == 2, 'two GUI steps accepted', 3)
        status = self.status()
        require('执行完成' not in status and '执行结果见输出' in status, 'submission must not claim execution completion')
        require(slow not in self.raw_output and done not in self.raw_output, 'slow command has no completion stdout yet')
        observation = {**self.timestamp(), 'case': 'submitted_before_execution', 'status': status,
                       'accepted_steps': len(self.inputs), 'completion_stdout_present': False}
        self.report['macro_observations'].append(observation)
        self.screenshot('submitted-before-execution')
        self.observe(lambda: done in self.raw_output, 'real CMD later completes both commands')
        require(slow in self.raw_output and len(self.inputs) == 2, 'two accepted batches correspond to actual CMD output')
        observation['execution_output_observed'] = self.timestamp()
        self.remove_macro(index)

    def stopped_submission(self):
        self.begin_case()
        first_done = ('SCM_' + self.token + '_FIRST').encode()
        not_submitted = ('SCM_' + self.token + '_SECOND').encode()
        barrier = ('SCM_' + self.token + '_BARRIER').encode()
        index = self.add_macro('ping -n 4 127.0.0.1 >nul & echo SCM_' + self.token + '_^FIRST',
                               'echo SCM_' + self.token + '_^SECOND', 5000)
        first_input = len(self.trace)
        self.run_or_stop(index)
        # Native progress is published after the first write succeeds. Waiting
        # for its WebSocket copy can consume the double-click window before we
        # send Stop. Observe the GUI here, then independently verify raw input
        # events after both real clicks; do not loosen the timing requirement.
        self.wait(lambda: '已提交 1/2' in self.status(), 'first macro step accepted in native GUI', 3)
        first_step_status = self.status()
        first_at = time.monotonic()
        self.run_or_stop(index)
        presses = [item for item in self.trace[first_input:] if item['event'] == 'left_down']
        require(len(presses) == 2, 'Run/Stop must use exactly two real mouse presses')
        click_interval_ms = (presses[1]['seconds'] - presses[0]['seconds']) * 1000
        double_click_ms = self.user.GetDoubleClickTime()
        require(click_interval_ms < double_click_ms, 'rapid Run/Stop must exercise the native double-click window')
        self.observe(lambda: '已停止后续提交（1/2）' in self.status() and len(self.inputs) == 1,
                     'stop explains its exact scope and one raw input was accepted', 3)
        status = self.status()
        require('已提交内容可能继续执行' in status, 'stop must not claim cancellation of accepted work')
        self.report['macro_observations'].append({**self.timestamp(), 'case': 'stop_after_one', 'status': status,
                                                  'accepted_steps_at_stop': len(self.inputs),
                                                  'click_interval_ms': click_interval_ms,
                                                  'system_double_click_ms': double_click_ms,
                                                  'timing_basis': 'actual left_down input trace',
                                                  'first_step_native_status': first_step_status})
        self.screenshot('stopped-future-submission')
        self.observe(lambda: first_done in self.raw_output, 'already accepted command continues after Stop')
        while time.monotonic() - first_at < 5.5:
            self.pump()
            time.sleep(.02)
        self.client.input(self.cmd, ('echo SCM_' + self.token + '_^BARRIER\r\n').encode())
        self.observe(lambda: barrier in self.raw_output, 'CMD remains usable beyond the canceled next-step deadline')
        require(len(self.inputs) == 1 and not_submitted not in self.raw_output, 'Stop prevents the unsent second batch')
        self.remove_macro(index)

    def interrupted_submission(self):
        self.begin_case()
        index = self.add_macro('exit & rem ' + self.token, 'echo UNSENT_' + self.token, 1500)
        self.run_or_stop(index)
        self.observe(lambda: '后续提交已中断（1/2）' in self.status(), 'CMD exit interrupts future submission', 8)
        self.pump()
        status = self.status()
        require('执行状态未知' in status and '不自动重发' in status and len(self.inputs) == 1,
                'interruption retains unknown execution state and accepted count')
        self.report['macro_observations'].append({**self.timestamp(), 'case': 'interrupted', 'status': status,
                                                  'accepted_steps': len(self.inputs)})
        self.screenshot('interrupted-unknown')
        self.remove_macro(index)
        self.restore_cmd()

    def restore_cmd(self):
        before = {r['id'] for r in self.client.resources()['resources']}
        self.click_window(self.control(300))
        def fresh():
            return [r['id'] for r in self.client.resources()['resources']
                    if r['kind'] == 'cmd' and r['connected'] and r['id'] not in before]
        self.wait(fresh, 'restore fresh CMD for later smoke tests')
        self.cmd = fresh()[-1]
        self.wait(lambda: self.user.IsWindowEnabled(self.control(105)), 'fresh CMD active in the GUI')
        self.expect_resource_gone = self.resource_gone = False
        snapshot = self.client.events_since(self.cmd, (1 << 64) - 1)
        self.client.subscribe(self.cmd, snapshot['cursor'])

    def explicit_disconnect(self, all_sessions=False):
        self.begin_case()
        index = self.add_macro('ping -n 7 127.0.0.1 >nul & rem ' + self.token,
                               'echo UNSENT_' + self.token, 5000)
        self.run_or_stop(index)
        self.observe(lambda: len(self.inputs) == 1, 'first step accepted before explicit disconnect')
        self.expect_resource_gone = True
        self.click_window(self.control(302 if all_sessions else 105))
        self.observe(lambda: self.resource_gone and '后续提交已中断（1/2）' in self.status(), 'explicit disconnect retains macro outcome', 3)
        status = self.status()
        require('执行状态未知' in status and '不自动重发' in status and len(self.inputs) == 1,
                'explicit disconnect retains accepted count and unknown execution state')
        resources = self.client.resources()['resources']
        require(not any(r['id'] == self.cmd for r in resources), 'disconnected CMD resource removed')
        if all_sessions:
            require(not any(r['kind'] == 'cmd' and r['connected'] for r in resources), 'all CMD sessions removed')
        self.report['macro_observations'].append({**self.timestamp(),
            'case': 'disconnect_all' if all_sessions else 'disconnect_current', 'status': status,
            'accepted_steps': len(self.inputs), 'resource_removed': True})
        self.screenshot('disconnect-all-unknown' if all_sessions else 'disconnect-current-unknown')
        self.remove_macro(index)
        self.restore_cmd()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--window', type=int, required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--native-dir', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--prefix', required=True)
    args = parser.parse_args()
    args.mode, args.root = 'desktop', args.window
    test = MacroActions(args)
    try:
        test.user.SetForegroundWindow(args.window)
        test.wait(lambda: test.user.GetForegroundWindow() == args.window, 'foreground actual production window')
        test.completed_submission()
        test.stopped_submission()
        test.interrupted_submission()
        test.explicit_disconnect()
        test.explicit_disconnect(all_sessions=True)
        test.report['status'] = 'PASS'
    except BaseException as error:
        test.report['status'] = 'FAIL'
        test.report['error'] = traceback.format_exc()
        try: test.screenshot('failure')
        except Exception: pass
        message = str(error).replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
        print('::error title=Macro submission regression::' + message, flush=True)
        raise
    finally:
        test.client.close()
        test.flush_frames()
    print('PASS: real GUI/CMD submitted != executed; Stop preserves accepted work; interrupted state unknown; ' + args.prefix)
    evidence = json.dumps({'prefix': args.prefix, 'program_sha256': test.report['program_sha256'],
                           'observations': test.report['macro_observations']}, ensure_ascii=False)
    print('::notice title=Macro submission evidence::' + evidence.replace('%', '%25'), flush=True)


if __name__ == '__main__':
    main()
