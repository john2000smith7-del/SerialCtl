"""Run a measured multi-session high-throughput soak on the packaged GUI.

The producer writes to two real CMD child stdout pipes. Its byte counters are
recorded at the successful Write() boundary, so requested and achieved rates
remain distinguishable. All GUI operations use system mouse input.
"""
import argparse
import ctypes as c
import json
from pathlib import Path
import statistics
import sys
import time
import traceback

from desktop_user_actions import POINT, require
from gui_endurance import Endurance, wait


PRODUCER = r'''param(
    [Parameter(Mandatory=$true)][string]$Report,
    [Parameter(Mandatory=$true)][int]$Worker,
    [Parameter(Mandatory=$true)][int]$SoakSeconds
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$stream = [Console]::OpenStandardOutput()
$blockSize = 16384
$block = New-Object byte[] $blockSize
$line = "`e[31mRED`e[0m worker=$Worker ANSI 中文 Ω wide-output terminal-soak " + ('x' * 48) + "`r`n"
$lineBytes = $utf8.GetBytes($line)
for ($offset = 0; $offset -lt $blockSize; $offset += $lineBytes.Length) {
    [Array]::Copy($lineBytes, 0, $block, $offset, [Math]::Min($lineBytes.Length, $blockSize - $offset))
}
$overall = [Diagnostics.Stopwatch]::StartNew()
$totalBytes = [int64]0
$sequence = [int64]0
$results = @()

function Save-Progress([string]$Status, [string]$Phase) {
    $document = [ordered]@{
        status = $Status; worker = $Worker; phase = $Phase
        totalBytes = $script:totalBytes; elapsedSeconds = $script:overall.Elapsed.TotalSeconds
        sequence = $script:sequence; phases = $script:results
    }
    $temporary = $Report + '.tmp'
    [IO.File]::WriteAllText($temporary, ($document | ConvertTo-Json -Depth 5), $utf8)
    Move-Item -LiteralPath $temporary -Destination $Report -Force
}

function Run-Phase([string]$Name, [int64]$BytesPerSecond, [int]$Seconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $phaseBytes = [int64]0
    $nextProgress = 5.0
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $marker = $utf8.GetBytes(('SERIALCTL_PAINT_{0:D3} SOAK_WORKER_{1}_SEQ_{2:D12} ' -f ($script:sequence % 1000), $Worker, $script:sequence))
        [Array]::Copy($marker, 0, $block, 0, [Math]::Min($marker.Length, 64))
        $stream.Write($block, 0, $block.Length)
        $phaseBytes += $block.Length
        $script:totalBytes += $block.Length
        $script:sequence++
        $expected = $phaseBytes / [double]$BytesPerSecond
        $ahead = $expected - $watch.Elapsed.TotalSeconds
        if ($ahead -gt 0.001) { Start-Sleep -Milliseconds ([Math]::Min(5, [Math]::Floor($ahead * 1000))) }
        if ($watch.Elapsed.TotalSeconds -ge $nextProgress) {
            Save-Progress 'RUNNING' $Name
            $nextProgress += 5.0
        }
    }
    $stream.Flush()
    $elapsed = $watch.Elapsed.TotalSeconds
    $script:results += [ordered]@{name=$Name; targetBytesPerSecond=$BytesPerSecond;
        bytes=$phaseBytes; elapsedSeconds=$elapsed; actualBytesPerSecond=$phaseBytes/$elapsed}
    Save-Progress 'RUNNING' $Name
}

try {
    Run-Phase 'burst-5mib-aggregate' (2621440) 60
    Run-Phase 'soak-1mib-aggregate' (524288) $SoakSeconds
    Save-Progress 'PASS' 'complete'
} catch {
    Save-Progress ('FAIL: ' + $_.Exception.Message) 'exception'
    throw
}
'''


def percentile(values, percent):
    ordered=sorted(values)
    if not ordered:return None
    return ordered[min(len(ordered)-1, int((len(ordered)-1)*percent+0.999999))]


