"""Bounded actual-user endurance checks on the caller's isolated packaged GUI.

Uses real Windows mouse input and visible desktop frames. No command/button
notifications substitute for clicks. Loopback WebSocket submits only the test
CMD output producer; it never selects or operates physical devices.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import sys
import time
import traceback

from desktop_user_actions import DesktopActions, POINT, RECT, require
from gui_paint_user_actions import ScreenRecording


class GuiThreadInfo(c.Structure):
    _fields_ = [('size',w.DWORD),('flags',w.DWORD),('active',w.HWND),('focus',w.HWND),
                ('capture',w.HWND),('menuOwner',w.HWND),('moveSize',w.HWND),('caret',w.HWND),('caretRect',RECT)]


def wait(predicate, budget=5):
    deadline=time.perf_counter()+budget
    while not predicate():
        require(time.perf_counter()<deadline,'Timed out while observing actual native state')
        time.sleep(.005)


class Endurance(DesktopActions):
    def __init__(self,args):
        super().__init__(args)
        self.user.GetGUIThreadInfo.argtypes=(w.DWORD,c.POINTER(GuiThreadInfo))
        self.user.GetGUIThreadInfo.restype=w.BOOL
        self.user.GetGuiResources.argtypes=(w.HANDLE,w.DWORD)
        self.user.GetGuiResources.restype=w.DWORD
        self.user.FindWindowExW.argtypes=(w.HWND,w.HWND,w.LPCWSTR,w.LPCWSTR)
        self.user.FindWindowExW.restype=w.HWND
        self.kernel=c.WinDLL('kernel32',use_last_error=True)
        self.kernel.OpenProcess.argtypes=(w.DWORD,w.BOOL,w.DWORD)
        self.kernel.OpenProcess.restype=w.HANDLE
        self.kernel.GetProcessHandleCount.argtypes=(w.HANDLE,c.POINTER(w.DWORD))
        self.kernel.CloseHandle.argtypes=(w.HANDLE,)
        self.kernel.WaitForSingleObject.argtypes=(w.HANDLE,w.DWORD)
        self.kernel.GetExitCodeProcess.argtypes=(w.HANDLE,c.POINTER(w.DWORD))
        self.process=self.kernel.OpenProcess(0x00100400,False,args.pid)
        require(self.process,'Cannot observe native process resources')
        self.responses=[]
        self.resources=[]
        self.dialogs=[]
        self.recordings=[]
        self.clicks=0
        self.report['scope']='Actual packaged GUI, real mouse clicks and nonclient/splitter drags; controlled CMD stdout; no physical devices'
        self.report['limits']=['Actual CI system DPI only','Finite bounded stress, no eight-hour soak claim',
                               'Finite screen samples do not prove absence of every inter-frame artifact']

    def sample_resources(self,label):
        count=w.DWORD()
        require(self.kernel.GetProcessHandleCount(self.process,c.byref(count)),'Cannot read process handles')
        row={**self.timestamp(),'label':label,'handles':count.value,
             'gdi':self.user.GetGuiResources(self.process,0),'user':self.user.GetGuiResources(self.process,1)}
        require(row['gdi']>0 and row['user']>0,'GUI resource counters unavailable')
        self.resources.append(row)
        return row

    def hit_click(self,h,point=None):
        require(h and self.user.IsWindow(h) and self.user.IsWindowVisible(h),'Click target is unavailable')
        owner=w.DWORD();self.user.GetWindowThreadProcessId(h,c.byref(owner))
        require(owner.value==self.args.pid,'Click target left the tested process')
        r=self.rect(h)
        point=point or ((r[0]+r[2])//2,(r[1]+r[3])//2)
        self.move_mouse(point)
        require(self.user.WindowFromPoint(POINT(*point))==h,'Real click target is obscured')
        self.button(True);time.sleep(.012);self.button(False)
        self.clicks+=1

    def modal_windows(self):
        found=[]
        callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
        def observe(h,_):
            pid=w.DWORD();self.user.GetWindowThreadProcessId(h,c.byref(pid))
            if pid.value==self.args.pid and self.user.IsWindowVisible(h):
                name=c.create_unicode_buffer(64);self.user.GetClassNameW(h,name,len(name))
                if name.value=='#32770':found.append(h)
            return True
        self.user.EnumWindows.argtypes=(callback,w.LPARAM)
        require(self.user.EnumWindows(callback(observe),0),'Cannot enumerate real modal windows')
        return found

    def open_cancel(self,identifier,background_burst=False):
        self.hit_click(self.control(identifier))
        wait(lambda:len(self.modal_windows())==1)
        dialog=self.modal_windows()[0]
        wait(lambda:int(self.user.GetPropW(dialog,'SerialCtl.DialogFirstPaint') or 0)>0 and self.user.IsWindowEnabled(self.user.GetDlgItem(dialog,2)))
        require(self.user.GetForegroundWindow()==dialog,'The opened modal did not receive foreground')
        if background_burst:
            r=self.rect(self.control(identifier));point=((r[0]+r[2])//2,(r[1]+r[3])//2)
            dr=self.rect(dialog)
            require(not(dr[0]<=point[0]<dr[2] and dr[1]<=point[1]<dr[3]),'Background burst point entered the modal')
            for _ in range(6):
                self.move_mouse(point);self.button(True);time.sleep(.008);self.button(False);self.clicks+=1
            time.sleep(.03)
            require(self.modal_windows()==[dialog],'Rapid background clicks created another modal or closed this one')
        if identifier==103:time.sleep(.65)  # Exercise discovery scheduling before closing.
        self.hit_click(self.user.GetDlgItem(dialog,2))
        wait(lambda:not self.user.IsWindow(dialog))
        wait(lambda:self.user.GetForegroundWindow()==self.window)
        ms=self.responsive();self.responses.append(ms)
        self.dialogs.append({'control':identifier,'response_ms':ms,'background_burst':background_burst})

    def park_visible_pet(self):
        pet=self.user.FindWindowExW(None,None,'SerialCtlAiPetPane',None)
        if not pet or not self.user.IsWindowVisible(pet):return
        pid=w.DWORD();thread=self.user.GetWindowThreadProcessId(pet,c.byref(pid))
        require(pid.value==self.args.pid,'Pet belongs to a different instance')
        before=self.rect(pet);main=self.rect(self.window);screen=self.desktop_rect()
        width,height=before[2]-before[0],before[3]-before[1]
        left=main[2]+12
        require(left+width+12<=screen[2],'No clear desktop area for enabled pet fixture')
        start=None
        for fy in (.75,.65,.85,.55):
            for fx in (.5,.4,.6):
                point=(before[0]+round(width*fx),before[1]+round(height*fy))
                if self.user.WindowFromPoint(POINT(*point))==pet:start=point;break
            if start:break
        require(start,'Cannot hit the visible pet body')
        def capture():
            info=GuiThreadInfo();info.size=c.sizeof(info)
            require(self.user.GetGUIThreadInfo(thread,c.byref(info)),'Cannot observe native capture')
            return info.capture
        self.capture('pet-before-real-drag');self.move_mouse(start);time.sleep(.08);self.button(True)
        try:
            wait(lambda:capture()==pet,2)
            for i in range(1,9):
                self.move_mouse((start[0]+round((left-before[0])*i/8),start[1]));time.sleep(.02)
            wait(lambda:abs(self.rect(pet)[0]-left)<=3,2)
        finally:self.button(False)
        wait(lambda:capture()!=pet,2)
        require(self.user.IsWindowVisible(pet) and self.rect(pet)[0]>=main[2],'Pet fixture hid the pet or still covers main')
        self.capture('pet-after-real-drag');self.flush_frames()
        self.report['pet_fixture']={'before':before,'after':self.rect(pet),'input':'one real drag after observed native capture',
                                    'enabled_and_visible':True,'position_only':'Test-account fixture; topmost/roaming preferences unchanged'}
        self.move_mouse(((main[0]+main[2])//2,main[1]+60))

    def run_stress(self):
        require(self.user.GetForegroundWindow()==self.window,'Main window must be foreground')
        self.hit_click(self.control(304))
        wait(lambda:len(self.modal_windows())==1)
        initial=self.modal_windows()[0]
        wait(lambda:int(self.user.GetPropW(initial,'SerialCtl.DialogFirstPaint') or 0)>0)
        self.hit_click(self.user.GetDlgItem(initial,2));wait(lambda:not self.modal_windows())
        time.sleep(5)
        self.park_visible_pet()
        self.hit_click(self.control(300))
        wait(lambda:self.user.IsWindowVisible(self.control(107)))
        sys.path.insert(0,self.args.native_dir)
        from serialctl_ws import Client
        with Client('127.0.0.1') as client:
            resources=client.resources()['resources']
            cmd=next(r['id'] for r in resources if r['kind']=='cmd')
            producer=b'powershell -NoProfile -Command "1..60000 | ForEach-Object {Write-Output (\'SERIALCTL_PAINT_\'+($_ % 1000).ToString(\'D3\')+\' endurance stdout\'); Start-Sleep -Milliseconds 20}"\r\n'
            client.input(cmd,producer)
        wait(lambda:int(self.user.GetPropW(self.control(107),'SerialCtl.PaintProbe') or 0)>0,10)
        self.prepare_full_window_drag()
        import imageio_ffmpeg
        ffmpeg=imageio_ffmpeg.get_ffmpeg_exe()
        try:
            for theme in ('light','dark'):
                self.args.theme=theme;self.args.prefix='endurance-'+theme
                desired='深色' if theme=='light' else '浅色'
                if self.caption(self.control(104))!=desired:
                    self.hit_click(self.control(104));wait(lambda:self.caption(self.control(104))==desired)
                self.capture('theme-initial');self.flush_frames()
                for cycle in range(24):
                    identifier=(100,101,102,103,303)[cycle%5]
                    self.open_cancel(identifier,background_burst=cycle<5)
                    if cycle%4==0:self.sample_resources(theme+'-dialogs-'+str(cycle))
                # AI entry/cancel is a distinct modal/page transition. Close its
                # pseudo-session through the existing real Disconnect action.
                for cycle in range(6):
                    self.open_cancel(304)
                    self.hit_click(self.control(105))
                baseline=self.sample_resources(theme+'-warm')
                recorder=ScreenRecording(self,self.desktop_rect(),ffmpeg,8)
                recorder.start()
                try:
                    for cycle in range(8):
                        recorder.phase=theme+'-drag-'+str(cycle)
                        self.move_window_to(20 if cycle%2==0 else 0,20 if cycle%2==0 else 0,'caption-'+str(cycle))
                        self.resize(1040 if cycle%2==0 else 1180,680 if cycle%2==0 else 760,'resize-'+str(cycle))
                        self.responses.append(self.responsive())
                        self.sample_resources(theme+'-drag-'+str(cycle))
                    recorder.phase=theme+'-sidebar'
                    self.sidebar_drag()
                    self.sidebar_toggle()
                    self.maximize_restore()
                    recording=recorder.stop()
                    markers={row.get('terminal_stdout_marker') for row in recorder.frames}
                    recording['distinct_stdout_markers']=len(markers)
                    require(len(markers)>2,'Continuous real CMD output did not advance during drag recording')
                    self.recordings.append(recording);recorder=None
                finally:
                    if recorder:self.recordings.append(recorder.stop())
                final=self.sample_resources(theme+'-after')
                # Bounded caches may warm, but a short fixed set of operations
                # must not grow native resources without a practical bound.
                require(final['gdi']-baseline['gdi']<=256,'GDI resources grew beyond the declared fixed-cache allowance')
                require(final['user']-baseline['user']<=32,'Native USER windows leaked through repeated interactions')
                require(final['handles']-baseline['handles']<=64,'Process handles leaked through repeated interactions')
                self.capture('theme-final');self.flush_frames()
            require(max(self.responses)<500,'UI response probe exceeded 500ms during bounded endurance')
            self.hit_click(self.control(302))
            time.sleep(.3)
            with Client('127.0.0.1') as client:
                require(not[r for r in client.resources()['resources'] if r['kind']=='cmd'],'All-disconnect left CMD resources')
            self.capture('all-disconnected');self.flush_frames()
            bounds=self.rect(self.window)
            close=next(((x,y) for x in range(bounds[2]-40,bounds[2]-5,3)
                        for y in range(bounds[1]+5,bounds[1]+28,3) if self.hit_test(x,y)==20),None)
            require(close,'Cannot locate the actual native Close caption target')
            self.hit_click(self.window,close)
            started=time.perf_counter();require(self.kernel.WaitForSingleObject(self.process,10000)==0,'Native close failed to finish')
            exit_code=w.DWORD();require(self.kernel.GetExitCodeProcess(self.process,c.byref(exit_code)),'No process exit code')
            require(exit_code.value==0,'Application exited abnormally')
            self.report.update({'status':'PASS','actual_click_count':self.clicks,'actual_mouse_releases':sum(x.get('event')=='left_up' for x in self.trace),'modal_cycles':len(self.dialogs),
                                'response_samples_ms':self.responses,'resource_samples':self.resources,
                                'recordings':self.recordings,'dialog_operations':self.dialogs,
                                'close_seconds':round(time.perf_counter()-started,3),'exit_code':exit_code.value})
        finally:
            self.restore_full_window_drag()
            self.kernel.CloseHandle(self.process)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--window',type=int,required=True);parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--native-dir',required=True)
    parser.add_argument('--prefix',default='endurance');parser.add_argument('--mode',default='desktop')
    args=parser.parse_args();actions=Endurance(args)
    try:actions.run_stress()
    except Exception as error:
        actions.report.update({'status':'FAIL','error':repr(error),'traceback':traceback.format_exc(),
                               'actual_click_count':actions.clicks,'resource_samples':actions.resources,
                               'response_samples_ms':actions.responses,'dialog_operations':actions.dialogs})
        print(traceback.format_exc(),file=sys.stderr)
        try:actions.capture('failure-desktop');actions.flush_frames()
        except Exception:pass
    finally:
        actions.save()
        (args.output/'endurance-result.json').write_text(json.dumps(actions.report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:actions.report.get(k) for k in ('status','actual_click_count','modal_cycles','exit_code','error')}))
    return 0 if actions.report['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
