"""Exercise the production notebook with real mouse, clipboard and CMD output."""
import argparse
import base64
import ctypes as c
import json
from pathlib import Path
import queue
import sys
import time
import traceback
import uuid
from gui_global_user_actions import GlobalActions
from desktop_user_actions import require


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('window','pid'):p.add_argument('--'+key,type=int,required=True)
    for key in ('output','prefix','native-dir'):p.add_argument('--'+key,required=True)
    p.add_argument('--edge-scroll',action='store_true')
    args=p.parse_args();args.mode='desktop';args.root=args.window
    t=GlobalActions(args);client=None
    try:
        t.user.SetForegroundWindow(args.window)
        t.click_window(t.control(300))
        sys.path.insert(0,args.native_dir)
        from serialctl_ws import Client
        client=Client('127.0.0.1')
        active=[]
        def cmd_ready():
            active[:]=[r['id'] for r in client.resources()['resources'] if r['kind']=='cmd' and r['connected']]
            return bool(active)
        t.wait(cmd_ready,'real CMD is connected',10)
        cmd=active[-1];snap=client.events_since(cmd,(1<<64)-1);client.subscribe(cmd,snap['cursor'])
        pane=t.control(112);edit=t.user.GetDlgItem(pane,410)
        require(edit,'native notebook text control missing')
        token='NB_'+uuid.uuid4().hex[:8]
        text='# selection fixture\r\necho '+token+'_A\r\necho '+token+'_B\r\necho '+token+'_C'
        t.click_window(edit);t.chord(0x11,ord('A'));t.set_clipboard(text);t.chord(0x11,ord('V'))
        t.wait(lambda:token+'_C' in t.native_control_text(edit),'clipboard pasted in the actual editor')
        # Partial first and last lines: run the three complete intersecting lines.
        t.chord(0x11,0x24);t.key(0x28)
        for _ in range(7):t.key(0x27)
        t.chord(0x10,0x28);t.chord(0x10,0x28)
        t.screenshot('partial-three-lines')
        t.click_window(t.user.GetDlgItem(pane,412))
        accepted=[];output=bytearray();deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:e=client.next_event(cmd,timeout=.05)
            except queue.Empty:continue
            raw=base64.b64decode(e.get('data',''))
            if e.get('kind')=='input' and e.get('source')=='gui' and token.encode() in raw:accepted.append(raw)
            if e.get('kind')=='output':output.extend(raw)
            if len(accepted)==3 and (token+'_C').encode() in output:break
        require(len(accepted)==3,'Run must submit exactly three complete commands')
        for i,raw in enumerate(accepted):require(raw.rstrip(b'\r\n')==('echo '+token+'_'+chr(65+i)).encode(),'partial text selection sent a truncated command')
        t.check('partial selection sends complete lines',submitted=[x.decode().strip() for x in accepted])
        # Ordinary Enter edits the document, and clipboard copy is ordinary text selection.
        t.click_window(edit);t.chord(0x11,0x23);t.key(0x0d);t.text('# retained note')
        t.wait(lambda:'retained note' in t.native_control_text(edit),'Enter inserts a new document line')
        t.chord(0x11,ord('A'));t.chord(0x11,ord('C'))
        require('retained note' in (t.clipboard() or ''),'editor Copy must preserve selected text')
        t.click_window(t.user.GetDlgItem(pane,413));t.screenshot('saved-light')
        t.check('document edit copy save')
        # File actions really open owned system file dialogs and cancel cleanly.
        for identifier,name in ((414,'import'),(415,'export')):
            t.click_window(t.user.GetDlgItem(pane,identifier))
            t.wait(lambda:t.user.GetForegroundWindow()!=args.window,name+' opens native file dialog')
            t.key(0x1b)
            t.wait(lambda:t.user.GetForegroundWindow()==args.window,name+' dialog returns to owner')
            t.check(name+' dialog')
        # Resize and collapse/expand use the existing sidebar split and native editor.
        for width in (980,1200):
            t.user.MoveWindow(args.window,0,0,min(width,t.user.GetSystemMetrics(0)),min(800,t.user.GetSystemMetrics(1)-30),True)
            time.sleep(.15);t.screenshot('width-'+str(width))
            for identifier in range(412,416):
                button=t.user.GetDlgItem(pane,identifier)
                require(t.user.IsWindowVisible(button),'compact command action disappeared')
                bounds=t.rect(button);require(bounds[2]>bounds[0] and bounds[3]>bounds[1],'compact action has empty bounds')
        t.click_window(t.control(104));t.screenshot('saved-dark')
        t.check('themes and responsive command actions')
        t.report['status']='PASS'
    except BaseException:
        t.report['status']='FAIL';t.report['error']=traceback.format_exc()
        try:t.screenshot('failure')
        except Exception:pass
        raise
    finally:
        if client:client.close()
        t.flush_frames()
        out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
        (out/'notebook-user-actions.json').write_text(json.dumps(t.report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps({'status':t.report.get('status'),'checks':t.report.get('checks'),'scope':'Real packaged GUI/SendInput/clipboard/CMD; no physical UART or Win7 hardware'}),flush=True)

if __name__=='__main__':main()
