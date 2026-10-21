"""Real packaged CMD clipboard preflight: bytes, draft, caret and no implicit Enter."""
import argparse
import base64
import ctypes as c
import hashlib
import json
from pathlib import Path
import queue
import sys
import time
import traceback
import uuid

from gui_global_user_actions import GlobalActions
from desktop_user_actions import require


class PasteActions(GlobalActions):
    def __init__(self, args):
        super().__init__(args)
        self.user.SetForegroundWindow.argtypes = (c.c_void_p,)
        self.user.SetForegroundWindow.restype = c.c_int
        sys.path.insert(0, args.native_dir)
        from serialctl_ws import Client
        self.client = Client('127.0.0.1')
        self.resource = [r['id'] for r in self.client.resources()['resources']
                         if r['kind'] == 'cmd' and r['connected']][-1]
        snapshot = self.client.events_since(self.resource, (1 << 64) - 1)
        require(snapshot['gap'] and not snapshot['events'], 'explicit current cursor without old history')
        self.client.subscribe(self.resource, snapshot['cursor'])
        self.report['subscribe_start_cursor'] = snapshot['cursor']
        self.report['program_sha256'] = hashlib.sha256((Path(args.native_dir) / 'serialctl.exe').read_bytes()).hexdigest()
        self.report['paste_observations'] = []
        self.report['scope'] = 'Production clipboard branch, actual right mouse/SendInput, real CMD and production input/output events'
        self.gui_chunks = []
        self.token = uuid.uuid4().hex[:8]
        self.barrier_index = 0

    def pump(self):
        while True:
            try:
                event = self.client.next_event(self.resource, timeout=.001)
            except queue.Empty:
                return
            self.report['websocket_events'].append({**self.timestamp(), **event})
            data = base64.b64decode(event.get('data', ''), validate=True)
            if event.get('kind') == 'output':
                self.raw.extend(data)
            elif event.get('kind') == 'input' and event.get('source') == 'gui':
                self.gui_chunks.append(data)

    def observe(self, predicate, description, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            self.pump()
            if predicate():
                return
            require(time.monotonic() < deadline, 'Timed out: ' + description)
            time.sleep(.01)

    def draft_length(self):
        value = int(self.user.GetPropW(self.control(107), 'SerialCtl.CmdDraftLength') or 0)
        require(value > 0, 'missing opt-in read-only draft length')
        return value - 1

    def status(self):
        return self.native_control_text(self.control(111))

    def paste(self, text):
        self.set_clipboard(text)
        bounds = self.rect(self.control(107))
        self.right_click((bounds[0] + 40, bounds[1] + 40))

    def barrier(self):
        self.barrier_index += 1
        marker = ('PB_' + self.token + '_B' + str(self.barrier_index)).encode()
        command = 'echo PB_' + self.token + '_^B' + str(self.barrier_index) + '\r\n'
        self.client.input(self.resource, command.encode())
        self.observe(lambda: marker in self.raw, 'real CMD FIFO output barrier')

    def begin(self):
        self.pump()
        self.raw.clear()
        self.gui_chunks.clear()
        require(self.draft_length() == 0, 'each case starts with an empty CMD draft')

    def assert_gui(self, expected):
        # Publish splits input into <=16384-byte events. Concatenate bytes;
        # event count is deliberately not treated as accepted batch count.
        actual = b''.join(self.gui_chunks)
        require(actual == expected, 'GUI input bytes differ: actual=' + str(len(actual)) +
                '/' + hashlib.sha256(actual).hexdigest() + ' expected=' + str(len(expected)) +
                '/' + hashlib.sha256(expected).hexdigest())

    def record(self, case, **details):
        self.report['paste_observations'].append({**self.timestamp(), 'case': case,
            'gui_bytes': sum(map(len, self.gui_chunks)), 'gui_event_chunks': len(self.gui_chunks),
            'gui_sha256': hashlib.sha256(b''.join(self.gui_chunks)).hexdigest(), **details})

    def set_page(self, page):
        self.client.input(self.resource, ('chcp ' + str(page) + '\r\n').encode())
        self.barrier()
        require(self.client.request('session.get', self.resource)['inputCodePage'] == page,
                'real console code page changes to ' + str(page))

    def rejected(self, clipboard, reason, caret=5, label='rejected'):
        self.begin()
        original = 'echo ORIG_' + self.token + '_END'
        self.text(original)
        self.key(0x24)
        for _ in range(caret):
            self.key(0x27)
        self.observe(lambda: self.draft_length() == len(original), 'original draft painted')
        self.paste(clipboard)
        self.observe(lambda: reason in self.status(), 'complete paste rejected')
        self.barrier()
        self.assert_gui(b'')
        require(self.draft_length() == len(original), 'rejection preserves draft length')
        status = self.status()
        self.screenshot(label)
        # The exact accepted line proves both full draft preservation and the
        # unchanged caret, without adding a production editor mutation hook.
        self.text('KEPT')
        self.key(0x0d)
        expected = (original[:caret] + 'KEPT' + original[caret:] + '\r\n').encode()
        self.barrier()
        self.assert_gui(expected)
        self.observe(lambda: self.draft_length() == 0, 'explicit Enter clears original draft')
        self.record(label, rejected_clipboard_units=len(clipboard), rejected_gui_bytes=0,
                    original_draft_units=len(original), preserved_caret=caret,
                    later_enter_base64=base64.b64encode(expected).decode(), rejection_status=status)

    def boundaries(self):
        original = 'echo ORIG_' + self.token + '_END'
        for newline in ('\r', '\n', '\r\n'):
            for caret in (0, 5, len(original)):
                for length in (8190, 8191, 8192):
                    suffix = original[caret:]
                    inserted_tail = 'x' * (length - len(suffix))
                    clipboard = 'rem PREFIX_' + self.token + newline + inserted_tail
                    label = 'tail-' + str(length) + '-nl-' + newline.encode().hex() + '-caret-' + str(caret)
                    if length == 8192:
                        self.rejected(clipboard, '超长行', caret, label)
                        continue
                    self.begin()
                    self.text(original)
                    self.key(0x24)
                    for _ in range(caret): self.key(0x27)
                    self.paste(clipboard)
                    self.observe(lambda: self.draft_length() == length, 'valid boundary tail remains draft')
                    self.barrier()
                    expected = (original[:caret] + 'rem PREFIX_' + self.token + '\r\n').encode()
                    self.assert_gui(expected)
                    self.key(0x1b)
                    self.observe(lambda: self.draft_length() == 0, 'Escape clears only incomplete test tail')
                    self.record(label, tail_units=length, no_implicit_enter=True, original_caret=caret)

    def ordinary_multiline(self):
        for newline in ('\r', '\n', '\r\n'):
            self.begin()
            first = 'PASTE_' + self.token + '_FIRST_' + newline.encode().hex()
            tail = 'PASTE_' + self.token + '_TAIL_' + newline.encode().hex()
            command = 'echo PASTE_' + self.token + '_^FIRST_' + newline.encode().hex()
            draft = 'echo PASTE_' + self.token + '_^TAIL_' + newline.encode().hex()
            self.paste(command + newline + draft)
            self.observe(lambda: first.encode() in self.raw and self.draft_length() == len(draft), 'completed line runs with deferred tail')
            self.barrier()  # asynchronous real stdout must preserve pending draft
            self.assert_gui((command + '\r\n').encode())
            require(tail.encode() not in self.raw and self.draft_length() == len(draft), 'stdout cannot submit or replace draft')
            self.key(0x0d)
            self.observe(lambda: tail.encode() in self.raw, 'explicit Enter alone executes tail')
            self.barrier()
            self.assert_gui((command + '\r\n' + draft + '\r\n').encode())
            self.observe(lambda: self.draft_length() == 0, 'valid multiline draft completed')
            self.record('ordinary-' + newline.encode().hex(), async_stdout_preserves_draft=True,
                        explicit_enter_required=True, raw_stdout_sha256=hashlib.sha256(self.raw).hexdigest())

    def encoded_and_batch_limits(self):
        self.rejected('rem PREFIX\r\n' + '中' * 2731, '8191 字节', label='utf8-encoded-tail-over-limit')
        self.rejected('x' * 65537, '剪贴板文本超过粘贴上限', label='clipboard-units-over-limit')
        self.rejected('rem PREFIX\r\n\x1b[A', '控制字符', label='control-in-tail')
        self.set_page(437)
        self.rejected('rem PREFIX\r\n中', '无法表示输入', label='unrepresentable-cp437-tail')
        self.set_page(65001)
        self.begin()
        self.paste('rem PREFIX\r\n' + '中' * 2730)
        self.observe(lambda: self.draft_length() == 2730, '8190-byte UTF8 tail is valid')
        self.barrier()
        self.assert_gui(b'rem PREFIX\r\n')
        self.key(0x1b)
        self.observe(lambda: self.draft_length() == 0, 'clear UTF8 test draft')
        self.record('utf8-tail-8190-bytes', tail_characters=2730, encoded_tail_bytes=8190)
        def batch(last):
            return '\n'.join(('rem ' + 'x' * ((last if i == 7 else 8191) - 4)) for i in range(8)) + '\n'
        self.begin()
        rejected = batch(8184)
        require(len(rejected.replace('\n', '\r\n').encode()) == 65537, 'reject exactly one byte above normalized boundary')
        self.paste(rejected)
        self.observe(lambda: '65536 字节' in self.status(), 'normalized batch rejected before any prefix')
        self.barrier()
        self.assert_gui(b'')
        require(self.draft_length() == 0, 'over-limit batch leaves empty draft unchanged')
        self.record('normalized-batch-65537', raw_clipboard_units=len(rejected),
                    normalized_bytes=65537, rejected_gui_bytes=0, rejection_status=self.status())
        self.begin()
        clipboard = batch(8183)
        expected = clipboard.replace('\n', '\r\n').encode()
        require(len(expected) == 65536 and len(clipboard) < 65536, 'exercise normalized batch boundary')
        self.paste(clipboard)
        self.barrier()
        self.assert_gui(expected)
        require(self.draft_length() == 0, 'exact batch has no incomplete tail')
        self.record('normalized-batch-65536', raw_clipboard_units=len(clipboard),
                    normalized_bytes=65536, input_sha256=hashlib.sha256(expected).hexdigest())
        self.screenshot('paste-acceptance-finished')


def main():
    parser = argparse.ArgumentParser()
    for name in ('window', 'pid'): parser.add_argument('--' + name, type=int, required=True)
    for name in ('native-dir', 'output', 'prefix'): parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    args.root, args.mode = args.window, 'desktop'
    test = PasteActions(args)
    try:
        test.user.SetForegroundWindow(args.window)
        test.wait(lambda: test.user.GetForegroundWindow() == args.window, 'actual foreground window')
        test.click_window(test.control(107))
        test.wait(lambda: test.focus_state()['focus'] == test.control(107), 'actual terminal focus')
        test.set_page(65001)
        test.boundaries()
        test.ordinary_multiline()
        test.encoded_and_batch_limits()
        test.report['status'] = 'PASS'
    except BaseException as error:
        test.report['status'] = 'FAIL'
        test.report['error'] = traceback.format_exc()
        context = {'error': str(error), 'status': test.status(), 'draft_units': test.draft_length(),
                   'gui_bytes': sum(map(len, test.gui_chunks)),
                   'completed_cases': test.report['paste_observations'],
                   'program_sha256': test.report['program_sha256']}
        try: test.screenshot('paste-failure')
        except Exception: pass
        message = json.dumps(context, ensure_ascii=False).replace('%', '%25').replace('\n', '%0A').replace('\r', '%0D')
        print('::error title=CMD paste regression::' + message, flush=True)
        raise
    finally:
        test.client.close()
        test.flush_frames()
    evidence = {'prefix': args.prefix, 'program_sha256': test.report['program_sha256'], 'dpi': test.dpi,
                'observations': test.report['paste_observations']}
    print('::notice title=CMD paste evidence::' + json.dumps(evidence, ensure_ascii=False).replace('%', '%25'), flush=True)


if __name__ == '__main__':
    main()
