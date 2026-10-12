[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.Drawing
# Test-only managed harness. The packaged product and updater remain native C++.
Add-Type @'
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;
public sealed class SerialCtlUpdateHttp : IDisposable {
    readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
    readonly ManualResetEvent stopped = new ManualResetEvent(false);
    readonly ConcurrentQueue<string> requests = new ConcurrentQueue<string>();
    readonly ConcurrentBag<TcpClient> clients = new ConcurrentBag<TcpClient>();
    readonly Thread accept;
    public int Port { get; private set; }
    public string[] Requests { get { return requests.ToArray(); } }
    public SerialCtlUpdateHttp() {
        listener.Start(); Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        accept = new Thread(Accept); accept.IsBackground = true; accept.Start();
    }
    void Accept() {
        while (!stopped.WaitOne(0)) {
            try { var c = listener.AcceptTcpClient(); clients.Add(c);
                var t = new Thread(() => Serve(c)); t.IsBackground = true; t.Start(); }
            catch (SocketException) { break; }
            catch (ObjectDisposedException) { break; }
        }
    }
    void Serve(TcpClient client) {
        try { using(client) {
            client.ReceiveTimeout = 5000; client.SendTimeout = 2000;
            var stream = client.GetStream(); var bytes = new List<byte>();
            while (bytes.Count < 8192) {
                int value = stream.ReadByte(); if(value < 0) return; bytes.Add((byte)value);
                int n=bytes.Count; if(n>=4 && bytes[n-4]==13 && bytes[n-3]==10 && bytes[n-2]==13 && bytes[n-1]==10) break;
            }
            string head=Encoding.ASCII.GetString(bytes.ToArray());
            string first=head.Split(new[]{"\r\n"},StringSplitOptions.None)[0];
            string[] words=first.Split(' '); if(words.Length<2) return;
            string path=words[1]; requests.Enqueue(DateTime.UtcNow.ToString("o")+" "+first);
            if(path=="/slowheaders/update.json") { stopped.WaitOne(30000); return; }
            if(path=="/slowbody/update.json") {
                Write(stream, Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Length: 64000\r\nConnection: close\r\n\r\n"));
                while(!stopped.WaitOne(100)) Write(stream, new byte[]{32}); return;
            }
            byte[] body; string status;
            if(path=="/b/update.json") { status="200 OK"; body=Encoding.UTF8.GetBytes("{\"schema\":1,\"product\":\"SerialCtl\",\"architecture\":\"x64\",\"version\":\"V99.0.0\",\"files\":[]}"); }
            else if(path=="/b/update.json.sig") { status="200 OK"; body=new byte[256]; }
            else { status="404 Not Found"; body=Encoding.ASCII.GetBytes("controlled missing update feed"); }
            Write(stream, Encoding.ASCII.GetBytes("HTTP/1.1 "+status+"\r\nContent-Length: "+body.Length+"\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n"));
            Write(stream,body);
        }} catch(System.IO.IOException) {} catch(SocketException) {} catch(ObjectDisposedException) {}
    }
    static void Write(NetworkStream stream, byte[] data) { stream.Write(data,0,data.Length); stream.Flush(); }
    public void Dispose() { stopped.Set(); listener.Stop(); foreach(var client in clients) client.Close(); accept.Join(2000); }
}
public static class SerialCtlUpdateUi {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx,dy; public uint mouseData,dwFlags,time; public UIntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort vk,scan; public uint flags,time; public UIntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct UNION { [FieldOffset(0)] public MOUSEINPUT mouse; [FieldOffset(0)] public KEYBDINPUT key; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public UNION data; }
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr window,int x,int y,int w,int h,bool paint);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageTimeoutW")] static extern IntPtr ReadText(IntPtr window,uint message,IntPtr w,StringBuilder text,uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr window,uint message,IntPtr w,IntPtr l,uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs,int size);
    public static string Text(IntPtr window) { var text=new StringBuilder(4096); IntPtr result; if(ReadText(window,13,new IntPtr(text.Capacity),text,2,1000,out result)==IntPtr.Zero) throw new Exception("Cannot read GUI control text"); return text.ToString(); }
    public static bool Responsive(IntPtr window) { IntPtr result; return SendMessageTimeout(window,0,IntPtr.Zero,IntPtr.Zero,2,1000,out result)!=IntPtr.Zero; }
    public static void Click(IntPtr window) {
        RECT r; if(window==IntPtr.Zero || !GetWindowRect(window,out r)) throw new Exception("Missing clickable control");
        if(!SetCursorPos((r.Left+r.Right)/2,(r.Top+r.Bottom)/2)) throw new Exception("Cannot position real mouse");
        mouse_event(2,0,0,0,UIntPtr.Zero); Thread.Sleep(30); mouse_event(4,0,0,0,UIntPtr.Zero);
    }
    public static void Type(string text) {
        foreach(char c in text) {
            var down=new INPUT(); down.type=1; down.data.key.scan=c; down.data.key.flags=4;
            var up=down; up.data.key.flags=6;
            if(SendInput(2,new[]{down,up},Marshal.SizeOf(typeof(INPUT)))!=2) throw new Exception("Keyboard SendInput failed");
        }
        var enter=new INPUT(); enter.type=1; enter.data.key.vk=13;
        var release=enter; release.data.key.flags=2;
        if(SendInput(2,new[]{enter,release},Marshal.SizeOf(typeof(INPUT)))!=2) throw new Exception("Enter SendInput failed");
    }
}
'@
function Wait-UpdateCondition([scriptblock]$Condition, [string]$Description, [int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 30 } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Timed out: $Description"
}
function Save-UpdateDesktop([string]$Name) {
    $size = [Drawing.Size]::new([SerialCtlUpdateUi]::GetSystemMetrics(0), [SerialCtlUpdateUi]::GetSystemMetrics(1))
    $bitmap = [Drawing.Bitmap]::new($size.Width, $size.Height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try { $graphics.CopyFromScreen(0,0,0,0,$size); $bitmap.Save((Join-Path $OutputDirectory ($Name+'.png')), [Drawing.Imaging.ImageFormat]::Png) }
    finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Use-UpdateTheme([IntPtr]$Window, [string]$Name) {
    $control = [SerialCtlUpdateUi]::GetDlgItem($Window,104)
    $before = [SerialCtlUpdateUi]::Text($control)
    [SerialCtlUpdateUi]::Click($control)
    Wait-UpdateCondition { [SerialCtlUpdateUi]::Text($control) -ne $before } 'actual mouse theme toggle' 2000
    Save-UpdateDesktop ($Name+'-theme')
    return [ordered]@{before=$before; after=[SerialCtlUpdateUi]::Text($control); input='physical mouse via mouse_event'}
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$root = Get-SerialCtlRepositoryRoot
$archive = Join-Path $root ('bin/' + (Get-SerialCtlArchiveName))
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('serialctl-updater-gui-'+[Guid]::NewGuid().ToString('N'))
$install = Join-Path $temporary 'app'
New-Item -ItemType Directory -Path $install -Force | Out-Null
# Only immutable executables and VERSION are copied. No key or source enters this fixture.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($name in @('serialctl.exe','plink.exe','psftp.exe','VERSION')) {
        $entry = $zip.GetEntry($name)
        if ($null -eq $entry) { throw "Package is missing $name" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $install $name),$false)
    }
} finally { $zip.Dispose() }
$beforeHashes = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $install -File) { $beforeHashes[$file.Name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
$server = [SerialCtlUpdateHttp]::new()
$application = $null
$results = [Collections.Generic.List[object]]::new()
$summary = [ordered]@{
    system=[Environment]::OSVersion.VersionString; source=(Get-SerialCtlSourceState).Revision
    packageSha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    exeSha256=$beforeHashes['serialctl.exe']; httpOrigin="http://127.0.0.1:$($server.Port)"
    capture='Actual desktop CopyFromScreen; physical mouse and SendInput keyboard; no PrintWindow'
    scope='Production packaged GUI negative update integration; HTTP layer is a loopback substitute. No production signing key, successful production upgrade, Win7, real intranet server or hardware is tested here.'
    noConfigScope='No requests to controlled HTTP fixture and no updater staging for missing/empty URL. This is not a machine-wide packet capture.'
    cases=$results; result='RUNNING'
}
try {
    foreach ($case in @('missing','empty','url-a-404','url-b-invalid-signature','slowheaders','slowbody')) {
        $record = [ordered]@{name=$case; result='RUNNING'}
        $results.Add($record)
        $config = Join-Path $install 'update.ini'
        if (Test-Path -LiteralPath $config) { Remove-Item -LiteralPath $config -Force }
        $route = switch ($case) {
            'url-a-404' {'/a/update.json'}
            'url-b-invalid-signature' {'/b/update.json'}
            'slowheaders' {'/slowheaders/update.json'}
            'slowbody' {'/slowbody/update.json'}
            default {''}
        }
        if ($case -ne 'missing') {
            $url = if ($route) { "http://127.0.0.1:$($server.Port)$route" } else { '' }
            [IO.File]::WriteAllText($config,"[update]`r`nurl=$url`r`n",[Text.Encoding]::ASCII)
            $record['configuredUrl']=$url
            $record['iniSha256Before']=(Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
        }
        $firstRequest = $server.Requests.Length
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $application = Start-Process -FilePath (Join-Path $install 'serialctl.exe') -WorkingDirectory $install -PassThru
        Wait-UpdateCondition { $application.Refresh(); if($application.HasExited) { throw 'Production GUI exited before creating its window' }; $application.MainWindowHandle -ne [IntPtr]::Zero } 'production GUI main window'
        $window = $application.MainWindowHandle
        $record['startupMs']=$clock.ElapsedMilliseconds
        $record['processId']=$application.Id
        [SerialCtlUpdateUi]::MoveWindow($window,10,10,1100,720,$true) | Out-Null
        [SerialCtlUpdateUi]::SetForegroundWindow($window) | Out-Null
        Wait-UpdateCondition { [SerialCtlUpdateUi]::GetForegroundWindow() -eq $window } 'production GUI foreground' 2000
        if (-not [SerialCtlUpdateUi]::Responsive($window)) { throw 'Main window is unresponsive' }
        if ($route) {
            Wait-UpdateCondition { @($server.Requests | Where-Object { $_ -like "*GET $route HTTP/*" }).Count -ge 1 } "HTTP request for $route"
        }
        if ($case -eq 'url-a-404' -or $case -eq 'url-b-invalid-signature') {
            $expected = if ($case -eq 'url-a-404') {'HTTP 200'} else {'signature is invalid'}
            $status = [SerialCtlUpdateUi]::GetDlgItem($window,111)
            Wait-UpdateCondition { [SerialCtlUpdateUi]::Text($status).Contains($expected) } "visible updater error: $expected"
            $record['visibleError']=[SerialCtlUpdateUi]::Text($status)
        }
        if ($case -eq 'missing' -or $case -eq 'empty') {
            Start-Sleep -Milliseconds 600
            if ($server.Requests.Length -ne $firstRequest) { throw 'Unconfigured updater contacted the HTTP fixture' }
            if (Test-Path -LiteralPath (Join-Path $install '.serialctl-update')) { throw 'Unconfigured updater created staging' }
        }
        Save-UpdateDesktop ('updater-'+$case+'-before')
        $record['theme']=Use-UpdateTheme $window ('updater-'+$case)
        if ($case -eq 'url-a-404' -or $case -eq 'slowbody') {
            # Start real CMD with the visible toolbar, then type through the actual focused terminal.
            [SerialCtlUpdateUi]::Click([SerialCtlUpdateUi]::GetDlgItem($window,300))
            $status = [SerialCtlUpdateUi]::GetDlgItem($window,111)
            Wait-UpdateCondition { [SerialCtlUpdateUi]::Text($status).Contains('CMD') -and -not [SerialCtlUpdateUi]::Text($status).Contains('正在连接') } 'real CMD connected'
            $terminal = [SerialCtlUpdateUi]::GetDlgItem($window,107)
            [SerialCtlUpdateUi]::Click($terminal)
            $proof = Join-Path $install ('cmd-proof-'+$case+'.txt')
            $marker = 'SERIALCTL_UPDATE_GUI_'+$case.Replace('-','_')
            [SerialCtlUpdateUi]::Type(('echo '+$marker+'>"'+$proof+'"'))
            Wait-UpdateCondition { (Test-Path -LiteralPath $proof) -and ([IO.File]::ReadAllText($proof).Trim() -eq $marker) } 'actual CMD execution output'
            $record['cmdProof']=[IO.File]::ReadAllText($proof).Trim()
            Copy-Item -LiteralPath $proof -Destination (Join-Path $OutputDirectory ('updater-'+$case+'-cmd.txt'))
            Save-UpdateDesktop ('updater-'+$case+'-cmd')
        }
        if (-not [SerialCtlUpdateUi]::Responsive($window)) { throw 'GUI stopped responding during update fixture' }
        $record['beforeCloseMs']=$clock.ElapsedMilliseconds
        $record['threadsBeforeClose']=$application.Threads.Count
        $record['handlesBeforeClose']=$application.HandleCount
        $close = [Diagnostics.Stopwatch]::StartNew()
        if (-not [SerialCtlUpdateUi]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)) { throw 'WM_CLOSE could not be delivered' }
        if (-not $application.WaitForExit(5000)) { throw 'Normal close exceeded 5000 ms with pending update work' }
        $record['normalCloseMs']=$close.ElapsedMilliseconds
        $record['exitCode']=$application.ExitCode
        if ($application.ExitCode -ne 0) { throw 'Production GUI exited abnormally' }
        if ($case -ne 'missing') {
            $record['iniSha256After']=(Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash
            if ($record['iniSha256After'] -ne $record['iniSha256Before']) { throw 'Updater changed editable URL configuration' }
        }
        foreach ($name in $beforeHashes.Keys) {
            if ((Get-FileHash -LiteralPath (Join-Path $install $name) -Algorithm SHA256).Hash -ne $beforeHashes[$name]) { throw "Negative update case changed $name" }
        }
        $record['immutableFilesUnchanged']=$true
        $record['httpRequests']=@($server.Requests | Select-Object -Skip $firstRequest)
        if ($case -eq 'url-b-invalid-signature' -and @($record['httpRequests'] | Where-Object { $_ -like '*GET /b/update.json.sig HTTP/*' }).Count -ne 1) { throw 'Invalid-signature case did not fetch the signature' }
        if (@($record['httpRequests'] | Where-Object { $_ -like '*GET */files/*' }).Count -ne 0) { throw 'Unverified manifest downloaded payload files' }
        $record['result']='PASS'
        $application.Dispose(); $application=$null
        $summary | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'updater-gui.json') -Encoding UTF8
        Write-Host "[PASS] updater production GUI $case"
    }
    $summary['result']='PASS'
} catch {
    $summary['result']='FAIL'; $summary['error']=$_.Exception.ToString()
    if ($results.Count -gt 0) { $results[$results.Count-1]['result']='FAIL' }
    try { Save-UpdateDesktop 'updater-failure-desktop' } catch { }
    throw
} finally {
    if ($null -ne $application) {
        if (-not $application.HasExited) { $application.Kill(); $application.WaitForExit(5000) | Out-Null }
        $application.Dispose()
    }
    $summary['httpRequests']=$server.Requests
    $summary | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'updater-gui.json') -Encoding UTF8
    $server.Requests | Set-Content -LiteralPath (Join-Path $OutputDirectory 'updater-http.log') -Encoding UTF8
    $server.Dispose()
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
Write-Host '[NOT RUN] Production-signed successful upgrade, real Win7, real intranet server and hardware; see separate native installer/client tests and manual acceptance.'
