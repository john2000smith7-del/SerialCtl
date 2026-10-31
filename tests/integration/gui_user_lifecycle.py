"""Local Win32 GUI lifecycle check; creates real CMD jobs, no remote permissions."""
import ctypes
from ctypes import wintypes as w
import json
import pathlib
import subprocess
import sys
import time
import traceback
sys.path.insert(0, sys.argv[1])
from serialctl_ws import Client
window, pid = int(sys.argv[2]), int(sys.argv[3])
u, k = ctypes.windll.user32, ctypes.windll.kernel32
u.GetDlgItem.argtypes = (w.HWND, ctypes.c_int)
u.GetDlgItem.restype = w.HWND
u.FindWindowExW.argtypes = (w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR)
u.FindWindowExW.restype = w.HWND
u.GetWindowTextW.argtypes = (w.HWND, w.LPWSTR, ctypes.c_int)
u.SendMessageW.argtypes = (w.HWND, w.UINT, ctypes.c_size_t, ctypes.c_ssize_t)
u.SendMessageW.restype = ctypes.c_ssize_t
u.GetClientRect.argtypes = u.GetWindowRect.argtypes = (w.HWND, ctypes.POINTER(w.RECT))
u.ClientToScreen.argtypes = (w.HWND, ctypes.POINTER(w.POINT))
u.SetForegroundWindow.argtypes = (w.HWND,)
u.GetForegroundWindow.restype = w.HWND
u.IsWindow.argtypes = u.IsWindowVisible.argtypes = u.IsWindowEnabled.argtypes = (w.HWND,)
u.GetDlgCtrlID.argtypes = (w.HWND,)
u.GetWindowThreadProcessId.argtypes = (w.HWND, ctypes.POINTER(w.DWORD))
u.GetWindowThreadProcessId.restype = w.DWORD
u.WindowFromPoint.argtypes = (w.POINT,)
u.WindowFromPoint.restype = w.HWND
u.SetCursorPos.argtypes = (ctypes.c_int, ctypes.c_int)
u.mouse_event.argtypes = (w.DWORD,w.DWORD,w.DWORD,w.DWORD,ctypes.c_size_t)
u.GetDoubleClickTime.restype = w.UINT
class MouseInput(ctypes.Structure):
    _fields_ = [('dx',w.LONG),('dy',w.LONG),('mouseData',w.DWORD),('flags',w.DWORD),
                ('time',w.DWORD),('extra',ctypes.c_size_t)]
class KeyboardInput(ctypes.Structure):
    _fields_ = [('vk',w.WORD),('scan',w.WORD),('flags',w.DWORD),('time',w.DWORD),('extra',ctypes.c_size_t)]
class HardwareInput(ctypes.Structure):
    _fields_ = [('message',w.DWORD),('paramLow',w.WORD),('paramHigh',w.WORD)]
class InputData(ctypes.Union):
    _fields_ = [('mouse',MouseInput),('key',KeyboardInput),('hardware',HardwareInput)]
class Input(ctypes.Structure):
    _fields_ = [('type',w.DWORD),('data',InputData)]
u.SendInput.argtypes = (w.UINT, ctypes.POINTER(Input), ctypes.c_int)
u.SendInput.restype = w.UINT
class GuiThreadInfo(ctypes.Structure):
    _fields_ = [('size',w.DWORD),('flags',w.DWORD),('active',w.HWND),('focus',w.HWND),
                ('capture',w.HWND),('menuOwner',w.HWND),('moveSize',w.HWND),('caret',w.HWND),('caretRect',w.RECT)]
