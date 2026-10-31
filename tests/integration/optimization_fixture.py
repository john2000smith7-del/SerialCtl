"""V2.0.16 actual packaged Windows controls; no device/server claims."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from types import SimpleNamespace
import uuid
import zipfile
from desktop_user_actions import DesktopActions, RECT, POINT, require
from gui_paint_user_actions import ScreenRecording
import imageio_ffmpeg


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    require(os.name=='nt' and os.environ.get('RUNNER_ENVIRONMENT')=='github-hosted','Requires isolated hosted Windows')
    args.output.mkdir(parents=True,exist_ok=False)
    user=c.WinDLL('user32',use_last_error=True)
    shell=c.WinDLL('shell32')
    shell.SHGetFolderPathW.argtypes=(w.HWND,c.c_int,w.HANDLE,w.DWORD,w.LPWSTR)
    root=c.create_unicode_buffer(260)
    require(shell.SHGetFolderPathW(None,0x1A,None,0,root)==0,'Cannot resolve actual profile')
    config=Path(root.value)/'SerialCtl'
    backup=config.with_name('SerialCtl.optimization-backup-'+uuid.uuid4().hex)
    callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.EnumWindows.argtypes=(callback,w.LPARAM)
    user.GetWindowThreadProcessId.argtypes=(w.HWND,c.POINTER(w.DWORD))
    user.GetClassNameW.argtypes=(w.HWND,w.LPWSTR,c.c_int)
    user.GetWindowTextW.argtypes=(w.HWND,w.LPWSTR,c.c_int)
    user.GetDlgItem.argtypes,user.GetDlgItem.restype=(w.HWND,c.c_int),w.HWND
    user.SendMessageW.argtypes,user.SendMessageW.restype=(w.HWND,w.UINT,c.c_size_t,c.c_ssize_t),c.c_ssize_t
    user.SetWindowTextW.argtypes=(w.HWND,w.LPCWSTR)
    user.PostMessageW.argtypes=(w.HWND,w.UINT,c.c_size_t,c.c_ssize_t)
    user.IsWindowVisible.argtypes=user.IsWindowEnabled.argtypes=(w.HWND,)
    user.MoveWindow.argtypes=(w.HWND,c.c_int,c.c_int,c.c_int,c.c_int,w.BOOL)
    user.GetWindowRect.argtypes=(w.HWND,c.POINTER(RECT))
    user.ClientToScreen.argtypes=(w.HWND,c.POINTER(POINT))
    def windows(pid=None,cls='SerialCtlWin7NativeWindowV3'):
        result=[]
        @callback
        def visit(h,_):
            owner=w.DWORD();user.GetWindowThreadProcessId(h,c.byref(owner))
            name=c.create_unicode_buffer(128);user.GetClassNameW(h,name,128)
            if (pid is None or owner.value==pid) and name.value==cls and user.IsWindowVisible(h):result.append(h)
            return True
        user.EnumWindows(visit,0)
        return result
    require(not windows(),'Existing GUI; refusing profile changes')
    def wait(fn):
        until=time.monotonic()+15
        while time.monotonic()<until:
            value=fn()
            if value:return value
            time.sleep(.02)
        raise RuntimeError('Windows control did not reach expected state')
    process=None; moved=False; created=False; report={'status':'RUNNING','themes':[],'hardware':'NOT RUN','scope':'Actual packaged GUI, actual mouse, measured system DPI, continuous desktop scroll frames'}
    try:
        app=args.output/'app';app.mkdir()
        with zipfile.ZipFile(args.archive) as z:
            for name in ('serialctl.exe','plink.exe','psftp.exe'): (app/name).write_bytes(z.read(name))
        (app/'update.ini').write_text('[update]\r\nurl=\r\n',encoding='ascii')
        if config.exists():
            require(not config.is_symlink(),'Linked configuration is unsupported')
            os.replace(config,backup);moved=True
        config.mkdir();created=True
        (config/'settings.ini').write_text('[Pet]\r\nEnabled=0\r\n[Layout]\r\nRightPanelCollapsed=0\r\n',encoding='ascii')
        process=subprocess.Popen([str(app/'serialctl.exe')],cwd=app)
        hwnd=wait(lambda: windows(process.pid))[0]
        user.MoveWindow(hwnd,0,0,1044,760,True)
        desktop=DesktopActions(SimpleNamespace(window=hwnd,pid=process.pid,output=str(args.output),mode='desktop',prefix='v216'))
        def click(h):
            box=desktop.rect(h);require(user.IsWindowVisible(h) and user.IsWindowEnabled(h),'Inactive test control')
            desktop.click(((box[0]+box[2])//2,(box[1]+box[3])//2));time.sleep(.08)
        def control(parent,i):
            h=user.GetDlgItem(parent,i);require(h,'Missing control '+str(i));return h
        def dialog():
            # Owned password prompts disable their still-visible settings owner.
            # Select the interactive modal, independent of EnumWindows Z order.
            return wait(lambda: [h for h in windows(process.pid,'#32770') if user.IsWindowEnabled(h)])[-1]
        def close(h):
            user.PostMessageW(h,0x10,0,0);wait(lambda:user.IsWindowEnabled(hwnd))
        def text(h):
            value=c.create_unicode_buffer(256);user.GetWindowTextW(h,value,256);return value.value
        for theme in ('light','dark'):
            if theme=='dark':desktop.click_control(104);time.sleep(.15)
            # Left lane remains bounded and changes width through actual capture/drag.
            before=desktop.rect(control(hwnd,106))
            point=POINT(196,230);user.ClientToScreen(hwnd,c.byref(point))
            desktop.drag(theme+'-left-width',(point.x,point.y),(point.x+55,point.y),steps=8)
            after=desktop.rect(control(hwnd,106))
            require(after[2]-after[0]>before[2]-before[0],'Left sidebar drag did not resize')
            desktop.drag(theme+'-left-restore',(point.x+55,point.y),(point.x,point.y),steps=8)
            desktop.click_control(100);ssh=dialog()
            require(control(ssh,1028) and control(ssh,1029) and control(ssh,1030),'SSH history/platform controls absent')
            desktop.capture(theme+'-ssh-history');close(ssh)
            desktop.click_control(303);settings=dialog();gate=control(settings,2507)
            require(user.SendMessageW(gate,0xF0,0,0)==0,'Test must default to off')
            click(gate);password=dialog();user.SetWindowTextW(control(password,10),'JJ123123');click(control(password,1))
            wait(lambda:'密码错误' in text(control(settings,2508)))
            require(user.SendMessageW(gate,0xF0,0,0)==0,'Uppercase password unlocked Test')
            click(gate);password=dialog();user.SetWindowTextW(control(password,10),'jj123123');desktop.capture(theme+'-test-password');click(control(password,1))
            wait(lambda:user.IsWindowEnabled(settings))
            desktop.capture(theme+'-test-result');desktop.flush_frames()
            note=text(control(settings,2508))
            require('Test 已开启' in note and user.SendMessageW(gate,0xF0,0,0)==1,'Correct password did not unlock Test: '+note)
            desktop.capture(theme+'-test-unlocked');click(gate)
            require(user.SendMessageW(gate,0xF0,0,0)==0,'Test did not lock immediately')
            desktop.capture(theme+'-test-locked');close(settings)
            desktop.click_control(301);time.sleep(.2)
            user.MoveWindow(hwnd,0,0,980,680,True);time.sleep(.15)
            # An independent sampler records every screen frame while the input
            # driver captures named checkpoints, including PNG encoding pauses.
            powers=[]
            @callback
            def find_power(child,_):
                name=c.create_unicode_buffer(128);user.GetClassNameW(child,name,128)
                if name.value=='SerialCtlPowerPane':powers.append(child)
                return True
            desktop.user.EnumChildWindows(hwnd,find_power,0)
            require(powers,'Power pane is missing for continuous capture')
            desktop.args.theme=theme;desktop.args.prefix='v216-'+theme+'-power'
            recording=ScreenRecording(desktop,desktop.rect(powers[0]),imageio_ffmpeg.get_ffmpeg_exe(),30)
            recording.start()
            # Capture both directions during native wheel input, not only after release.
            center=POINT(430,350);user.ClientToScreen(hwnd,c.byref(center));desktop.move_mouse((center.x,center.y))
            try:
                for i in range(24):
                    recording.phase='scroll-down' if i<12 else 'scroll-up'
                    user.mouse_event(0x800,0,0,-120 if i<12 else 120,0)
                    time.sleep(.08);desktop.capture(theme+'-power-scroll-'+str(i).zfill(2))
                    if i%6==5:desktop.flush_frames()
                recording.phase='settled-final';time.sleep(.2)
            finally:
                video=recording.stop()
            recording.ensure_alive()
            require(video['encoded_frames']>=30 and video['encoder_queue_drops']==0,'Continuous power evidence is incomplete')
            report.setdefault('powerRecordings',[]).append(video)
            desktop.click_control(105);time.sleep(.1)
            desktop.click_control(304);time.sleep(.15)
            # AI connection form may open on first entry; close it without any network request.
            modals=windows(process.pid,'#32770')
            if modals:desktop.capture(theme+'-ai-connection');close(modals[-1])
            wait(lambda:user.IsWindowVisible(control(hwnd,105)))
            desktop.capture(theme+'-ai-header')
            desktop.click_control(105);time.sleep(.1)
            user.MoveWindow(hwnd,0,0,1044,760,True)
            desktop.flush_frames()
            report['themes'].append({'theme':theme,'test_gate':'PASS','history_form':'PASS','left_resize':'PASS','scrollFrames':24})
        user.PostMessageW(hwnd,0x10,0,0);process.wait(timeout=15);process=None
        report.update(status='PASS',systemDpi=desktop.report.get('dpi'),frames=desktop.frames)
    except Exception as exc:
        report.update(status='FAIL',error=str(exc));raise
    finally:
        if process and process.poll() is None:process.terminate();process.wait(timeout=10)
        if created and config.exists():shutil.rmtree(config)
        if moved:os.replace(backup,config)
        (args.output/'optimization-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(report,ensure_ascii=True))

if __name__=='__main__':main()
