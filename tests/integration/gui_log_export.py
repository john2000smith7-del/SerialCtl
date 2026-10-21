"""Actual packaged Settings export of >=65 MiB, live CMD input and fixed-prefix SHA256.

The source comes from harmless REM batches through the existing production CMD
input operation. Read-only metrics observe state; no test command can export,
slow down, cancel or mutate production state. Controlled slow I/O/failures are
covered separately by the native SessionLogger regression.
"""
import argparse
import base64
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time
import traceback
import uuid

from desktop_user_actions import require
from gui_global_user_actions import GlobalActions


class ExportActions(GlobalActions):
    def __init__(self, args):
        super().__init__(args)
        sys.path.insert(0, str(args.native_dir))
        from serialctl_ws import Client
        self.client = Client('127.0.0.1')
        self.healthy = Client('127.0.0.1')
        self.resource = [r['id'] for r in self.client.resources()['resources']
                         if r['kind'] == 'cmd' and r['connected']][-1]
        cursor = self.client.events_since(self.resource, (1 << 64) - 1)['cursor']
        self.client.subscribe(self.resource, cursor)
        self.raw = bytearray()
        self.events = 0
        self.report['program_sha256'] = hashlib.sha256((args.native_dir / 'serialctl.exe').read_bytes()).hexdigest()
        self.report['scope'] = 'Actual packaged GUI, Settings/native Save As, real CMD REM input, normal-speed local Win32 file export'
        self.report['not_run'] = ['actual slow/failed SMB', 'physical disk full', 'Win7 hardware', 'non-default DPI',
                                  'independent screenshot visual review']
        self.report['health_observations'] = []
        self.monitor_stop = threading.Event()
        self.monitor = None
        self.token = uuid.uuid4().hex[:8]
        self.barrier_id = 0

    def prop(self, name):
        return int(self.user.GetPropW(self.window, 'SerialCtl.' + name) or 0)

    def drain(self):
        while True:
            try: event = self.client.next_event(self.resource, timeout=.001)
            except queue.Empty: return
            self.events += 1
            if event.get('kind') == 'output':
                self.raw.extend(base64.b64decode(event.get('data', ''), validate=True))
                del self.raw[:-8192]

    def barrier(self, prefix=b''):
        self.barrier_id += 1
        marker = ('EX_' + self.token + '_B' + str(self.barrier_id)).encode()
        self.raw.clear()
        data = prefix + ('echo EX_' + self.token + '_^B' + str(self.barrier_id) + '\r\n').encode()
        self.client.input(self.resource, data)
        deadline = time.monotonic() + 10
        while marker not in self.raw:
            self.drain()
            require(time.monotonic() < deadline, 'actual CMD FIFO output barrier')
            time.sleep(.002)

    def create_large_source(self):
        settings = self.open_settings()
        self.set_settings(settings, {2503: 1})
        self.close_settings(settings, accept=True)
        self.barrier()  # refresh read-only byte count for the current session
        started = time.monotonic()
        batch = (b'rem ' + b'A' * 3994 + b'\r\n') * 8
        count = 0
        while self.prop('LogAcceptedBytes') < 65 * 1024 * 1024:
            require(count < 1500 and time.monotonic() - started < 180, 'large-source fixture time/size bound')
            self.barrier(batch)
            count += 1
        self.report['source'] = {'batches': count, 'accepted_bytes': self.prop('LogAcceptedBytes'),
                                 'elapsed_seconds': round(time.monotonic() - started, 3),
                                 'event_count': self.events}
        # Existing error text must retain priority over automatic export feedback.
        self.set_clipboard('\x1b')
        self.click_window(self.control(107))
        bounds = self.rect(self.control(107))
        self.right_click((bounds[0] + 40, bounds[1] + 40))
        self.wait(lambda: '不支持的控制字符' in self.native_control_text(self.control(111)), 'existing invalid-paste warning')
        self.warning = self.native_control_text(self.control(111))

    def observe_export(self, target):
        def run():
            try:
                while not self.monitor_stop.is_set():
                    busy = bool(self.prop('LogExportBusy'))
                    started = time.perf_counter()
                    self.query(self.window, 0)  # WM_NULL; actual UI message pump roundtrip
                    ui_ms = round((time.perf_counter() - started) * 1000, 3)
                    started = time.perf_counter()
                    resources = self.healthy.resources()
                    api_ms = round((time.perf_counter() - started) * 1000, 3)
                    require(any(r['id'] == self.resource for r in resources['resources']), 'healthy client retains source resource')
                    self.report['health_observations'].append({'busy_at_start': busy, 'ui_ms': ui_ms, 'api_ms': api_ms})
                    if busy:
                        # Real later accepted CMD input; cutoff must exclude it.
                        self.healthy.input(self.resource, b'rem LATER_THAN_EXPORT_CUTOFF\r\n')
                    self.monitor_stop.wait(.005)
            except BaseException:
                self.report['monitor_error'] = traceback.format_exc()
        self.monitor = threading.Thread(target=run)
        self.monitor.start()

    def export(self):
        settings = self.open_settings()
        require(not self.user.IsWindowEnabled(self.user.GetDlgItem(settings, 2207)), 'idle cancel disabled')
        self.save_from_settings(settings, 2003, 'large-background.log', before_accept=self.observe_export)
        self.wait(lambda: not self.prop('LogExportBusy'), 'background task completion', 15)
        self.wait(lambda: '日志已保存' in self.native_control_text(self.user.GetDlgItem(settings, 2506)),
                  'persistent export result in Settings')
        self.monitor_stop.set()
        self.monitor.join(10)
        require(not self.monitor.is_alive() and 'monitor_error' not in self.report, 'healthy client/UI monitor completes')
        require(self.native_control_text(self.control(111)) == self.warning, 'export result must preserve existing error warning')
        require(not self.user.IsWindowEnabled(self.user.GetDlgItem(settings, 2207)), 'finished cancel disabled')
        self.report['settings_note'] = self.native_control_text(self.user.GetDlgItem(settings, 2506))
        self.report['preserved_warning'] = self.warning
        self.screenshot('export-completed')
        resolution = self.report['save_dialog_controls'][-1]
        filename = resolution['selected']['text']
        source = Path(os.environ['LOCALAPPDATA']) / 'SerialCtl' / 'logs' / filename
        if not source.is_file(): source = source.with_suffix('.log')
        require(source.is_file(), 'native picker source filename identifies actual session log')
        cutoff = self.prop('LogExportCutoff')
        remaining = cutoff
        expected = hashlib.sha256()
        part = 1
        while remaining:
            path = source if part == 1 else source.with_name(source.stem + '.part' + str(part) + source.suffix)
            with path.open('rb') as file:
                while remaining:
                    data = file.read(min(65536, remaining))
                    if not data: break
                    expected.update(data)
                    remaining -= len(data)
            part += 1
            require(part <= 10, 'source prefix reconstruction bound')
        exported = self.report['files'][-1]
        require(cutoff >= 65 * 1024 * 1024 and exported['bytes'] == cutoff, 'complete fixed-cutoff large export')
        require(exported['sha256'] == expected.hexdigest(), 'export SHA256 equals exact actual source prefix')
        observations = self.report['health_observations']
        require(observations, 'UI and healthy client actually observed')
        require(max(x['ui_ms'] for x in observations) < 2000 and max(x['api_ms'] for x in observations) < 2000,
                'UI/healthy client responsiveness bound')
        self.report['export'] = {'cutoff': cutoff, 'bytes': exported['bytes'], 'sha256': exported['sha256'],
            'source_prefix_sha256': expected.hexdigest(), 'parts_read': part - 1,
            'observed_busy_samples': sum(x['busy_at_start'] for x in observations),
            'ui_max_ms': max(x['ui_ms'] for x in observations), 'api_max_ms': max(x['api_ms'] for x in observations),
            'normal_speed_only': True}
        self.close_settings(settings)
        # Avoid leaking optional raw-byte tracing into later smoke cases.
        settings = self.open_settings()
        self.set_settings(settings, {2503: 0})
        self.close_settings(settings, accept=True)
        self.barrier()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--window', type=lambda v: int(v, 0), required=True)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--native-dir', type=Path, required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--prefix', required=True)
    args = parser.parse_args()
    args.root, args.mode = args.window, 'desktop'
    test = ExportActions(args)
    try:
        test.create_large_source()
        test.export()
        test.report['status'] = 'PASS'
        summary = {k: test.report[k] for k in ('program_sha256', 'source', 'export', 'settings_note', 'preserved_warning')}
        summary.update(dpi=test.dpi, scope=test.report['scope'], not_run=test.report['not_run'])
        print('::notice title=GUI background log export evidence::' + json.dumps(summary, ensure_ascii=False).replace('%', '%25'), flush=True)
    except BaseException:
        test.report['status'] = 'FAIL'
        test.report['error'] = traceback.format_exc()
        print('::error title=GUI background log export regression::' + test.report['error'].replace('\n', '%0A'), flush=True)
        raise
    finally:
        test.monitor_stop.set()
        if test.monitor: test.monitor.join(10)
        test.client.close()
        test.healthy.close()
        test.flush_frames()


if __name__ == '__main__':
    main()
