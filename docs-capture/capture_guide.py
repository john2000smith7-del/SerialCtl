"""Capture the unchanged released executable with clearly identified demo transports."""
from pathlib import Path
import argparse,base64,ctypes as c,hashlib,json,os,re,shutil,socket,socketserver,struct,subprocess,threading,time,zipfile
from ctypes import wintypes as w
from PIL import ImageGrab
BOOT=b'''U-Boot 2025.01\r\nCPU: ARM Cortex-A53\r\nDRAM: 2 GiB\r\nLoading kernel and device tree...\r\nStarting kernel ...\r\n[    0.000000] Linux version 6.1.0-demo (guide@build)\r\n[    0.000000] Kernel command line: console=ttyS0,115200 root=/dev/mmcblk0p2 rootfstype=ext4 rootwait earlycon\r\n[    1.025000] mmc0: new high speed SDHC card\r\n[    1.170000] mmcblk0: p1 p2\r\n[    2.000000] Waiting for root device /dev/mmcblk0p2...\r\n'''
class DemoSerial(socketserver.BaseRequestHandler):
 def readn(self,n):
  b=b''
  while len(b)<n:
   p=self.request.recv(n-len(b))
   if not p:raise EOFError
   b+=p
  return b
 def send(self,j):
  b=json.dumps(j).encode();head=bytes([0x81,len(b)]) if len(b)<126 else b'\x81\x7e'+struct.pack('>H',len(b));self.request.sendall(head+b)
 def handle(self):
  try:
   data=b''
   while b'\r\n\r\n' not in data:
    chunk=self.request.recv(4096)
    if not chunk:return
    data+=chunk
   key=re.search(rb'Sec-WebSocket-Key:\s*([^\r\n]+)',data,re.I)[1].decode()
   accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
   self.request.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: serialctl.v1\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n').encode())
   instance='20502050205020502050205020502050';self.send({'type':'hello','version':1,'instance':instance,'maxInput':65536,'maxClients':128})
   seq=0
   while True:
    a,b=self.readn(2);n=b&127
    if n==126:n=struct.unpack('>H',self.readn(2))[0]
    elif n==127:n=struct.unpack('>Q',self.readn(8))[0]
    mask=self.readn(4) if b&128 else b'';raw=self.readn(n)
    if mask:raw=bytes(v^mask[i%4] for i,v in enumerate(raw))
    if a&15==8:return
    if a&15!=1:continue
    m=json.loads(raw);op=m['op'];r={}
    if op=='resources':r={'resources':[{'id':'board-demo','kind':'serial','com':'COM3','friendlyName':'USB-SERIAL CH340 (COM3)','connected':True,'baudRate':115200}]}
    elif op=='subscribe':r={'subscribed':True,'cursor':0}
    elif op=='input':r={'accepted':len(base64.b64decode(m['params']['data'])),'queued':True,'execution':'unknown'}
    elif op=='session.get':r={'connected':True,'codePage':65001,'inputCodePage':65001,'cursor':seq}
    elif op=='events':r={'events':[],'cursor':seq}
    self.send({'type':'response','version':1,'instance':instance,'requestId':m['requestId'],'ok':True,'result':r})
    if op in ('subscribe','input'):
     seq+=1;out=BOOT if op=='subscribe' else b'root@board:~# uname -a\r\nLinux board 6.1.0-demo #1 SMP aarch64 GNU/Linux\r\nroot@board:~# '
     self.send({'type':'event','version':1,'instance':instance,'resource':'board-demo','seq':seq,'kind':'output','source':'device','codePage':65001,'tick':1,'data':base64.b64encode(out).decode()})
  except (OSError,EOFError,ValueError):pass
