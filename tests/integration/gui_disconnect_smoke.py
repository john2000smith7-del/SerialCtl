"""Local Win32 GUI lifecycle check; creates real CMD jobs, no remote permissions."""
import ctypes
from ctypes import wintypes as w
import json
import pathlib
import subprocess
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
u.GetClientRect.argtypes = u.GetWindowRect.argtypes = (w.HWND, ctypes.POINTER(w.RECT))
u.ClientToScreen.argtypes = (w.HWND, ctypes.POINTER(w.POINT))
u.SetForegroundWindow.argtypes = (w.HWND,)
u.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
u.mouse_event.argtypes = (w.DWORD,w.DWORD,w.DWORD,w.DWORD,ctypes.c_size_t)
k.CreateToolhelp32Snapshot.argtypes = (w.DWORD, w.DWORD)
k.CreateToolhelp32Snapshot.restype = w.HANDLE
class ProcessEntry(ctypes.Structure):
    _fields_ = [('size',w.DWORD),('uses',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),('module',w.DWORD),
                ('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('exe',w.WCHAR*260)]
k.Process32FirstW.argtypes = k.Process32NextW.argtypes = (w.HANDLE, ctypes.POINTER(ProcessEntry))
k.CloseHandle.argtypes = (w.HANDLE,)
k.OpenProcess.argtypes = (w.DWORD,w.BOOL,w.DWORD)
k.OpenProcess.restype = w.HANDLE
k.GetProcessTimes.argtypes = (w.HANDLE,ctypes.POINTER(w.FILETIME),ctypes.POINTER(w.FILETIME),
                             ctypes.POINTER(w.FILETIME),ctypes.POINTER(w.FILETIME))
def creation(process_id):
    handle=k.OpenProcess(0x1000,False,process_id)
    if not handle:return None
    times=[w.FILETIME() for _ in range(4)]
    try:
        if not k.GetProcessTimes(handle,*(ctypes.byref(t) for t in times)):return None
        return (times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
    finally:k.CloseHandle(handle)
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
    times={p:creation(p) for p in parents}
    owned={pid}
    for _ in range(16):
        # Parent PIDs can be reused after compiler processes exit. A child that
        # predates its apparent parent is not a descendant of this GUI instance.
        nxt={p for p,parent in parents.items() if parent in owned and times.get(p) is not None
             and times.get(parent) is not None and times[p]>=times[parent]}
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
    # IsConnected means the native bridge transport is established; its real CMD
    # is created asynchronously. Keep the exact process assertion after readiness.
    wait(lambda:sum(name.lower()=='cmd.exe' for name in children().values())==baseline+32)
    live=children()
    assert sum(name.lower()=='cmd.exe' for name in live.values())==baseline+32,live
    listing=u.GetDlgItem(window,106)
    u.SendMessageW(listing,0x197,12,0) # LB_SETTOPINDEX
    top=u.SendMessageW(listing,0x18e,0,0)
    assert top==12,top
    assert u.PostMessageW(window,0x111,300,0)
    wait(lambda:count()==baseline+33)
    assert u.SendMessageW(listing,0x18e,0,0)==top,'top resource ID must survive a rebuilt list with duplicate CMD names'
    live=children()
    known_processes={p:creation(p) for p in live}
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
    # Physical mouse drag of the visible overlay thumb, with real desktop captures.
    bounds=w.RECT();origin=w.POINT(0,0)
    assert u.GetClientRect(listing,ctypes.byref(bounds)) and u.ClientToScreen(listing,ctypes.byref(origin))
    item_height=u.SendMessageW(listing,0x1a1,0,0)
    count_items=u.SendMessageW(listing,0x18b,0,0)
    page=max(1,(bounds.bottom-bounds.top)//item_height)
    max_top=count_items-page
    track_height=bounds.bottom-16
    thumb_height=min(track_height,max(24,track_height*page//count_items))
    current_top=u.SendMessageW(listing,0x18e,0,0)
    thumb_top=8+(track_height-thumb_height)*min(max_top,current_top)//max_top
    mouse_x=origin.x+bounds.right-8
    start_y=origin.y+thumb_top+thumb_height//2
    u.SetForegroundWindow(window)
    assert u.SetCursorPos(mouse_x,start_y)
    u.mouse_event(2,0,0,0,0)
    time.sleep(.03)
    images=[]
    for frame in range(4):
        destination=pathlib.Path(sys.argv[4]).with_name('gui-connections-drag-%d.png'%frame)
        assert u.SetCursorPos(mouse_x,start_y-(start_y-origin.y-16)*(frame+1)//4)
        time.sleep(.03)
        literal=str(destination).replace("'","''")
        script=("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
                "$s=[Windows.Forms.Screen]::PrimaryScreen.Bounds;"
                "$b=New-Object Drawing.Bitmap($s.Width,$s.Height);$g=[Drawing.Graphics]::FromImage($b);"
                "$g.CopyFromScreen(0,0,0,0,$s.Size);$b.Save('"+literal+"',[Drawing.Imaging.ImageFormat]::Png);"
                "$g.Dispose();$b.Dispose()")
        subprocess.run(['powershell.exe','-NoProfile','-Command',script],check=True,timeout=10)
        images.append(destination.name)
    u.mouse_event(4,0,0,0,0)
    wait(lambda:u.SendMessageW(listing,0x18e,0,0)==0)
    resource=sorted([r for r in c.resources()['resources'] if r['kind']=='cmd'],key=lambda r:int(r['id'].split('-')[1]))[-1]['id']
    c.input(resource,b'ping -n 100 127.0.0.1 >nul\r\n')
    time.sleep(.1)
    started=time.monotonic()
    assert u.PostMessageW(window,0x111,302,0)
    wait(lambda:not [r for r in c.resources()['resources'] if r['kind']=='cmd'])
    wait(lambda:u.SendMessageW(listing,0x18b,0,0)==0)
    wait(lambda:not children())
    wait(lambda:all(creation(p)!=created for p,created in known_processes.items()))
    duration=time.monotonic()-started
    report={'created_real_cmd_sessions':baseline+33,'child_processes_before':live,'children_after':children(),
            'disconnect_seconds':duration,'top_anchor':top,'selection_anchor':selection,
            'top_after_mouse_drag':0,'desktop_frames':images,
            'known_child_creation_times':known_processes,'known_children_all_exited':True,
            'checks':['stable refresh','native wheel','keyboard Home/End','physical overlay thumb drag','real GUI all-disconnect','job child termination'],
            'hardware':'no physical serial/SFTP/power; task-owned output covered separately by PowerService controlled tests'}
    pathlib.Path(sys.argv[4]).write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('PASS: %d real CMD sessions, list anchors/wheel/keys, all-disconnect child cleanup %.3fs'%(baseline+33,duration))
