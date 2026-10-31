"""Run actual original-identity released clients against an exact signed candidate.

Only an isolated GitHub-hosted Windows account is supported. The public JSON
and immutable ZIP are inputs. This program never receives a signing key.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
import uuid
import zipfile


def require(value,message):
    if not value:raise RuntimeError(message)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for arg in ('candidate','metadata','previous','older','output'):p.add_argument('--'+arg,type=Path,required=True)
    args=p.parse_args()
    require(os.name=='nt' and os.environ.get('RUNNER_ENVIRONMENT')=='github-hosted','Requires isolated hosted Windows')
    args.output.mkdir(parents=True,exist_ok=False)
    user=c.WinDLL('user32',use_last_error=True);shell=c.WinDLL('shell32')
    callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.EnumWindows.argtypes=(callback,w.LPARAM)
    user.GetWindowThreadProcessId.argtypes=(w.HWND,c.POINTER(w.DWORD))
    user.GetClassNameW.argtypes=(w.HWND,w.LPWSTR,c.c_int)
    user.GetWindowTextW.argtypes=(w.HWND,w.LPWSTR,c.c_int)
    user.IsWindowVisible.argtypes=(w.HWND,)
    user.GetDlgItem.argtypes,user.GetDlgItem.restype=(w.HWND,c.c_int),w.HWND
    user.GetWindowRect.argtypes=(w.HWND,c.c_void_p)
    user.PostMessageW.argtypes=(w.HWND,w.UINT,c.c_size_t,c.c_ssize_t)
    user.SetForegroundWindow.argtypes=(w.HWND,)
    def windows(cls=None):
        result=[]
        @callback
        def visit(h,_):
            name=c.create_unicode_buffer(128);user.GetClassNameW(h,name,128)
            if user.IsWindowVisible(h) and (cls is None or name.value==cls):result.append(h)
            return True
        user.EnumWindows(visit,0);return result
    require(not windows('SerialCtlWin7NativeWindowV3'),'An existing GUI instance is running')
    def wait(fn,seconds=60):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            value=fn()
            if value:return value
            time.sleep(.05)
        raise RuntimeError('Upgrade stage timed out')
    def click(h):
        require(h,'Missing update confirmation')
        rect=(c.c_long*4)();require(user.GetWindowRect(h,c.byref(rect)),'Cannot find update confirmation bounds')
        user.SetForegroundWindow(h);user.SetCursorPos((rect[0]+rect[2])//2,(rect[1]+rect[3])//2)
        user.mouse_event(2,0,0,0,0);time.sleep(.025);user.mouse_event(4,0,0,0,0)
    def shutdown_apps():
        for h in windows('SerialCtlWin7NativeWindowV3'):user.PostMessageW(h,0x10,0,0)
        wait(lambda:not windows('SerialCtlWin7NativeWindowV3'),20)
    shell.SHGetFolderPathW.argtypes=(w.HWND,c.c_int,w.HANDLE,w.DWORD,w.LPWSTR)
    buf=c.create_unicode_buffer(260);require(shell.SHGetFolderPathW(None,0x1A,None,0,buf)==0,'No actual profile directory')
    config=Path(buf.value)/'SerialCtl';backup=config.with_name('SerialCtl.upgrade-backup-'+uuid.uuid4().hex)
    moved=False;created=False;server=None;worker=None;process=None
    report={'status':'RUNNING','candidateSha256':hashlib.sha256(args.candidate.read_bytes()).hexdigest(),'cases':[],'scope':'Real released Windows executables; original production signature; loopback HTTP; actual confirmation, install and healthy restart; no physical devices or Win7 claim'}
    with zipfile.ZipFile(args.candidate) as z:
        expected_exe=hashlib.sha256(z.read('serialctl.exe')).hexdigest()
        expected_version=z.read('VERSION')
    metadata=json.loads(args.metadata.read_text());payload=json.loads(metadata['payload'])
    require(payload['archive']['sha256']==report['candidateSha256'],'JSON does not describe exact candidate ZIP')
    feed=args.output/'feed';feed.mkdir();shutil.copyfile(args.candidate,feed/args.candidate.name);shutil.copyfile(args.metadata,feed/'update.json')
    tampered=dict(metadata);tampered['signature']=('0' if metadata['signature'][0]!='0' else '1')+metadata['signature'][1:]
    (feed/'invalid.json').write_text(json.dumps(tampered),encoding='utf-8')
    requests=[]
    corrupt_archive=[False]
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self,*a,**k):super().__init__(*a,directory=str(feed),**k)
        def do_GET(self):
            requests.append(self.path)
            if corrupt_archive[0] and self.path=='/'+args.candidate.name:
                self.send_response(200);self.send_header('Content-Length',str(args.candidate.stat().st_size));self.end_headers()
                with args.candidate.open('rb') as source:
                    first=True
                    while True:
                        block=source.read(65536)
                        if not block:break
                        if first:block=bytes([block[0]^1])+block[1:];first=False
                        self.wfile.write(block)
                return
            super().do_GET()
        def log_message(self,*a):pass
    try:
        if config.exists():
            require(not config.is_symlink(),'Refuse linked profile')
            os.replace(config,backup);moved=True
        config.mkdir();created=True
        (config/'settings.ini').write_text('[Pet]\r\nEnabled=0\r\n[Layout]\r\nRightPanelCollapsed=1\r\n',encoding='ascii')
        (config/'command-notebook.txt').write_text('echo KEEP_UPGRADE_NOTEBOOK\r\n',encoding='utf-8')
        note=(config/'command-notebook.txt').read_bytes()
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler);worker=threading.Thread(target=server.serve_forever);worker.start()
        for label,archive in (('V2.0.15',args.previous),('V2.0.6',args.older)):
            for mode in ('invalid-signature','corrupt-archive','valid'):
                invalid=mode=='invalid-signature'
                corrupt_archive[0]=mode=='corrupt-archive' 
                install=args.output/(label+'-'+mode);install.mkdir()
                with zipfile.ZipFile(archive) as z:
                    for name in ('serialctl.exe','plink.exe','psftp.exe','VERSION'):(install/name).write_bytes(z.read(name))
                (install/'drivers').mkdir();(install/'drivers'/'keep.txt').write_text('DRIVER_PRESERVED',encoding='ascii')
                (install/'keep-user.txt').write_text('USER_PRESERVED',encoding='ascii')
                url=f'http://127.0.0.1:{server.server_port}/'+('invalid.json' if invalid else 'update.json')
                ini=('[update]\r\nurl='+url+'\r\n').encode('ascii');(install/'update.ini').write_bytes(ini)
                original=hashlib.sha256((install/'serialctl.exe').read_bytes()).hexdigest()
                start=len(requests);process=subprocess.Popen([str(install/'serialctl.exe')],cwd=install)
                wait(lambda:len(requests)>start,20)
                if invalid:
                    time.sleep(2)
                    require(hashlib.sha256((install/'serialctl.exe').read_bytes()).hexdigest()==original,'Invalid signature changed the installed executable')
                    require(args.candidate.name not in requests[start:],'Invalid signature downloaded a ZIP')
                    require(not (install/'.serialctl-update'/'transaction.txt').exists(),'Invalid signature started installation')
                    shutdown_apps();process.wait(timeout=20);process=None
                    report['cases'].append({'from':label,'invalidSignature':'PASS'});continue
                def confirmation():
                    for h in windows('#32770'):
                        title=c.create_unicode_buffer(256);user.GetWindowTextW(h,title,256)
                        if '更新' in title.value:
                            button=user.GetDlgItem(h,1)
                            if button:return button
                    return None
                button=wait(confirmation,30)
                require('/'+args.candidate.name not in requests[start:],'Archive downloaded before user confirmation')
                click(button)
                if corrupt_archive[0]:
                    def failed_dialog():
                        for h in windows('#32770'):
                            title=c.create_unicode_buffer(256);user.GetWindowTextW(h,title,256)
                            if title.value=='更新失败':return h
                        return None
                    failed=wait(failed_dialog,60)
                    require(hashlib.sha256((install/'serialctl.exe').read_bytes()).hexdigest()==original,'Damaged ZIP replaced the old client')
                    require(not (install/'.serialctl-update'/'transaction.txt').exists(),'Damaged ZIP began installation')
                    click(user.GetDlgItem(failed,2));process.wait(timeout=20);process=None
                    report['cases'].append({'from':label,'corruptArchive':'PASS','oldExecutablePreserved':'PASS'})
                    continue
                def installed():
                    try:return hashlib.sha256((install/'serialctl.exe').read_bytes()).hexdigest()==expected_exe and (install/'VERSION').read_bytes()==expected_version
                    except OSError:return False
                wait(installed,180)
                process.wait(timeout=30);process=None
                wait(lambda:windows('SerialCtlWin7NativeWindowV3'),30)
                def committed():
                    logs=list((install/'.serialctl-update').glob('*/update.log'))
                    return any('committed' in f.read_text(errors='replace').lower() or 'success' in f.read_text(errors='replace').lower() for f in logs)
                wait(committed,30)
                require((install/'update.ini').read_bytes()==ini,'Update URL changed')
                require((config/'command-notebook.txt').read_bytes()==note,'Notebook changed')
                require((install/'drivers'/'keep.txt').read_text()=='DRIVER_PRESERVED','Driver data changed')
                require((install/'keep-user.txt').read_text()=='USER_PRESERVED','Unapproved user file changed')
                shutdown_apps()
                report['cases'].append({'from':label,'signature':'PASS','installation':'PASS','healthyRestart':'PASS','configurationAndNotebook':'PASS','driverAndUserFiles':'PASS'})
        report['status']='PASS'
    except Exception as exc:
        report['status']='FAIL';report['error']=str(exc);raise
    finally:
        try:shutdown_apps()
        except Exception:pass
        if process and process.poll() is None:process.terminate();process.wait(timeout=10)
        if server:server.shutdown();server.server_close()
        if worker:worker.join(timeout=5)
        if created and config.exists():shutil.rmtree(config)
        if moved:os.replace(backup,config)
        report['requests']=requests
        (args.output/'upgrade-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(report))

if __name__=='__main__':main()