u.GetGUIThreadInfo.argtypes = (w.DWORD, ctypes.POINTER(GuiThreadInfo))
input_events=[]
observations=[]
behavior_failures=[]
last_click={}
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
def input_target(control_id,point=None):
    """Read HWND geometry/state, then require that a real pointer hits this control."""
    control=u.GetDlgItem(window,control_id)
    assert control and u.IsWindow(control),'Missing input target %d'%control_id
    assert u.GetDlgCtrlID(control)==control_id
    owner=w.DWORD()
    thread=u.GetWindowThreadProcessId(control,ctypes.byref(owner))
    assert owner.value==pid and thread,'Input target belongs to another process'
    assert u.IsWindowVisible(control) and u.IsWindowEnabled(control),'Unavailable control %d'%control_id
    if u.GetForegroundWindow()!=window:
        u.SetForegroundWindow(window)
        wait(lambda:u.GetForegroundWindow()==window,2)
    rect=w.RECT()
    assert u.GetWindowRect(control,ctypes.byref(rect))
    assert rect.right>rect.left and rect.bottom>rect.top
    x,y=point if point is not None else ((rect.left+rect.right)//2,(rect.top+rect.bottom)//2)
    assert rect.left<=x<rect.right and rect.top<=y<rect.bottom,'Pointer target lies outside control'
    assert u.SetCursorPos(x,y)
    assert u.WindowFromPoint(w.POINT(x,y))==control,'Input target %d is obscured or not hit'%control_id
    return control,thread,x,y
def click(control_id,point=None):
    # Repeated CMD creation is a series of separate user clicks, not double-clicks.
    elapsed=time.monotonic()-last_click.get(control_id,0)
    delay=u.GetDoubleClickTime()/1000+.02-elapsed
    if delay>0:time.sleep(delay)
    control,thread,x,y=input_target(control_id,point)
    u.mouse_event(2,0,0,0,0)
    try:time.sleep(.03)
    finally:u.mouse_event(4,0,0,0,0)
    last_click[control_id]=time.monotonic()
    input_events.append({'action':'click','control':control_id,'hwnd':int(control),'point':[x,y],
                         'monotonic':last_click[control_id],'method':'system mouse_event down/up'})
    return control,thread
def focus_state(thread):
    info=GuiThreadInfo();info.size=ctypes.sizeof(info)
    assert u.GetGUIThreadInfo(thread,ctypes.byref(info)),'Cannot inspect GUI keyboard focus'
    return {'hwnd':int(info.focus or 0),'control':u.GetDlgCtrlID(info.focus) if info.focus else None,
            'foreground':int(u.GetForegroundWindow() or 0)}
def focused(control,thread):
    state=focus_state(thread)
    return state['foreground']==window and state['hwnd']==control
def control_text(control):
    text=ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(control,text,len(text))
    return text.value
def key(control_id,vk,require_list_focus=True):
    control,thread,_,_=input_target(control_id)
    focus=focus_state(thread)
    if require_list_focus:
        assert focused(control,thread),'Keyboard target lacks actual GUI focus'
    else:
        # Deliberately retain the focus produced by Home. Re-focusing the list here
        # would conceal the behavior of consecutive real keyboard navigation.
        actual=focus['hwnd'];owner=w.DWORD()
        assert actual and u.IsWindow(actual) and u.IsWindowVisible(actual) and u.IsWindowEnabled(actual)
        u.GetWindowThreadProcessId(actual,ctypes.byref(owner))
        assert owner.value==pid and focus['foreground']==window,'Keyboard focus left the tested application'
    down=Input();down.type=1;down.data.key.vk=vk;down.data.key.flags=1 # extended Home/End
    up=Input();up.type=1;up.data.key.vk=vk;up.data.key.flags=3
    events=(Input*2)(down,up)
    assert u.SendInput(2,events,ctypes.sizeof(Input))==2,'System keyboard injection failed'
    input_events.append({'action':'key','control':control_id,'hwnd':int(control),'vk':vk,
                         'actual_focus_before':focus,'required_list_focus':require_list_focus,
                         'monotonic':time.monotonic(),'method':'SendInput virtual key down/up'})
def thumb_geometry(listing):
    bounds=w.RECT();origin=w.POINT(0,0)
    assert u.GetClientRect(listing,ctypes.byref(bounds)) and u.ClientToScreen(listing,ctypes.byref(origin))
    item_height=u.SendMessageW(listing,0x1a1,0,0)
    count_items=u.SendMessageW(listing,0x18b,0,0)
    assert item_height>0
    page=max(1,(bounds.bottom-bounds.top)//item_height)
    max_top=count_items-page
    assert max_top>0,'Expected enough connections for a visible overlay scroll thumb'
    track_height=bounds.bottom-16
    thumb_height=min(track_height,max(24,track_height*page//count_items))
    current_top=u.SendMessageW(listing,0x18e,0,0)
    thumb_top=8+(track_height-thumb_height)*min(max_top,current_top)//max_top
    return origin.x+bounds.right-8,origin.y+thumb_top+thumb_height//2,origin
def reacquire_list_focus(listing,reason):
    before=u.SendMessageW(listing,0x18e,0,0)
    selected=u.SendMessageW(listing,0x188,0,0)
    x,y,_=thumb_geometry(listing)
    control,thread=click(106,(x,y))
    assert control==listing
    wait(lambda:focused(listing,thread),2)
    assert u.SendMessageW(listing,0x18e,0,0)==before,'Thumb focus click changed the scroll anchor'
    assert u.SendMessageW(listing,0x188,0,0)==selected,'Thumb focus click changed selection'
    input_events.append({'action':'reacquire-list-focus','reason':reason,'control':106,
                         'focus':focus_state(thread),'method':'actual click on current overlay thumb; no SetFocus',
                         'monotonic':time.monotonic()})
    return thread
def navigation_state(listing,thread,power_pane):
    return {'selection':u.SendMessageW(listing,0x188,0,0),
            'top_index':u.SendMessageW(listing,0x18e,0,0),
            'focus_after':focus_state(thread),
            'terminal_visible':bool(u.IsWindowVisible(u.GetDlgItem(window,107))),
            'power_visible':bool(u.IsWindowVisible(power_pane))}
def verify_navigation(listing,thread,theme,label,vk,selection_matches,power_row,power_pane):
    # Each call requires focus produced by the preceding key, with no repair.
    key(106,vk)
    deadline=time.monotonic()+2
    while True:
        state=navigation_state(listing,thread,power_pane)
        passed=(selection_matches(state['selection']) and
                state['focus_after']['hwnd']==listing and
                state['focus_after']['foreground']==window and
                state['terminal_visible']==(state['selection']!=power_row) and
                state['power_visible']==(state['selection']==power_row))
        if passed or time.monotonic()>=deadline:break
        time.sleep(.02)
    observations.append({'action':label,'theme':theme,'result':state,'passed':passed,
                         'input_sequence':'uninterrupted real connection-list keyboard navigation'})
    if not passed:
        behavior_failures.append({'check':label,'theme':theme,'actual':state,
                                  'expected':'Native list selection, list HWND 106 focus, and matching session/power view',
                                  'power_row':power_row})
    return passed
def verify_extended_navigation(listing,thread,theme,power_row,power_pane):
    # Start immediately after End selected the power row. Page offsets are native
    # listbox behavior; assert meaningful movement without reproducing its formula.
    cases=[('Up-from-power',0x26,lambda selected:selected==power_row-1),
           ('Down-to-power',0x28,lambda selected:selected==power_row),
           ('PageUp-from-power',0x21,lambda selected:0<=selected<power_row)]
    for label,vk,selection_matches in cases:
        if not verify_navigation(listing,thread,theme,label,vk,selection_matches,power_row,power_pane):return False
    before_page_down=u.SendMessageW(listing,0x188,0,0)
    cases=[('PageDown',0x22,lambda selected:before_page_down<selected<=power_row),
           ('Home-from-lower-rows',0x24,lambda selected:selected==0),
           ('Down-between-sessions',0x28,lambda selected:selected==1),
           ('Up-between-sessions',0x26,lambda selected:selected==0),
           ('End-to-power',0x23,lambda selected:selected==power_row),
           ('Home-from-power',0x24,lambda selected:selected==0)]
    for label,vk,selection_matches in cases:
        if not verify_navigation(listing,thread,theme,label,vk,selection_matches,power_row,power_pane):return False
    return True
def verify_mouse_session_activation(listing,thread,theme,power_row,power_pane):
    # End the independent keyboard sequence with an actual click. Selecting a
    # different session detects a keyboard-origin flag leaking into mouse input.
    assert focused(listing,thread),'Completed keyboard sequence must retain list focus'
    selected=u.SendMessageW(listing,0x188,0,0)
    top=u.SendMessageW(listing,0x18e,0,0)
    target=top if top!=selected else top+1
    assert 0<=target<power_row and target!=selected,'Expected a visible unselected session row'
    item_height=u.SendMessageW(listing,0x1a1,0,0)
    bounds=w.RECT();origin=w.POINT(0,0)
    assert u.GetClientRect(listing,ctypes.byref(bounds)) and u.ClientToScreen(listing,ctypes.byref(origin))
    row_y=(target-top)*item_height+item_height//2
    assert item_height>0 and 0<=row_y<bounds.bottom,'Session click target is outside the visible list'
    actual_list,_=click(106,(origin.x+bounds.right//2,origin.y+row_y))
    assert actual_list==listing
    terminal=u.GetDlgItem(window,107)
    deadline=time.monotonic()+2
    while True:
        state=navigation_state(listing,thread,power_pane)
        passed=(state['selection']==target and state['focus_after']['hwnd']==terminal and
                state['focus_after']['foreground']==window and state['terminal_visible'] and
                not state['power_visible'])
        if passed or time.monotonic()>=deadline:break
        time.sleep(.02)
    observations.append({'action':'mouse-session-after-keyboard-navigation','theme':theme,
                         'selection_before':selected,'expected_selection':target,
                         'expected_focus_control':107,'result':state,'passed':passed,
                         'method':'system mouse click on an unselected ordinary session row'})
    if not passed:
        behavior_failures.append({'check':'Mouse session activation after keyboard navigation',
                                  'theme':theme,'expected_selection':target,'expected_focus_control':107,
                                  'actual':state})
    return passed
def park_desktop_pet():
    """Real fixture setup: keep the topmost pet enabled, outside list-test geometry."""
    from types import SimpleNamespace
    from desktop_user_actions import DesktopActions
    pet=u.FindWindowExW(None,None,'SerialCtlAiPetPane',None)
    if not pet or not u.IsWindowVisible(pet):
        return
    owner=w.DWORD()
    u.GetWindowThreadProcessId(pet,ctypes.byref(owner))
    assert owner.value==pid,'Desktop pet belongs to another application instance'
    desktop=DesktopActions(SimpleNamespace(window=window,pid=pid,
        output=str(pathlib.Path(sys.argv[4]).parent),prefix='gui-lifecycle-pet',mode='desktop'))
    bounds=desktop.rect(pet)
    main=desktop.rect(window)
    screen=desktop.desktop_rect()
    width=bounds[2]-bounds[0]
    height=bounds[3]-bounds[1]
    # The approved birth anchor is above the status area and can cover list rows.
    # A user moves this independent topmost window before operating those rows.
    # Do that once here, never synthesize a command or relax a control hit test.
    if main[2]+width+24<=screen[2]:
        left=main[2]+12
    elif main[0]-width-24>=screen[0]:
        left=main[0]-width-12
    else:
        raise AssertionError('Lifecycle fixture requires visible desktop space beside the main window')
    start=None
    for fy in (.75,.65,.85,.55):
        for fx in (.5,.4,.6):
            point=(bounds[0]+round(width*fx),bounds[1]+round(height*fy))
            if u.WindowFromPoint(w.POINT(*point))==pet:
                start=point
                break
        if start:break
    assert start,'Cannot hit the visible desktop pet body for real fixture dragging'
    desktop.capture('before-parking')
    desktop.move_mouse(start)
    time.sleep(.08)
    assert u.WindowFromPoint(w.POINT(*start))==pet,'Pet drag start was covered'
    try:
        desktop.drag('park-pet-beside-main',start,(start[0]+left-bounds[0],start[1]))
        after=desktop.rect(pet)
        assert abs(after[0]-left)<=3,'Actual mouse did not move the desktop pet'
        assert after[2]<=main[0] or after[0]>=main[2],'Parked desktop pet still covers the main window'
        assert u.IsWindowVisible(pet),'Parking hid the desktop pet'
        desktop.report['status']='PASS'
        input_events.append({'action':'park-desktop-pet','hwnd':int(pet),'before':bounds,
            'after':after,'method':'real system mouse drag; enabled/topmost preference unchanged',
            'desktop_frames':[frame['path'] for frame in desktop.frames]})
    finally:
        desktop.flush_frames()
    u.SetCursorPos((main[0]+main[2])//2,main[1]+60)
    u.SetForegroundWindow(window)
    wait(lambda:u.GetForegroundWindow()==window,2)

def run(c):
    park_desktop_pet()
    count=lambda:len([r for r in c.resources()['resources'] if r['kind']=='cmd' and r['connected']])
    baseline=count()
    listing=u.GetDlgItem(window,106)
    power_pane=u.FindWindowExW(window,None,'SerialCtlPowerPane',None)
    assert power_pane and u.IsWindow(power_pane),'Missing direct-child power pane'
    power_owner=w.DWORD()
    power_thread=u.GetWindowThreadProcessId(power_pane,ctypes.byref(power_owner))
    assert power_owner.value==pid and power_thread,'Power pane belongs to another process'
    # Real toolbar activation establishes the disconnected power pseudo-row;
    # keyboard transitions do not require or operate physical power hardware.
    click(301)
    wait(lambda:u.SendMessageW(listing,0x18b,0,0)>0 and
         u.SendMessageW(listing,0x188,0,0)==u.SendMessageW(listing,0x18b,0,0)-1 and
         not u.IsWindowVisible(u.GetDlgItem(window,107)) and u.IsWindowVisible(power_pane))
    observations.append({'action':'power-row-setup','method':'system mouse click on toolbar control 301',
                         'power_row':u.SendMessageW(listing,0x188,0,0),'terminal_visible':False,
                         'power_hwnd':int(power_pane),'power_pid':power_owner.value,'power_visible':True})
    baseline_rows=u.SendMessageW(listing,0x18b,0,0)
    for index in range(32):
        click(300)
        wait(lambda:count()==baseline+index+1)
        wait(lambda:u.SendMessageW(listing,0x18b,0,0)==baseline_rows+index+1)
    # IsConnected means the native bridge transport is established; its real CMD
    # is created asynchronously. Keep the exact process assertion after readiness.
    wait(lambda:sum(name.lower()=='cmd.exe' for name in children().values())==baseline+32)
    live=children()
    assert sum(name.lower()=='cmd.exe' for name in live.values())==baseline+32,live
    u.SendMessageW(listing,0x197,12,0) # Fixture setup only: LB_SETTOPINDEX, not a simulated user action.
    top=u.SendMessageW(listing,0x18e,0,0)
    assert top==12,top
    click(300)
    wait(lambda:count()==baseline+33)
    wait(lambda:u.SendMessageW(listing,0x18b,0,0)==baseline_rows+33)
    wait(lambda:sum(name.lower()=='cmd.exe' for name in children().values())==baseline+33)
    assert u.SendMessageW(listing,0x18e,0,0)==top,'top resource ID must survive a rebuilt list with duplicate CMD names'
    live=children()
    known_processes={p:creation(p) for p in live}
    selection=u.SendMessageW(listing,0x188,0,0)
    # Fixture-triggered production state-notification refresh, not a user action.
    # It must retain both anchors before the subsequent real input changes selection.
    for _ in range(20):u.SendMessageW(window,0x8009,0,0)
    assert u.SendMessageW(listing,0x18e,0,0)==top
    assert u.SendMessageW(listing,0x188,0,0)==selection
    rect=w.RECT();assert u.GetWindowRect(listing,ctypes.byref(rect))
    first_row_y=rect.top+u.SendMessageW(listing,0x1a1,0,0)//2
    focused_list,thread=click(106,((rect.left+rect.right)//2,first_row_y))
    assert focused_list==listing
    wait(lambda:u.SendMessageW(listing,0x188,0,0)==top)
    focus_started=time.monotonic()
    row_focus=focus_state(thread)
    # Selection is observable before the parent finishes native activation.
    # Wait for completion without an extra click, key or forced focus repair.
    while row_focus['hwnd']!=u.GetDlgItem(window,107) and time.monotonic()-focus_started<2:
        time.sleep(.01)
        row_focus=focus_state(thread)
    observations.append({'action':'connection-row-click','selection':u.SendMessageW(listing,0x188,0,0),
                         'focus_after':row_focus,'expected_focus_control':107,
                         'classification':'Mouse row activation must preserve terminal focus behavior'})
    if row_focus['hwnd']!=u.GetDlgItem(window,107) or row_focus['foreground']!=window:
        behavior_failures.append({'check':'Mouse connection-row activation focuses terminal',
                                  'expected_focus_control':107,'actual':row_focus})
    assert u.SendMessageW(listing,0x18e,0,0)==top,'Focusing a visible row moved the top anchor'
    thread=reacquire_list_focus(listing,'Establish real list keyboard focus after observing the row-click result')
    input_target(106)
    u.mouse_event(0x800,0,0,(-120)&0xffffffff,0)
    input_events.append({'action':'wheel','control':106,'hwnd':int(listing),'delta':-120,
                         'monotonic':time.monotonic(),'method':'system mouse_event wheel'})
    wait(lambda:u.SendMessageW(listing,0x18e,0,0)>top)
    assert u.SendMessageW(listing,0x18e,0,0)>top,'wheel must scroll native/overlay list'
    key(106,0x24) # Actual keyboard Home.
    wait(lambda:u.SendMessageW(listing,0x188,0,0)==0)
    home_state=navigation_state(listing,thread,power_pane)
    observations.append({'action':'Home','result':home_state})
    key(106,0x23,require_list_focus=False) # End follows Home without repairing focus.
    expected_end=u.SendMessageW(listing,0x18b,0,0)-1
    deadline=time.monotonic()+2
    while u.SendMessageW(listing,0x188,0,0)!=expected_end and time.monotonic()<deadline:
        time.sleep(.02)
    end_state=navigation_state(listing,thread,power_pane)
    observations.append({'action':'End-directly-after-Home','expected_selection':expected_end,'result':end_state})
    consecutive_passed=(end_state['selection']==expected_end and
                        home_state['focus_after']['hwnd']==listing and
                        end_state['focus_after']['hwnd']==listing and
                        home_state['focus_after']['foreground']==window and
                        end_state['focus_after']['foreground']==window and
                        home_state['terminal_visible'] and not home_state['power_visible'] and
                        not end_state['terminal_visible'] and end_state['power_visible'])
    if not consecutive_passed:
        behavior_failures.append({'check':'consecutive Home/End connection-list navigation',
                                  'expected':'Home selects a session and End selects power; list HWND 106 retains focus and exactly the selected view is visible',
                                  'expected_selection':expected_end,'actual':end_state,'home':home_state,
                                  'impact':'Keyboard navigation leaves the connection list after selecting a session'})
        observations.append({'action':'extended-list-keyboard-navigation','status':'SKIPPED',
                             'reason':'Original uninterrupted Home/End sequence failed; no focus repair conceals it'})
    else:
        theme_button=u.GetDlgItem(window,104)
        original_theme_caption=control_text(theme_button)
        assert original_theme_caption in ('浅色','深色'),'Unexpected theme button caption'
        current_theme='dark' if original_theme_caption=='浅色' else 'light'
        # The first extended sequence directly follows the original Home/End,
        # before any thumb click or other focus recovery.
        if verify_extended_navigation(listing,thread,current_theme,expected_end,power_pane):
            verify_mouse_session_activation(listing,thread,current_theme,expected_end,power_pane)
            try:
                click(104)
                expected_theme_caption='深色' if original_theme_caption=='浅色' else '浅色'
                wait(lambda:control_text(theme_button)==expected_theme_caption)
                other_theme='light' if current_theme=='dark' else 'dark'
                observations.append({'action':'theme-switch-for-keyboard-navigation','theme':other_theme,
                                     'caption_before':original_theme_caption,'caption_after':control_text(theme_button)})
                thread=reacquire_list_focus(listing,'Start an independent navigation sequence in '+other_theme+' theme')
                if (verify_navigation(listing,thread,other_theme,'Home',0x24,lambda selected:selected==0,expected_end,power_pane) and
                        verify_navigation(listing,thread,other_theme,'End-directly-after-Home',0x23,
                                          lambda selected:selected==expected_end,expected_end,power_pane)):
                    if verify_extended_navigation(listing,thread,other_theme,expected_end,power_pane):
                        verify_mouse_session_activation(listing,thread,other_theme,expected_end,power_pane)
            finally:
                if control_text(theme_button)!=original_theme_caption:
                    click(104)
                    wait(lambda:control_text(theme_button)==original_theme_caption)
        else:
            observations.append({'action':'other-theme-list-keyboard-navigation','status':'SKIPPED',
                                 'reason':'Extended uninterrupted navigation failed in the initial theme'})
    # This recovery is a separate user action for subsequent checks. It cannot
    # turn a failed uninterrupted Home/End sequence into a passing result.
    thread=reacquire_list_focus(listing,'Independently verify End and continue lifecycle checks after recording consecutive-key behavior')
    key(106,0x23)
    wait(lambda:u.SendMessageW(listing,0x188,0,0)==u.SendMessageW(listing,0x18b,0,0)-1)
    recovered_end=navigation_state(listing,thread,power_pane)
    observations.append({'action':'End-after-explicit-thumb-focus-recovery',
                         'result':recovered_end})
    if (recovered_end['focus_after']['hwnd']!=listing or
            recovered_end['focus_after']['foreground']!=window or
            recovered_end['terminal_visible'] or not recovered_end['power_visible']):
        behavior_failures.append({'check':'End after explicit thumb focus recovery',
                                  'expected':'Power view visible, terminal hidden, list focus retained',
                                  'actual':recovered_end})
    # Physical mouse drag of the visible overlay thumb, with real desktop captures.
    mouse_x,start_y,origin=thumb_geometry(listing)
    actual_list,_,_,_=input_target(106,(mouse_x,start_y))
    assert actual_list==listing
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
    input_events.append({'action':'overlay-thumb-drag','control':106,'hwnd':int(listing),
                         'monotonic':time.monotonic(),'method':'system mouse_event down/up with SetCursorPos path'})
    wait(lambda:u.SendMessageW(listing,0x18e,0,0)==0)
    resource=sorted([r for r in c.resources()['resources'] if r['kind']=='cmd'],key=lambda r:int(r['id'].split('-')[1]))[-1]['id']
    c.input(resource,b'ping -n 100 127.0.0.1 >nul\r\n')
    time.sleep(.1)
    started=time.monotonic()
    click(302)
    wait(lambda:not [r for r in c.resources()['resources'] if r['kind']=='cmd'])
    wait(lambda:u.SendMessageW(listing,0x18b,0,0)==0)
    wait(lambda:not children())
    wait(lambda:all(creation(p)!=created for p,created in known_processes.items()))
    duration=time.monotonic()-started
    report={'created_real_cmd_sessions':baseline+33,'child_processes_before':live,'children_after':children(),
            'disconnect_seconds':duration,'top_anchor':top,'selection_anchor':selection,
            'top_after_mouse_drag':0,'desktop_frames':images,
            'known_child_creation_times':known_processes,'known_children_all_exited':True,
            'checks':['stable refresh fixture','system mouse creation of 33 CMD sessions','system mouse wheel',
                      'SendInput Home/End with observed actual focus and explicit recovery',
                      'conditional uninterrupted arrows/pages/session-power navigation in both themes',
                      'mouse row activation retains terminal focus','physical overlay thumb drag',
                      'system mouse all-disconnect','job child termination'],
            'input_events':input_events,
            'input_validation':'Every input target HWND is matched to the GUI PID and control ID; visible, enabled, foreground, and WindowFromPoint hit-tested; keyboard focus read through GetGUIThreadInfo',
            'fixtures':['LB_SETTOPINDEX establishes top anchor 12; not a user action',
                        '20 WM_APP+9 messages trigger state-notification refresh; not user actions',
                        'WebSocket input starts a real long-running CMD child before all-disconnect'],
            'hardware':'no physical serial/SFTP/power; task-owned output covered separately by PowerService controlled tests'}
    print('Completed: %d real CMD sessions, list anchors/wheel/keys, all-disconnect child cleanup %.3fs'%(baseline+33,duration))
    return report

report={'status':'FAIL','window':window,'pid':pid}
exit_code=1
try:
    with Client('127.0.0.1') as client:
        report.update(run(client))
    report['status']='FAIL' if behavior_failures else 'PASS'
    exit_code=2 if behavior_failures else 0
except Exception as error:
    report.update({'status':'FAIL','exception':repr(error),'traceback':traceback.format_exc()})
    print(report['traceback'],file=sys.stderr)
finally:
    # Release any physical drag even if screenshot capture or another assertion failed.
    u.mouse_event(4,0,0,0,0)
    report.update({'input_events':input_events,'observations':observations,
                   'behavior_failures':behavior_failures,'exit_code':exit_code})
    pathlib.Path(sys.argv[4]).write_text(json.dumps(report,indent=2),encoding='utf-8')
print('%s: GUI lifecycle checks; %d observed behavior failures'%(report['status'],len(behavior_failures)))
sys.exit(exit_code)