class TerminalSoak(Endurance):
    def __init__(self,args):
        super().__init__(args)
        self.diagnostics=[]
        self.paint_markers=[]
        self.report['scope']='Two real CMD stdout pipes plus actual packaged GUI and system mouse input'
        self.report['requested_load']={'burst_seconds':60,'burst_bytes_per_second':5*1024*1024,
                                       'soak_seconds':args.soak_seconds,'soak_bytes_per_second':1024*1024}

    def click_session_row(self,index):
        listing=self.control(106)
        count=int(self.user.SendMessageW(listing,0x18b,0,0))
        item_height=int(self.user.SendMessageW(listing,0x1a1,0,0))
        top=int(self.user.SendMessageW(listing,0x18e,0,0))
        require(0<=index<count and item_height>0,'Session list row is unavailable')
        if not top<=index<top+max(1,(self.rect(listing)[3]-self.rect(listing)[1])//item_height):
            # Positioning the viewport is fixture setup. Session activation below
            # remains a real mouse click and is verified from native selection.
            self.user.SendMessageW(listing,0x197,index,0)
            top=int(self.user.SendMessageW(listing,0x18e,0,0))
        bounds=self.rect(listing)
        point=((bounds[0]+bounds[2])//2,bounds[1]+(index-top)*item_height+item_height//2)
        self.hit_click(listing,point)
        wait(lambda:int(self.user.SendMessageW(listing,0x188,0,0))==index,2)

    def wheel_terminal(self,direction):
        bounds=self.rect(self.control(107));point=((bounds[0]+bounds[2])//2,(bounds[1]+bounds[3])//2)
        self.move_mouse(point)
        require(self.user.WindowFromPoint(POINT(*point))==self.control(107),'Terminal wheel target is obscured')
        self.user.mouse_event(0x0800,0,0,c.c_uint32(direction*120).value,0)
        self.clicks+=1

    def compact_diagnostics(self,client,label):
        value=client.request('log.network_diagnostics')
        terminal=value['terminalPerformance']
        row={'elapsed_seconds':round(time.monotonic()-self.started,3),'label':label,
             'terminalPerformance':terminal,'networkMetrics':value.get('metrics',{})}
        self.diagnostics.append(row)
        return terminal

    def run_soak(self):
        require(self.user.GetForegroundWindow()==self.window,'Main window must be foreground')
        sys.path.insert(0,self.args.native_dir)
        from serialctl_ws import Client
        script=Path(self.args.native_dir)/'terminal_soak_producer.ps1'
        script.write_text(PRODUCER,encoding='utf-8-sig')
        reports=[Path(self.args.native_dir)/('terminal-soak-worker-%d.json'%worker) for worker in (1,2)]
        with Client('127.0.0.1',timeout=10) as client:
            before={r['id'] for r in client.resources()['resources'] if r['kind']=='cmd'}
            for expected in (1,2):
                self.hit_click(self.control(300))
                wait(lambda:len([r for r in client.resources()['resources'] if r['kind']=='cmd' and r['id'] not in before])==expected,10)
            sessions=sorted((r for r in client.resources()['resources'] if r['kind']=='cmd' and r['id'] not in before),
                            key=lambda row:int(row['id'].split('-')[1]))
            require(len(sessions)==2 and all(row['connected'] for row in sessions),'Two real CMD sessions were not established')
            for session in sessions:
                client.input(session['id'],b'chcp 65001\r\n')
            time.sleep(1)
            for worker,(session,report) in enumerate(zip(sessions,reports),1):
                quoted_script=str(script).replace("'","''");quoted_report=str(report).replace("'","''")
                command=("powershell -NoProfile -ExecutionPolicy Bypass -File '%s' -Report '%s' -Worker %d -SoakSeconds %d\r\n"%
                         (quoted_script,quoted_report,worker,self.args.soak_seconds)).encode('utf-8')
                client.input(session['id'],command)
            wait(lambda:all(path.exists() for path in reports),15)
            self.started=time.monotonic()
            self.prepare_full_window_drag()
            initial=self.sample_resources('soak-start')
            next_diagnostic=next_interaction=0.0
            interaction=0
            deadline=self.started+60+self.args.soak_seconds+180
            try:
                while True:
                    now=time.monotonic();elapsed=now-self.started
                    documents=[]
                    for path in reports:
                        try:documents.append(json.loads(path.read_text(encoding='utf-8-sig')))
                        except (OSError,ValueError):documents.append({})
                    if len(documents)==2 and all(row.get('status')=='PASS' for row in documents):break
                    require(now<deadline,'CMD producers did not complete within the throughput deadline')
                    require(not any(str(row.get('status','')).startswith('FAIL') for row in documents),'A CMD producer failed')
                    if elapsed>=next_diagnostic:
                        self.responses.append(self.responsive())
                        self.paint_markers.append(int(self.user.GetPropW(self.control(107),'SerialCtl.PaintProbe') or 0))
                        self.sample_resources('soak-%06d'%round(elapsed))
                        self.compact_diagnostics(client,'periodic')
                        next_diagnostic+=30
                    if elapsed>=next_interaction:
                        operation=interaction%6
                        if operation in (0,3):self.click_session_row(operation//3)
                        elif operation==1:
                            self.wheel_terminal(-1);time.sleep(.1);self.wheel_terminal(1)
                        elif operation==2:self.sidebar_toggle(observation_budget=.3,capture_rapid_frames=False)
                        elif operation==4:self.resize(1040 if interaction%12==4 else 1180,680 if interaction%12==4 else 760,'soak-resize')
                        else:self.open_cancel((100,101,102,103,303)[(interaction//6)%5])
                        self.responses.append(self.responsive())
                        interaction+=1;next_interaction+=60
                    time.sleep(.2)
                final_documents=[json.loads(path.read_text(encoding='utf-8-sig')) for path in reports]
                drain_deadline=time.monotonic()+60
                while True:
                    terminal=self.compact_diagnostics(client,'drain')
                    if (terminal['receiveQueuePendingBytes']==0 and terminal['parseQueuePendingBytes']==0 and
                            terminal['loggerBacklogBytes']==0):break
                    require(time.monotonic()<drain_deadline,'Terminal queues did not drain after producers stopped')
                    time.sleep(.25)
                final=self.sample_resources('soak-end')
                phases={name:[] for name in ('burst-5mib-aggregate','soak-1mib-aggregate')}
                for document in final_documents:
                    for phase in document['phases']:phases[phase['name']].append(phase)
                measured={}
                for name,rows in phases.items():
                    require(len(rows)==2,'Missing producer phase measurements')
                    bytes_written=sum(row['bytes'] for row in rows)
                    elapsed=max(row['elapsedSeconds'] for row in rows)
                    measured[name]={'bytes':bytes_written,'elapsed_seconds':elapsed,
                                    'actual_bytes_per_second':bytes_written/elapsed}
                require(measured['burst-5mib-aggregate']['actual_bytes_per_second']>=4.5*1024*1024,
                        'Actual aggregate 5 MiB/s burst throughput fell below 4.5 MiB/s')
                require(measured['soak-1mib-aggregate']['actual_bytes_per_second']>=.9*1024*1024,
                        'Actual aggregate 1 MiB/s soak throughput fell below 0.9 MiB/s')
                require(terminal['dropOverflowEvents']==0,'Terminal reported a data gap or overflow')
                require(terminal['receiveNotificationFailures']==0,'Receive notification delivery failed')
                require(max(self.responses)<1000 and percentile(self.responses,.99)<500,
                        'UI response latency exceeded soak acceptance threshold')
                require(final['gdi']-initial['gdi']<=256 and final['user']-initial['user']<=32,
                        'GDI or USER objects grew beyond bounded cache allowance')
                require(final['handles']-initial['handles']<=64,'Process handles grew beyond allowance')
                require(final['private_bytes']-initial['private_bytes']<=256*1024*1024,
                        'Private memory grew by more than 256 MiB during bounded-history soak')
                require(len(set(self.paint_markers))>10,'Paint probe did not advance during the soak')
                self.report.update({'status':'PASS','sessions':[row['id'] for row in sessions],
                    'producer_reports':final_documents,'measured_load':measured,
                    'response_samples_ms':self.responses,'response_p95_ms':percentile(self.responses,.95),
                    'response_p99_ms':percentile(self.responses,.99),'resource_samples':self.resources,
                    'diagnostic_samples':self.diagnostics,'paint_probe_samples':self.paint_markers,
                    'actual_click_count':self.clicks})
                self.hit_click(self.control(302));time.sleep(.5)
                require(not[r for r in client.resources()['resources'] if r['kind']=='cmd' and r['id'] in {x['id'] for x in sessions}],
                        'All-disconnect left soak CMD sessions')
                bounds=self.rect(self.window)
                close=next(((x,y) for x in range(bounds[2]-40,bounds[2]-5,3)
                            for y in range(bounds[1]+5,bounds[1]+28,3) if self.hit_test(x,y)==20),None)
                require(close,'Cannot locate native Close caption target')
                self.hit_click(self.window,close)
                require(self.kernel.WaitForSingleObject(self.process,10000)==0,'Native close failed after soak')
            finally:
                self.restore_full_window_drag()
                self.kernel.CloseHandle(self.process)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--window',type=int,required=True);parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--native-dir',required=True)
    parser.add_argument('--soak-seconds',type=int,default=1800)
    parser.add_argument('--prefix',default='terminal-soak');parser.add_argument('--mode',default='desktop')
    args=parser.parse_args();require(60<=args.soak_seconds<=3600,'Soak duration must be 60..3600 seconds')
    actions=TerminalSoak(args)
    try:actions.run_soak()
    except Exception as error:
        actions.report.update({'status':'FAIL','error':repr(error),'traceback':traceback.format_exc(),
            'response_samples_ms':actions.responses,'resource_samples':actions.resources,
            'diagnostic_samples':actions.diagnostics,'paint_probe_samples':actions.paint_markers})
        print(traceback.format_exc(),file=sys.stderr)
    finally:
        actions.save()
        (args.output/'terminal-soak-result.json').write_text(json.dumps(actions.report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({key:actions.report.get(key) for key in ('status','measured_load','response_p99_ms','error')}))
    return 0 if actions.report['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
