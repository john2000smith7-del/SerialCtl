"""Official MCP SDK against the fully extracted native Windows candidate."""
import argparse
import asyncio
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import time
from types import SimpleNamespace
import uuid
import zipfile

import httpx
from mcp import ClientSession, McpError
from mcp.client.streamable_http import streamablehttp_client
from desktop_user_actions import DesktopActions, require


async def exercise(url):
    async with streamablehttp_client(url) as (read, write, _):
        async with ClientSession(read, write) as session:
            initialized = await session.initialize()
            require(initialized.protocolVersion == '2025-03-26', 'Unexpected negotiated MCP version')
            tools = await session.list_tools()
            names = {tool.name for tool in tools.tools}
            require({'app.status', 'session.list', 'power.get', 'power.output', 'tftp.status'} <= names,
                    'Required MCP tools missing')
            results = {}
            for name in ['app.status', 'session.list', 'power.get', 'tftp.status']:
                result = await session.call_tool(name, {})
                require(not result.isError, 'Safe tool failed: ' + name)
                results[name] = result.model_dump(mode='json')
            denied = False
            try:
                result = await session.call_tool('power.output', {'enabled': True, 'voltage': 3.3})
                denied = result.isError
            except McpError as error:
                denied = error.error.code == -32001
            require(denied, 'Forbidden power parameters were accepted')
            return {'protocolVersion': initialized.protocolVersion, 'tools': sorted(names),
                    'safe_results': results, 'forbidden_power_rejected': denied, 'sdk': 'mcp 1.20.0'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(os.name == 'nt' and os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted',
            'Requires an isolated hosted Windows test account')
    args.output.mkdir(parents=True, exist_ok=False)
    app = args.output / 'fully-extracted'
    with zipfile.ZipFile(args.archive) as archive:
        archive.extractall(app)
        manifest = json.loads(archive.read('MANIFEST.json'))
        require(manifest['version'] == 'V2.0.20', 'Candidate version mismatch')
    # Disable update checks only in this extracted test instance; the ZIP stays byte-for-byte intact.
    (app / 'update.ini').write_text('[update]\nurl=\n', encoding='ascii')
    config = Path(os.environ['APPDATA']) / 'SerialCtl'
    backup = config.with_name('SerialCtl.mcp-sdk-backup-' + uuid.uuid4().hex)
    moved = False
    process = None
    user = c.WinDLL('user32', use_last_error=True)
    user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    enum_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [enum_type, w.LPARAM]

    def windows(pid):
        result = []
        @enum_type
        def visit(handle, _):
            owner = w.DWORD()
            user.GetWindowThreadProcessId(handle, c.byref(owner))
            name = c.create_unicode_buffer(128)
            user.GetClassNameW(handle, name, 128)
            if owner.value == pid and name.value == 'SerialCtlWin7NativeWindowV3':
                result.append(handle)
            return True
        user.EnumWindows(visit, 0)
        return result

    report = {'status': 'RUNNING', 'source_commit': os.environ['GITHUB_SHA'],
              'build_run': os.environ['GITHUB_RUN_ID'], 'os': platform.platform(),
              'archive_sha256': hashlib.sha256(args.archive.read_bytes()).hexdigest(),
              'scope': 'Official SDK + actual extracted Win32 GUI; no physical devices or Win7 execution'}
    try:
        if config.exists():
            os.replace(config, backup)
            moved = True
        config.mkdir()
        with socket.socket() as temporary:
            temporary.bind(('127.0.0.1', 0))
            port = temporary.getsockname()[1]
        (config / 'settings.ini').write_text(
            f'[Mcp]\nEnabled=1\nPort={port}\nBindIp=127.0.0.1\n[Pet]\nEnabled=0\n', encoding='ascii')
        process = subprocess.Popen([str((app / 'serialctl.exe').resolve())], cwd=app)
        deadline = time.monotonic() + 20
        handles = []
        while time.monotonic() < deadline:
            handles = windows(process.pid)
            try:
                if handles and httpx.get(f'http://127.0.0.1:{port}/health', timeout=1).status_code == 200:
                    break
            except httpx.HTTPError:
                pass
            require(process.poll() is None, 'Packaged process exited during startup')
            time.sleep(.05)
        require(handles, 'Actual packaged GUI did not open')
        report.update(asyncio.run(exercise(f'http://127.0.0.1:{port}/mcp')))
        desktop = DesktopActions(SimpleNamespace(window=handles[0], pid=process.pid,
            output=str(args.output), mode='desktop', prefix='mcp-sdk'))
        desktop.capture('mcp-sdk-actual-package')
        desktop.flush_frames()
        # Exercise a real request racing ordinary GUI exit.
        user.PostMessageW(handles[0], 0x10, 0, 0)
        process.wait(timeout=15)
        require(process.returncode == 0, 'Packaged GUI exit failed')
        with socket.socket() as client:
            require(client.connect_ex(('127.0.0.1', port)) != 0, 'MCP listener survived GUI exit')
        require(hashlib.sha256(args.archive.read_bytes()).hexdigest() == report['archive_sha256'], 'Candidate ZIP changed')
        report['status'] = 'PASS'
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait()
        if config.exists():
            shutil.rmtree(config)
        if moved:
            os.replace(backup, config)
        (args.output / 'mcp-sdk-results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
