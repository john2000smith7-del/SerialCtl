"""Local Win32 GUI lifecycle check; creates real CMD jobs, no remote permissions."""
import ctypes
from ctypes import wintypes as w
import json
import pathlib
import sys
import time
sys.path.insert(0, sys.argv[1])
from serialctl_ws import Client
window, pid = int(sys.argv[2]), int(sys.argv[3])
u, k = ctypes.windll.user32, ctypes.windll.kernel32
u.GetDlgItem.argtypes = (w.HWND, ctypes.c_int)
u.GetDlgItem.restype = w.HWND
u.SendMessageW.argtypes = (w.HWND, w.UINT, ctypes.c_size_t, ctypes.c_ssize_t)
u.SendMessageW.restype = ctypes.c_ssize_t
u.PostMessageW.argtypes = u.SendMessageW.argtypes
k.CreateToolhelp32Snapshot.argtypes = (w.DWORD, w.DWORD)
k.CreateToolhelp32Snapshot.restype = w.HANDLE
class ProcessEntry(ctypes.Structure):
    _fields_ = [('size',w.DWORD),('uses',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),('module',w.DWORD),
                ('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('exe',w.WCHAR*260)]
k.Process32FirstW.argtypes = k.Process32NextW.argtypes = (w.HANDLE, ctypes.POINTER(ProcessEntry))
k.CloseHandle.argtypes = (w.HANDLE,)
def children():
    snapshot=k.CreateToolhelp32Snapshot(2,0)
    assert snapshot != ctypes.c_void_p(-1).value
    e=ProcessEntry();e.size=ctypes.sizeof(e);parents={};names={}
    try:
        more=k.Process32FirstW(snapshot,ctypes.byref(e))
        while more:
            parents[e.pid]=e.parent;names[e.pid]=e.exe
            more=k.Process32NextW(snapshot,ctypes.byref(e))
    finally:k.CloseHandle(snapshot)
    owned={pid}
    for _ in range(16):
        nxt={p for p,parent in parents.items() if parent in owned}
        if nxt.issubset(owned):break
        owned.update(nxt)
    return {p:names[p] for p in owned if p!=pid and p in names}
def wait(predicate,timeout=10):
    deadline=time.monotonic()+timeout
    while not predicate():
        if time.monotonic()>deadline:raise AssertionError('GUI lifecycle wait timed out')
        time.sleep(.02)
with Client('127.0.0.1') as c:
    count=lambda:len([r for r in c.resources()['resources'] if r['kind']=='cmd' and r['connected']])
    baseline=count()
    for index in range(32):
        assert u.PostMessageW(window,0x111,300,0)
        wait(lambda:count()==baseline+index+1)
    live=children()
    assert sum(name.lower()=='cmd.exe' for name in live.values())==baseline+32,live
    listing=u.GetDlgItem(window,106)
    u.SendMessageW(listing,0x197,12,0) # LB_SETTOPINDEX
    top=u.SendMessageW(listing,0x18e,0,0)
    assert top==12,top
    selection=u.SendMessageW(listing,0x188,0,0)
    # The production state-notification refresh path must retain both anchors.
    for _ in range(20):u.SendMessageW(window,0x8009,0,0)
    assert u.SendMessageW(listing,0x18e,0,0)==top
    assert u.SendMessageW(listing,0x188,0,0)==selection
    u.SendMessageW(listing,0x20a,ctypes.c_size_t((-120 & 65535)<<16).value,0)
    assert u.SendMessageW(listing,0x18e,0,0)>top,'wheel must scroll native/overlay list'
    u.SendMessageW(listing,0x100,0x24,0) # keyboard Home
    u.SendMessageW(listing,0x100,0x23,0) # keyboard End
    wait(lambda:u.SendMessageW(listing,0x188,0,0)==u.SendMessageW(listing,0x18b,0,0)-1)
    resource=sorted([r for r in c.resources()['resources'] if r['kind']=='cmd'],key=lambda r:int(r['id'].split('-')[1]))[-1]['id']
    c.input(resource,b'ping -n 100 127.0.0.1 >nul\r\n')
    time.sleep(.1)
    started=time.monotonic()
    assert u.PostMessageW(window,0x111,302,0)
    wait(lambda:not [r for r in c.resources()['resources'] if r['kind']=='cmd'])
    wait(lambda:u.SendMessageW(listing,0x18b,0,0)==0)
    wait(lambda:not children())
    duration=time.monotonic()-started
    report={'created_real_cmd_sessions':baseline+32,'child_processes_before':live,'children_after':children(),
            'disconnect_seconds':duration,'top_anchor':top,'selection_anchor':selection,
            'checks':['stable refresh','native wheel','keyboard Home/End','real GUI all-disconnect','job child termination'],
            'hardware':'no physical serial/SFTP/power; task-owned output covered separately by PowerService controlled tests'}
    pathlib.Path(sys.argv[4]).write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('PASS: %d real CMD sessions, list anchors/wheel/keys, all-disconnect child cleanup %.3fs'%(baseline+32,duration))
