"""Prepare the original lifecycle fixture using the exact published candidate."""
import ctypes as c
from ctypes import wintypes as w
import pathlib,subprocess,sys,time
native,pid,output=pathlib.Path(sys.argv[1]),int(sys.argv[2]),sys.argv[3]
u=c.WinDLL('user32')
cb=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
u.EnumWindows.argtypes=(cb,w.LPARAM)
u.GetWindowThreadProcessId.argtypes=(w.HWND,c.POINTER(w.DWORD))
u.GetClassNameW.argtypes=(w.HWND,w.LPWSTR,c.c_int)
u.PostMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM)
u.GetDlgItem.argtypes,u.GetDlgItem.restype=(w.HWND,c.c_int),w.HWND
def windows():
 found=[]
 @cb
 def each(hwnd,unused):
  owner=w.DWORD();u.GetWindowThreadProcessId(hwnd,c.byref(owner))
  if owner.value==pid:
   name=c.create_unicode_buffer(128);u.GetClassNameW(hwnd,name,len(name));found.append((hwnd,name.value))
  return True
 u.EnumWindows(each,0);return found
deadline=time.monotonic()+10
while True:
 main=next((h for h,n in windows() if n=='SerialCtlWin7NativeWindowV3'),None)
 if main:break
 assert time.monotonic()<deadline,'Exact main window did not appear'
 time.sleep(.05)
# Same established pane/CMD fixture as the full desktop audit. These setup
# messages are excluded from measured user inputs in gui_user_lifecycle.py.
assert u.PostMessageW(main,0x111,301,0)
deadline=time.monotonic()+10
while True:
 dialog=next((h for h,n in windows() if n=='#32770' and u.GetDlgItem(h,1)),None)
 if dialog:
  assert u.PostMessageW(u.GetDlgItem(dialog,1),0xf5,0,0);break
 assert time.monotonic()<deadline,'Expected unavailable-power notice did not appear'
 time.sleep(.05)
time.sleep(.2)
assert u.PostMessageW(main,0x111,300,0)
time.sleep(.5)
subprocess.run([sys.executable,str(pathlib.Path(__file__).with_name('gui_user_lifecycle.py')),str(native),str(main),str(pid),output],check=True)