class Server(socketserver.ThreadingTCPServer):allow_reuse_address=True;daemon_threads=True

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--zip',type=Path,required=True);ap.add_argument('--plink',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);args=ap.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True);app=out/'runtime';app.mkdir()
 with zipfile.ZipFile(args.zip) as z:
  for m in z.infolist():
   if '/' not in m.filename and not m.is_dir():(app/m.filename).write_bytes(z.read(m))
 assert hashlib.sha256((app/'serialctl.exe').read_bytes()).hexdigest()=='e4092b19324b2abc1b62a6b35f2ace653a7ab3764c970874859e5ca9218fb031'
 shutil.copy2(args.plink,app/'plink.exe');(app/'update.ini').write_text('[update]\nurl=\n')
 server=Server(('127.0.0.1',7000),DemoSerial);threading.Thread(target=server.serve_forever,daemon=True).start()
 user=c.WinDLL('user32');user.SetProcessDPIAware();CB=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
 user.EnumWindows.argtypes=(CB,w.LPARAM);user.EnumChildWindows.argtypes=(w.HWND,CB,w.LPARAM)
 user.GetDlgItem.argtypes=(w.HWND,c.c_int);user.GetDlgItem.restype=w.HWND
 user.GetWindowRect.argtypes=(w.HWND,c.POINTER(w.RECT));user.GetWindowTextW.argtypes=(w.HWND,w.LPWSTR,c.c_int);user.GetClassNameW.argtypes=(w.HWND,w.LPWSTR,c.c_int);user.GetWindowThreadProcessId.argtypes=(w.HWND,c.POINTER(w.DWORD))
 user.SendMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM);user.SendMessageW.restype=w.LPARAM;user.PostMessageW.argtypes=(w.HWND,w.UINT,w.WPARAM,w.LPARAM);user.SetForegroundWindow.argtypes=(w.HWND,);user.MoveWindow.argtypes=(w.HWND,c.c_int,c.c_int,c.c_int,c.c_int,w.BOOL)
 dm=c.create_string_buffer(220);struct.pack_into('<H',dm,68,220);struct.pack_into('<I',dm,72,0x1c0000);struct.pack_into('<III',dm,168,32,1920,1080);user.ChangeDisplaySettingsW(dm,0)
 env=os.environ.copy();profile=out/'profile';profile.mkdir();env['LOCALAPPDATA']=str(profile);env['APPDATA']=str(profile)
 process=subprocess.Popen([str(app/'serialctl.exe')],cwd=app,env=env)
 def caption(h):
  b=c.create_unicode_buffer(2048);user.GetWindowTextW(h,b,len(b));return b.value
 def cls(h):
  b=c.create_unicode_buffer(128);user.GetClassNameW(h,b,len(b));return b.value
 def windows():
  found=[]
  @CB
  def each(h,l):
   p=w.DWORD();user.GetWindowThreadProcessId(h,c.byref(p))
   if p.value==process.pid and user.IsWindowVisible(h):found.append(h)
   return True
  user.EnumWindows(each,0);return found
 def children(h):
  found=[]
  @CB
  def each(x,l):found.append(x);return True
  user.EnumChildWindows(h,each,0);return found
 def wait(f,label,seconds=20):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   value=f()
   if value:return value
   time.sleep(.08)
  raise RuntimeError('Timed out: '+label+' '+repr([(caption(h),cls(h)) for h in windows()]))
 def dialog(word):return next((h for h in windows() if cls(h)=='#32770' and word in caption(h)),0)
 def child(h,i):return user.GetDlgItem(h,i)
 def click(h):
  assert h
  user.SetForegroundWindow(h);time.sleep(.06)
  r=rect(h);user.SetCursorPos((r[0]+r[2])//2,(r[1]+r[3])//2)
  user.mouse_event(2,0,0,0,0);time.sleep(.07);user.mouse_event(4,0,0,0,0);time.sleep(.2)
 def fill(h,text):assert h;user.SendMessageW(h,0xc,0,c.cast(c.c_wchar_p(text),c.c_void_p).value)
 def rect(h):r=w.RECT();user.GetWindowRect(h,c.byref(r));return [r.left,r.top,r.right,r.bottom]
 def shot(h,name):
  user.SetForegroundWindow(h);time.sleep(.35);r=rect(h);ImageGrab.grab(bbox=tuple(r),all_screens=True).save(out/(name+'.png'))
  controls=[]
  for x in children(h):
   q=rect(x);controls.append({'id':user.GetDlgCtrlID(x),'class':cls(x),'text':'[hidden]' if user.GetDlgCtrlID(x) in (203,1020) else caption(x),'visible':bool(user.IsWindowVisible(x)),'rect':[q[0]-r[0],q[1]-r[1],q[2]-r[0],q[3]-r[1]]})
  (out/(name+'.json')).write_text(json.dumps({'window':r,'controls':controls},ensure_ascii=False,indent=2),encoding='utf-8');print('CAPTURED '+name,flush=True)
 try:
  main_window=wait(lambda:next((h for h in windows() if cls(h)=='SerialCtlWin7NativeWindowV3'),0),'main window');user.MoveWindow(main_window,40,40,1320,850,True);time.sleep(.4)
  click(child(main_window,300));time.sleep(.4)
  terminal=child(main_window,107)
  for char in 'ver':user.PostMessageW(terminal,0x102,ord(char),0)
  user.PostMessageW(terminal,0x102,13,0);time.sleep(.5);shot(main_window,'guide-cmd')
  click(child(main_window,103));remote=wait(lambda:dialog('共享串口'),'remote dialog');fill(child(remote,1014),'127.0.0.1');user.PostMessageW(remote,0x111,9010,0);port=wait(lambda:dialog('高级共享端口'),'advanced port');fill(child(port,1402),'7000');click(child(port,1));wait(lambda:caption(child(remote,1002)).startswith('COM3'),'COM3 discovery');shot(remote,'guide-remote-ready');click(child(remote,1));time.sleep(.7);shot(main_window,'guide-terminal')
  click(child(main_window,303));settings=wait(lambda:dialog('设置'),'settings');shot(settings,'guide-settings');click(child(settings,2))
  click(child(main_window,304));connect=wait(lambda:dialog('连接 AI 服务器'),'AI connection');fill(child(connect,200),'ai-demo.invalid');fill(child(connect,201),'22');fill(child(connect,202),'developer');fill(child(connect,203),'demo-only-password');fill(child(connect,208),'/srv/projects/board');shot(connect,'guide-ai-connect-filled');click(child(connect,206))
  trust=wait(lambda:next((h for h in windows() if cls(h)=='#32770' and 'SSH' in caption(h)),0),'demo host verification');shot(trust,'guide-ai-host-key');click(child(trust,6));wait(lambda:caption(child(connect,1))=='进入对话','AI ready');shot(connect,'guide-ai-connected');click(child(connect,1));time.sleep(.4)
  pane=wait(lambda:next((h for h in children(main_window) if cls(h)=='SerialCtlAiPane'),0),'AI pane');combo=child(pane,14)
  count=user.SendMessageW(combo,0x146,0,0);labels=[]
  for i in range(count):
   b=c.create_unicode_buffer(1024);user.SendMessageW(combo,0x148,i,c.cast(b,c.c_void_p).value);labels.append(b.value)
  index=next(i for i,label in enumerate(labels) if 'COM3' in label);user.SendMessageW(combo,0x14e,index,0);user.PostMessageW(pane,0x111,14|(1<<16),combo);time.sleep(.4);shot(main_window,'guide-ai-ready')
  fill(child(pane,10),'帮我分析当前开发板的启动日志，看看为什么停在启动阶段。');shot(main_window,'guide-ai-compose');click(child(pane,11));time.sleep(1.3);shot(main_window,'guide-ai-running');wait(lambda:user.IsWindowEnabled(child(pane,11)),'AI first answer',25);shot(main_window,'guide-ai-completed')
  transcript=next(h for h in children(pane) if cls(h)=='SerialCtlAiTranscript');r=rect(transcript);user.SetCursorPos(r[2]-65,r[1]+205);user.mouse_event(2,0,0,0,0);user.mouse_event(4,0,0,0,0);time.sleep(.2);shot(main_window,'guide-ai-process-open')
  click(child(pane,17));history=wait(lambda:dialog('历史会话'),'history');shot(history,'guide-ai-history');click(child(history,2));
  fill(child(pane,10),'请在关联终端执行 uname -a，并读取新的输出。');click(child(pane,11));time.sleep(1.0);shot(main_window,'guide-ai-command-running');wait(lambda:user.IsWindowEnabled(child(pane,11)),'AI command result',25);shot(main_window,'guide-ai-command-completed')
  click(child(pane,19));connect=wait(lambda:dialog('连接 AI 服务器'),'AI settings');shot(connect,'guide-ai-project-settings');click(child(connect,2))
  click(child(main_window,104));time.sleep(.5);shot(main_window,'guide-ai-dark')
  (out/'capture-receipt.json').write_text(json.dumps({'program':'SerialCtl V2.0.5','exe_sha256':'e4092b19324b2abc1b62a6b35f2ace653a7ab3764c970874859e5ca9218fb031','source':'e27b6b9ecffb6b9877c80233911c40000fd139a1','capture':'Real Windows desktop ImageGrab; unchanged released serialctl.exe','demo':'Documentation-only loopback serial gateway and offline Codex process; sample device/logs and AI text are demonstration data, no real SSH or physical device','status':'PASS'},indent=2))
 except Exception:
  for i,h in enumerate(windows()):
   try:shot(h,'capture-failure-'+str(i))
   except Exception:pass
  raise
 finally:
  for h in windows():user.PostMessageW(h,0x10,0,0)
  try:process.wait(timeout=8)
  except subprocess.TimeoutExpired:process.kill()
  server.shutdown();server.server_close()
if __name__=='__main__':main()
