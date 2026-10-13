[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory, [switch]$ActualMouse)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$behaviorFailures = [System.Collections.Generic.List[string]]::new()
. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.Drawing
$env:SERIALCTL_UI_METRICS = '1'
$env:PYTHONIOENCODING = 'utf-8'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SerialCtlUiSmoke {
    public delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumDisplaySettings(string device, int mode, [In,Out] byte[] settings);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int ChangeDisplaySettings([In,Out] byte[] settings, int flags);
    [DllImport("gdi32.dll")] public static extern int GetDeviceCaps(IntPtr dc,int index);
    public static string PrepareDesktop() {
        var mode=new byte[220]; BitConverter.GetBytes((ushort)220).CopyTo(mode,68);
        if(!EnumDisplaySettings(null,-1,mode)) return "Current mode unavailable";
        foreach(var candidate in new [] {new [] {1920,1080},new [] {1280,1024}}) {
            BitConverter.GetBytes(0x00180000u).CopyTo(mode,72);
            BitConverter.GetBytes(candidate[0]).CopyTo(mode,172);
            BitConverter.GetBytes(candidate[1]).CopyTo(mode,176);
            int result=ChangeDisplaySettings(mode,0);
            if(result==0) return "Requested "+candidate[0]+"x"+candidate[1]+" accepted";
        }
        return "Display driver retained current resolution";
    }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className,string caption);
    [DllImport("user32.dll")] public static extern bool GetMenuItemRect(IntPtr window,IntPtr menu,uint item,out RECT rect);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window,int command);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr window,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetPropW")] public static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SetText(IntPtr window, uint message, IntPtr wParam, string text);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr ReadText(IntPtr window, uint message, IntPtr wParam, StringBuilder text);
    public static void Fill(IntPtr dialog, int id, string text) {
        IntPtr edit = GetDlgItem(dialog, id);
        if (edit == IntPtr.Zero) throw new Exception("Missing control " + id);
        SetText(edit, 0x000C, IntPtr.Zero, text);
        var actual = new StringBuilder(256);
        ReadText(edit, 0x000D, new IntPtr(actual.Capacity), actual);
        if (actual.ToString() != text) throw new Exception("Control text mismatch " + id + ": " + actual);
    }
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern int MapWindowPoints(IntPtr from, IntPtr to, ref RECT rect, uint points);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr window, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr window, EnumProc proc, IntPtr parameter);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [StructLayout(LayoutKind.Sequential)] public struct SIZE { public int Width, Height; }
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] public static extern bool GetTextExtentPoint32(IntPtr dc, string text, int length, out SIZE extent);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] public static extern int GetTextFace(IntPtr dc, int count, StringBuilder face);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr CreateFont(int height, int width, int escapement, int orientation, int weight, uint italic, uint underline, uint strikeout, uint charset, uint output, uint clip, uint quality, uint pitch, string face);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    public static void AssertLabels(IntPtr dialog, string expectedFace) {
        // HFONT handles are process-local. Measure the design font in this process,
        // rather than silently selecting the stock System font with a foreign handle.
        IntPtr font = CreateFont(-15, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 32, expectedFace);
        string failure = null;
        EnumChildWindows(dialog, delegate(IntPtr child, IntPtr unused) {
            var name = new StringBuilder(64); GetClassName(child, name, name.Capacity);
            if (!IsWindowVisible(child) || name.ToString() != "Static") return true;
            var text = new StringBuilder(1024); ReadText(child, 0xD, new IntPtr(text.Capacity), text);
            if (text.Length == 0) return true;
            IntPtr dc = GetDC(child);
            IntPtr previous = SelectObject(dc, font);
            if (previous == IntPtr.Zero) throw new Exception("Cannot select design font");
            SIZE extent; GetTextExtentPoint32(dc, text.ToString(), text.Length, out extent);
            var face = new StringBuilder(128); GetTextFace(dc, face.Capacity, face);
            RECT bounds; GetClientRect(child, out bounds);
            SelectObject(dc, previous); ReleaseDC(child, dc);
            Console.WriteLine("Label {0}: design-font={1}, text-height={2}, bounds-height={3}", text, face, extent.Height, bounds.Bottom);
            if (extent.Height > bounds.Bottom || extent.Width > bounds.Right) failure = "Clipped label: " + text;
            return failure == null;
        }, IntPtr.Zero);
        DeleteObject(font);
        if (failure != null) throw new Exception(failure);
    }
    public static void AssertPanel(IntPtr window, bool collapsed) {
        for (int id=108; id<=110; id++) if (GetDlgItem(window,id) != IntPtr.Zero) throw new Exception("Obsolete composer control " + id);
        RECT client; GetClientRect(window, out client);
        MapWindowPoints(window, IntPtr.Zero, ref client, 2);
        RECT toggle; GetWindowRect(GetDlgItem(window, 125), out toggle);
        if (toggle.Right > client.Right - 12) throw new Exception("Panel toggle exceeds card edge");
        if (collapsed) {
            if (toggle.Left < client.Right - 60 || toggle.Right > client.Right - 12) throw new Exception("Toggle exceeds collapsed rail");
            for (int id = 112; id <= 132; id++) if (id != 125 && IsWindowVisible(GetDlgItem(window, id))) throw new Exception("Visible collapsed control " + id);
        } else {
            RECT tab, add;
            GetWindowRect(GetDlgItem(window, 118), out tab);
            GetWindowRect(GetDlgItem(window, 115), out add);
            if (IsWindowVisible(GetDlgItem(window, 115)) && (tab.Right + 8 > add.Left || add.Right + 8 > toggle.Left)) throw new Exception("Overlapping panel header actions");
        }
        string failure = null;
        EnumChildWindows(window, delegate(IntPtr child, IntPtr unused) {
            if (!IsWindowVisible(child)) return true;
            RECT r; GetWindowRect(child, out r);
            if (r.Left < client.Left || r.Right > client.Right || r.Top < client.Top || r.Bottom > client.Bottom) failure = "Child outside main window: " + GetDlgCtrlID(child);
            return failure == null;
        }, IntPtr.Zero);
        if (failure != null) throw new Exception(failure);
    }
    public static void AssertPowerBounds(IntPtr pane, int id, int x, int y, int width, int height) {
        RECT rect; GetWindowRect(GetDlgItem(pane,id),out rect); MapWindowPoints(IntPtr.Zero,pane,ref rect,2);
        if(Math.Abs(rect.Left-x)>2 || Math.Abs(rect.Top-y)>2 || Math.Abs(rect.Right-rect.Left-width)>2 || Math.Abs(rect.Bottom-rect.Top-height)>2)
            throw new Exception("Power geometry mismatch id="+id+" actual="+rect.Left+","+rect.Top+","+(rect.Right-rect.Left)+","+(rect.Bottom-rect.Top)+" expected="+x+","+y+","+width+","+height);
    }
    public static IntPtr Dialog(uint processId, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr ignored) {
            uint pid; GetWindowThreadProcessId(window, out pid);
            var name = new StringBuilder(64); GetClassName(window, name, name.Capacity);
            var caption = new StringBuilder(256); GetWindowText(window, caption, caption.Capacity);
            if (pid == processId && IsWindowVisible(window) && name.ToString() == "#32770" && (String.IsNullOrEmpty(title) || caption.ToString() == title)) { found = window; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
function Click-Control([IntPtr]$Parent, [int]$Id) {
    if (-not $ActualMouse) {
        # Preserve the existing fast regression mode; the separate interaction
        # audit opts into real Windows input and never takes this branch.
        [SerialCtlUiSmoke]::PostMessage($Parent, 0x111, [IntPtr]$Id, [IntPtr]::Zero) | Out-Null
        return
    }
    $control = [SerialCtlUiSmoke]::GetDlgItem($Parent, $Id)
    if ($control -eq [IntPtr]::Zero -or -not [SerialCtlUiSmoke]::IsWindowVisible($control) -or -not [SerialCtlUiSmoke]::IsWindowEnabled($control)) {
        throw "No visible enabled control for real mouse click: $Id"
    }
    $rootWindow = [SerialCtlUiSmoke]::GetAncestor($control, 2)
    if ([SerialCtlUiSmoke]::GetForegroundWindow() -ne $rootWindow) {
        [SerialCtlUiSmoke]::SetForegroundWindow($rootWindow) | Out-Null
        Start-Sleep -Milliseconds 60
    }
    $rect = New-Object SerialCtlUiSmoke+RECT
    if (-not [SerialCtlUiSmoke]::GetWindowRect($control, [ref]$rect)) { throw "Missing click bounds: $Id" }
    $point = New-Object SerialCtlUiSmoke+POINT
    $point.X = [int](($rect.Left + $rect.Right) / 2)
    $point.Y = [int](($rect.Top + $rect.Bottom) / 2)
    $hit = [SerialCtlUiSmoke]::WindowFromPoint($point)
    if ($hit -ne $control -and -not [SerialCtlUiSmoke]::IsChild($control, $hit)) { throw "Control $Id is covered; no synthetic command fallback is allowed." }
    if (-not [SerialCtlUiSmoke]::SetCursorPos($point.X, $point.Y)) { throw 'Cannot position system mouse.' }
    $before = [SerialCtlUiSmoke]::GetForegroundWindow().ToInt64()
    [SerialCtlUiSmoke]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 25
    [SerialCtlUiSmoke]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    [ordered]@{ utc=[DateTime]::UtcNow.ToString('o'); parent=$Parent.ToInt64(); control=$control.ToInt64(); id=$Id; x=$point.X; y=$point.Y; foregroundBefore=$before; input='system mouse_event down/up; target hit tested; behavior is not WM_COMMAND' } |
        ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'actual-clicks.jsonl') -Encoding UTF8
}
function Capture-Desktop([string]$Name) {
    $size=[Drawing.Size]::new([SerialCtlUiSmoke]::GetSystemMetrics(0),[SerialCtlUiSmoke]::GetSystemMetrics(1))
    $screen = New-Object Drawing.Bitmap($size.Width,$size.Height)
    $graphics = [Drawing.Graphics]::FromImage($screen)
    try { $graphics.CopyFromScreen(0,0,0,0,$size); $screen.Save((Join-Path $OutputDirectory ($Name+'.png')),[Drawing.Imaging.ImageFormat]::Png) }
    finally { $graphics.Dispose(); $screen.Dispose() }
}
function Wait-Dialog([int]$ProcessId, [string]$Title) {
    for ($attempt = 0; $attempt -lt 150; $attempt++) {
        $dialog = [SerialCtlUiSmoke]::Dialog([uint32]$ProcessId, $Title)
        if ($dialog -ne [IntPtr]::Zero) { return $dialog }
        Start-Sleep -Milliseconds 100
    }
    throw "The expected dialog did not appear: $Title"
}
function Capture-Window([IntPtr]$Handle, [string]$Name) {
    $rect = New-Object SerialCtlUiSmoke+RECT
    if (-not [SerialCtlUiSmoke]::GetWindowRect($Handle, [ref]$rect)) { throw 'No window bounds.' }
    $bitmap = New-Object System.Drawing.Bitmap(($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top))
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [SerialCtlUiSmoke]::PrintWindow($Handle, $dc, 0)) { throw 'PrintWindow failed.' }
    } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try { $bitmap.Save((Join-Path $OutputDirectory "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$displayResult=[SerialCtlUiSmoke]::PrepareDesktop()
Start-Sleep -Milliseconds 300
$screenDc=[SerialCtlUiSmoke]::GetDC([IntPtr]::Zero)
try {$dpi=[SerialCtlUiSmoke]::GetDeviceCaps($screenDc,88)} finally {[SerialCtlUiSmoke]::ReleaseDC([IntPtr]::Zero,$screenDc)|Out-Null}
$cpuInfo = Get-CimInstance Win32_Processor
$computerInfo = Get-CimInstance Win32_ComputerSystem
[ordered]@{system=[Environment]::OSVersion.VersionString; cpu=@($cpuInfo | ForEach-Object {$_.Name}); logicalProcessors=[Environment]::ProcessorCount; memoryBytes=[int64]$computerInfo.TotalPhysicalMemory; dpi=$dpi; width=[SerialCtlUiSmoke]::GetSystemMetrics(0);height=[SerialCtlUiSmoke]::GetSystemMetrics(1);resolution=$displayResult;capture='CopyFromScreen actual physical desktop; PrintWindow separate layout inspection';hardware='No physical UART/IT6332A; SFTP mock isolated'} | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'ui-environment.json') -Encoding UTF8
$root = Get-SerialCtlRepositoryRoot
$version = Get-SerialCtlVersion
$extract = Join-Path $OutputDirectory 'payload'
Expand-Archive -LiteralPath (Join-Path $root ('bin/' + (Get-SerialCtlArchiveName))) -DestinationPath $extract -Force
if ($ActualMouse) {
    # Record the exact untouched program before any fixture or final cleanup.
    @('serialctl.exe','plink.exe','psftp.exe') | ForEach-Object { Get-FileHash (Join-Path $extract $_) -Algorithm SHA256 } |
        Select-Object Path,Hash | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'program-executable-sha256.json') -Encoding UTF8
}
# This mock is confined to UI evidence and is never placed in the release archive.
$mock = Join-Path $OutputDirectory 'mock-plink.exe'
Add-Type -OutputAssembly $mock -OutputType ConsoleApplication -TypeDefinition @'
using System;
public static class FakePlink {
    public static int Main(string[] args) {
        foreach (string arg in args) if (arg == "-b") {
            Console.WriteLine("Remote directory is /home/test\nListing directory /home/test\n-rw-r--r-- 1 test test 12 Jan 01 2026 test.txt");
            return 0;
        }
        foreach (string arg in args) if (arg == "-hostkey") {
            var producer = new System.Threading.Thread(() => {
                for (int i=0;;i++) {
                    Console.WriteLine("SERIALCTL_PAINT_" + (i%1000).ToString("D3") + " controlled SSH output during SFTP layout");
                    System.Threading.Thread.Sleep(10);
                }
            });
            producer.IsBackground = true;
            producer.Start();
            while (Console.ReadLine() != null) {}
            return 0;
        }
        Console.WriteLine("The host key is not cached for this server\nkey fingerprint is:\nssh-ed25519 255 SHA256:ui-smoke-test");
        return 1;
    }
}
'@
$installedFonts = New-Object System.Drawing.Text.InstalledFontCollection
$expectedUiFace = 'Segoe UI'
foreach ($candidate in @('Microsoft YaHei UI','Microsoft YaHei','SimSun')) {
    if ($installedFonts.Families.Name -contains $candidate) { $expectedUiFace = $candidate; break }
}
$installedFonts.Dispose()
$settingsDirectory = Join-Path $env:APPDATA 'SerialCtl'
New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
$titles = @(([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu6IFNTSCDov57mjqU='))), ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu65Liy5Y+j6L+e5o6l'))), ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu6IFRlbG5ldCDov57mjqU='))), ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('6L+e5o6l5YWx5Lqr5Liy5Y+j'))))
foreach ($architecture in @('x64')) {
    $native = $extract
    # First launch the exact packaged program with its exact packaged dependencies.
    Set-Content -LiteralPath (Join-Path $settingsDirectory 'settings.ini') -Value "[Layout]`nRightPanelWidth=260`nRightPanelCollapsed=0" -Encoding ASCII
    $application = Start-Process -FilePath (Join-Path $native 'serialctl.exe') -PassThru
    try {
        if (-not $application.WaitForInputIdle(10000)) { throw 'Packaged application did not become idle.' }
        $application.Refresh()
        for ($attempt=0; $attempt -lt 50 -and -not $application.HasExited -and $application.MainWindowHandle -eq 0; $attempt++) {
            Start-Sleep -Milliseconds 100
            $application.Refresh()
        }
        if ($application.HasExited -or $application.MainWindowHandle -eq 0) {
            $diagnostic = "Packaged application startup failed; exited=$($application.HasExited)"
            if ($application.HasExited) { $diagnostic += "; exit=$($application.ExitCode)" }
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'startup.txt') -Value $diagnostic
            throw $diagnostic
        }
        foreach ($theme in @('light','dark')) {
            if ($theme -eq 'dark') {
                Click-Control $application.MainWindowHandle 104
                Start-Sleep -Milliseconds 300
            }
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle, 0, 0, 980, 620, $true) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $false)
            Capture-Window $application.MainWindowHandle "$architecture-packaged-$theme"
            Click-Control $application.MainWindowHandle 125
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $true)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-collapsed"
            Click-Control $application.MainWindowHandle 125
            Start-Sleep -Milliseconds 200
            foreach ($mode in 0..3) {
                Click-Control $application.MainWindowHandle (100+$mode)
                $dialog = Wait-Dialog $application.Id $titles[$mode]
                Start-Sleep -Milliseconds 300
                [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
                Capture-Window $dialog "$architecture-$theme-connection-$mode"
                if ($mode -eq 3) {
                    if ($ActualMouse) {
                        # 9010 is a context-menu command, not a child button.
                        # Open its real user entry on the Refresh button.
                        $refresh = [SerialCtlUiSmoke]::GetDlgItem($dialog,1024)
                        $refreshRect = New-Object SerialCtlUiSmoke+RECT
                        if (-not [SerialCtlUiSmoke]::GetWindowRect($refresh,[ref]$refreshRect)) { throw 'Missing remote refresh control.' }
                        [SerialCtlUiSmoke]::SetCursorPos([int](($refreshRect.Left+$refreshRect.Right)/2),[int](($refreshRect.Top+$refreshRect.Bottom)/2)) | Out-Null
                        [SerialCtlUiSmoke]::mouse_event(8,0,0,0,[UIntPtr]::Zero)
                        Start-Sleep -Milliseconds 25
                        [SerialCtlUiSmoke]::mouse_event(16,0,0,0,[UIntPtr]::Zero)
                        Start-Sleep -Milliseconds 100
                        Capture-Desktop "$architecture-$theme-shared-port-menu"
                        $popup = [SerialCtlUiSmoke]::FindWindow('#32768',$null)
                        if ($popup -eq [IntPtr]::Zero) { throw 'Actual context menu did not open.' }
                        $menu = [SerialCtlUiSmoke]::Send($popup,0x01E1,[IntPtr]::Zero,[IntPtr]::Zero) # MN_GETHMENU: read only.
                        $itemRect = New-Object SerialCtlUiSmoke+RECT
                        if (-not [SerialCtlUiSmoke]::GetMenuItemRect($dialog,$menu,0,[ref]$itemRect)) { throw 'Cannot locate first actual context-menu item.' }
                        [SerialCtlUiSmoke]::SetCursorPos([int](($itemRect.Left+$itemRect.Right)/2),[int](($itemRect.Top+$itemRect.Bottom)/2)) | Out-Null
                        [SerialCtlUiSmoke]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
                        Start-Sleep -Milliseconds 25
                        [SerialCtlUiSmoke]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
                        [ordered]@{utc=[DateTime]::UtcNow.ToString('o');id=1024;action='right click Refresh then click first actual menu item';targetCommand=9010;itemBounds=@($itemRect.Left,$itemRect.Top,$itemRect.Right,$itemRect.Bottom)} | ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'actual-clicks.jsonl') -Encoding UTF8
                    } else { Click-Control $dialog 9010 }
                    $portDialog = Wait-Dialog $application.Id '高级共享端口'
                    [SerialCtlUiSmoke]::Fill($portDialog, 1402, '7000')
                    [SerialCtlUiSmoke]::AssertLabels($portDialog, $expectedUiFace)
                    Capture-Window $portDialog "$architecture-$theme-shared-port"
                    Click-Control $portDialog 2
                    Start-Sleep -Milliseconds 200
                }
                Click-Control $dialog 2
                Start-Sleep -Milliseconds 200
            }
            # New power pane occupies the central terminal area. It keeps independent connection/output actions.
            Click-Control $application.MainWindowHandle 301
            Start-Sleep -Milliseconds 250
            $script:powerPane = [IntPtr]::Zero
            [SerialCtlUiSmoke]::EnumChildWindows($application.MainWindowHandle, {
                param($child,$unused)
                $name = New-Object Text.StringBuilder(64)
                [SerialCtlUiSmoke]::GetClassName($child,$name,64) | Out-Null
                if ($name.ToString() -eq 'SerialCtlPowerPane') { $script:powerPane=$child }
                return $true
            },[IntPtr]::Zero) | Out-Null
            if ($powerPane -eq [IntPtr]::Zero -or -not [SerialCtlUiSmoke]::IsWindowVisible($powerPane)) { throw 'Power pane missing.' }
            Capture-Window $application.MainWindowHandle "$architecture-$theme-power-disconnected"
            Click-Control $powerPane 10
            $dialog = Wait-Dialog $application.Id '连接电源'
            Start-Sleep -Milliseconds 200
            Capture-Window $dialog "$architecture-$theme-power-usb"
            [SerialCtlUiSmoke]::Send([SerialCtlUiSmoke]::GetDlgItem($dialog,100),0x14E,[IntPtr]1,[IntPtr]0) | Out-Null
            [SerialCtlUiSmoke]::PostMessage($dialog,0x111,[IntPtr]((1 -shl 16) -bor 100),[IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 200
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-power-serial"
            $methodCount = [SerialCtlUiSmoke]::Send([SerialCtlUiSmoke]::GetDlgItem($dialog,100),0x146,[IntPtr]0,[IntPtr]0).ToInt32()
            if ($methodCount -ne 2) { throw 'Power connection must expose only USB and RS232.' }
            Click-Control $dialog 2
            Start-Sleep -Milliseconds 250
            if ([SerialCtlUiSmoke]::IsIconic($application.MainWindowHandle)) { throw 'Main window minimized after power modal.' }
            foreach ($control in @(112,118,119,115)) {
                if ([SerialCtlUiSmoke]::IsWindowVisible([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,$control))) { throw 'Power page exposed command/SFTP sidebar.' }
            }
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,1180,760,$true) | Out-Null
            Start-Sleep -Milliseconds 250
            Capture-Window $application.MainWindowHandle "$architecture-$theme-power-layout"
            $ch1=New-Object SerialCtlUiSmoke+RECT; $ch2=New-Object SerialCtlUiSmoke+RECT; $ch3=New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetWindowRect([SerialCtlUiSmoke]::GetDlgItem($powerPane,20),[ref]$ch1)|Out-Null
            [SerialCtlUiSmoke]::GetWindowRect([SerialCtlUiSmoke]::GetDlgItem($powerPane,21),[ref]$ch2)|Out-Null
            [SerialCtlUiSmoke]::GetWindowRect([SerialCtlUiSmoke]::GetDlgItem($powerPane,22),[ref]$ch3)|Out-Null
            if ($ch1.Top -ne $ch2.Top -or $ch2.Top -ne $ch3.Top -or $ch1.Right -ge $ch2.Left -or $ch2.Right -ge $ch3.Left) { throw 'Power channel cards are not three distinct columns.' }
            $powerBounds=New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetClientRect($powerPane,[ref]$powerBounds)|Out-Null
            $powerWidth=$powerBounds.Right
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,10,($powerWidth-200),0,96,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,11,($powerWidth-96),0,96,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,80,72,52,160,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,81,($powerWidth-312),52,104,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,12,($powerWidth-200),52,96,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,13,($powerWidth-96),52,96,36)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,90,2,338,92,28)
            [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,103,($powerWidth-248),384,72,28)
            # The approved views are captured from the real EXE with no hardware or fake measurements.
            foreach ($tab in @(91,92,93)) {
                Click-Control $powerPane $tab
                Start-Sleep -Milliseconds 200
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-tab-$tab"
                if($tab -eq 91) { [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,40,0,499,[int](($powerWidth-184)/2),36) }
                if($tab -eq 92) { [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,202,132,451,[int](($powerWidth-144)/2),28) }
            }
            Click-Control $powerPane 92
            foreach ($category in @(191,192,193)) {
                Click-Control $powerPane $category
                Start-Sleep -Milliseconds 150
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-settings-$category"
            }
            Click-Control $powerPane 90
            foreach ($mode in @(1,2,3,0)) {
                [SerialCtlUiSmoke]::Send([SerialCtlUiSmoke]::GetDlgItem($powerPane,80),0x14E,[IntPtr]$mode,[IntPtr]0)|Out-Null
                [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]((1 -shl 16) -bor 80),[IntPtr]::Zero)|Out-Null
                Start-Sleep -Milliseconds 200
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-mode-$mode"
                if ($mode -eq 1 -or $mode -eq 2) {
                    if ([SerialCtlUiSmoke]::IsWindowVisible([SerialCtlUiSmoke]::GetDlgItem($powerPane,21))) { throw 'Combination rendered duplicate CH2 checkbox.' }
                    if ([SerialCtlUiSmoke]::IsWindowVisible([SerialCtlUiSmoke]::GetDlgItem($powerPane,51))) { throw 'Combination rendered duplicate CH2 parameters.' }
                }
            }
            Click-Control $powerPane 91
            foreach ($automation in @(111,112,113,114,110)) {
                Click-Control $powerPane $automation
                Start-Sleep -Milliseconds 150
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-automation-$automation"
            }
            # The editor is visible only on the Steps category. A WM_COMMAND
            # could previously invoke it while hidden on the Cycle category.
            Click-Control $powerPane 112
            Start-Sleep -Milliseconds 100
            Click-Control $powerPane 123
            $dialog=Wait-Dialog $application.Id '编辑电源步骤'
            Capture-Window $dialog "$architecture-$theme-power-sequence-editor"
            Click-Control $dialog 2
            Start-Sleep -Milliseconds 150
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,980,620,$true)|Out-Null
            Start-Sleep -Milliseconds 150
            foreach($frame in 0..15){Click-Control $powerPane (90+($frame%4));Start-Sleep -Milliseconds 16;Capture-Desktop "$architecture-$theme-power-tab-frame-$frame"}
            Capture-Window $application.MainWindowHandle "$architecture-$theme-power-minimum"
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,1180,760,$true)|Out-Null
            Click-Control $powerPane 90
            Click-Control $application.MainWindowHandle 300
            Start-Sleep -Milliseconds 700
            Capture-Window $application.MainWindowHandle "$architecture-$theme-cmd"
            if ([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,303) -ne [IntPtr]::Zero) { throw 'Obsolete API toolbar entry exists.' }
            # The exact packaged GUI starts its fixed-scope service automatically.
            & python (Join-Path (Get-SerialCtlRepositoryRoot) 'tests/integration/gui_gateway_smoke.py') $native $application.MainWindowHandle.ToInt64() (Join-Path $OutputDirectory ("$architecture-$theme-display.json"))
            if ($LASTEXITCODE -ne 0) { throw 'Unified gateway / real GUI CMD key editing failed.' }
            $streamOptions = @('--start-stream')
            if ($ActualMouse) { $streamOptions += '--long-stream' }
            & python (Join-Path $root 'tests/integration/gui_gateway_smoke.py') $native $application.MainWindowHandle.ToInt64() @streamOptions
            if ($LASTEXITCODE -ne 0) { throw 'Sustained CMD producer did not start.' }
            if ($ActualMouse) {
                & python (Join-Path $root 'tests/integration/desktop_user_actions.py') --window $application.MainWindowHandle.ToInt64() --pid $application.Id --output $OutputDirectory --prefix "$architecture-$theme" --mode desktop
                if ($LASTEXITCODE -ne 0) { throw 'Actual desktop mouse move/resize/panel test failed.' }
            }
            [SerialCtlUiSmoke]::SetForegroundWindow($application.MainWindowHandle)|Out-Null
            $commandRect=New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetWindowRect([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,112),[ref]$commandRect)|Out-Null
            $splitX=$commandRect.Left-22 # 16 px card padding and half of the 12 px gap (runner DPI 100%).
            $mouseY=$commandRect.Top+100
            [SerialCtlUiSmoke]::SetCursorPos($splitX,$mouseY)|Out-Null
            [SerialCtlUiSmoke]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
            foreach($frame in 0..11){
                [SerialCtlUiSmoke]::SetCursorPos(($splitX-($frame%6)*18),$mouseY)|Out-Null
                Start-Sleep -Milliseconds 16
                Capture-Desktop "$architecture-$theme-drag-frame-$frame"
            }
            [SerialCtlUiSmoke]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            $afterDrag=New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetWindowRect([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,112),[ref]$afterDrag)|Out-Null
            if($afterDrag.Left -eq $commandRect.Left){throw 'Actual mouse did not drag sidebar splitter.'}
            foreach($frame in 0..9){
                Click-Control $application.MainWindowHandle 125
                Start-Sleep -Milliseconds 45
                Capture-Desktop "$architecture-$theme-animation-frame-$frame"
            }
            Start-Sleep -Milliseconds 250
            if([SerialCtlUiSmoke]::IsWindowVisible([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,112)) -eq $false){Click-Control $application.MainWindowHandle 125;Start-Sleep -Milliseconds 250}
            foreach($frame in 0..11){[SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,(980+($frame%6)*40),(620+($frame%6)*20),$true)|Out-Null;Start-Sleep -Milliseconds 16;Capture-Desktop "$architecture-$theme-resize-frame-$frame"}
            [SerialCtlUiSmoke]::ShowWindow($application.MainWindowHandle,3)|Out-Null
            Start-Sleep -Milliseconds 100;Capture-Desktop "$architecture-$theme-maximized"
            [SerialCtlUiSmoke]::ShowWindow($application.MainWindowHandle,9)|Out-Null
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,1180,760,$true)|Out-Null
            Capture-Window $application.MainWindowHandle "$architecture-$theme-api-cmd-visible"
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle,$false)
            Click-Control $application.MainWindowHandle 115
            $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5re75Yqg5bi455So5ZG95Luk')))
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-command"
            Click-Control $dialog 1104
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-macro"
            for ($step=3; $step -le 10; $step++) {
                Click-Control $dialog 1104
                Start-Sleep -Milliseconds 70
            }
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-macro-ten"
            Click-Control $dialog 2
            Start-Sleep -Milliseconds 200
        }
        # Draft edits and reorder must not touch commands.txt until explicit Save.
        $commandsFile = Join-Path $settingsDirectory 'commands.txt'
        $before = (Get-FileHash $commandsFile -Algorithm SHA256).Hash
        Click-Control $application.MainWindowHandle 115
        $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5re75Yqg5bi455So5ZG95Luk')))
        [SerialCtlUiSmoke]::Fill($dialog, 1101, 'UI draft command')
        [SerialCtlUiSmoke]::Fill($dialog, 1102, 'echo ui-draft')
        $cover=Start-Process -FilePath (Join-Path $env:WINDIR 'System32/notepad.exe') -PassThru
        try {
            $cover.WaitForInputIdle(5000)|Out-Null
            [SerialCtlUiSmoke]::SetForegroundWindow($dialog)|Out-Null
            Start-Sleep -Milliseconds 100
            if([SerialCtlUiSmoke]::GetForegroundWindow() -ne $dialog){throw 'Cannot establish foreground modal test condition.'}
            Capture-Desktop "$architecture-modal-foreground-before"
            Click-Control $dialog 1
            Start-Sleep -Milliseconds 100
            if([SerialCtlUiSmoke]::GetForegroundWindow() -ne $application.MainWindowHandle){throw 'Modal dismissal did not retain owner foreground.'}
            Capture-Desktop "$architecture-modal-owner-after"
        } finally {if(-not $cover.HasExited){$cover.Kill()}}
        if ((Get-FileHash $commandsFile -Algorithm SHA256).Hash -ne $before) { throw 'Draft was saved automatically.' }
        $list = [SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 112)
        [SerialCtlUiSmoke]::PostMessage($list, 0x201, [IntPtr]1, [IntPtr]((20 -shl 16) -bor 16)) | Out-Null
        [SerialCtlUiSmoke]::PostMessage($list, 0x202, [IntPtr]0, [IntPtr]((80 -shl 16) -bor 16)) | Out-Null
        Start-Sleep -Milliseconds 300
        if ((Get-FileHash $commandsFile -Algorithm SHA256).Hash -ne $before) { throw 'Reorder was saved automatically.' }
        Capture-Window $application.MainWindowHandle "$architecture-command-draft"
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $confirm = Wait-Dialog $application.Id $null
        Capture-Window $confirm "$architecture-unsaved-close"
        Click-Control $confirm 2
        Start-Sleep -Milliseconds 200
        $application.Refresh()
        if ($application.HasExited) { throw 'Cancel did not preserve draft window.' }
        Click-Control $application.MainWindowHandle 132
        Start-Sleep -Milliseconds 300
        if ((Get-FileHash $commandsFile -Algorithm SHA256).Hash -eq $before) { throw 'Explicit Save did not persist draft.' }
        if (-not (Get-Content $commandsFile -Raw).Contains('echo ui-draft')) { throw 'Saved command missing.' }
        $lifecycleScript = if ($ActualMouse) { 'tests/integration/gui_user_lifecycle.py' } else { 'tests/integration/gui_disconnect_smoke.py' }
        & python (Join-Path $root $lifecycleScript) $native $application.MainWindowHandle.ToInt64() $application.Id (Join-Path $OutputDirectory 'gui-lifecycle.json')
        if ($ActualMouse -and $LASTEXITCODE -eq 2) { $behaviorFailures.Add('Real connection list Home/End behavior failed; see gui-lifecycle.json. Remaining cases continue.') }
        elseif ($LASTEXITCODE -ne 0) { throw 'GUI connection list / all-disconnect process cleanup failed.' }
        Capture-Desktop "$architecture-all-disconnected"
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not $application.WaitForExit(10000)) { throw 'Packaged application did not close normally.' }
        if ($application.ExitCode -ne 0) { throw 'Packaged application exit code was not zero.' }
    } catch {
        Capture-Desktop "$architecture-failure-desktop"
        if($application.MainWindowHandle -ne [IntPtr]::Zero){Capture-Window $application.MainWindowHandle "$architecture-failure-window"}
        $failureLogs=Join-Path $env:APPDATA 'SerialCtl/logs'
        if(Test-Path $failureLogs){Get-ChildItem $failureLogs -Filter '*.log' | ForEach-Object {Copy-Item $_.FullName (Join-Path $OutputDirectory ('failure-'+$_.Name)) -Force}}
        throw
    } finally { if (-not $application.HasExited) { $application.Kill() } }
    # Exercise the asynchronous first-host dialog without any real network/credentials.
    $sandbox = Join-Path $OutputDirectory "mock-$architecture"
    New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
    Copy-Item (Join-Path $native '*.exe') $sandbox
    Copy-Item $mock (Join-Path $sandbox 'plink.exe') -Force
    Copy-Item $mock (Join-Path $sandbox 'psftp.exe') -Force
    $application = Start-Process -FilePath (Join-Path $sandbox 'serialctl.exe') -PassThru
    $testHost = "serialctl-ui-$architecture-$($env:GITHUB_RUN_ID).invalid"
    try {
        if (-not $application.WaitForInputIdle(10000)) { throw 'UI mock application did not become idle.' }
        $application.Refresh()
        # Input-idle may arrive before .NET observes the new top-level HWND.
        for ($attempt=0; $attempt -lt 50 -and -not $application.HasExited -and $application.MainWindowHandle -eq 0; $attempt++) {
            Start-Sleep -Milliseconds 100
            $application.Refresh()
        }
        if ($application.HasExited -or $application.MainWindowHandle -eq 0) {
            throw 'UI mock application did not create a main window.'
        }
        foreach ($theme in @('light','dark')) {
            if ($theme -eq 'dark') {
                Click-Control $application.MainWindowHandle 104
                Start-Sleep -Milliseconds 200
            }
            Click-Control $application.MainWindowHandle 100
            $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu6IFNTSCDov57mjqU=')))
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::Fill($dialog, 1014, $testHost)
            [SerialCtlUiSmoke]::Fill($dialog, 1018, 'test')
            Start-Sleep -Milliseconds 200
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-ssh-dialog"
            Click-Control $dialog 1
            Start-Sleep -Milliseconds 300
            $confirmation = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('56Gu6K6kIFNTSCDkuLvmnLrouqvku70=')))
            Start-Sleep -Milliseconds 300
            Capture-Window $confirmation "$architecture-$theme-host-key"
            $answer = if ($theme -eq 'light') { 7 } else { 6 }
            Click-Control $confirmation $answer
            Start-Sleep -Milliseconds 1000
            $result = [IntPtr]::Zero
            if ([SerialCtlUiSmoke]::SendMessageTimeout($application.MainWindowHandle, 0, [IntPtr]::Zero,
                    [IntPtr]::Zero, 2, 2000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Main window became unresponsive.' }
        }
        Click-Control $application.MainWindowHandle 104
        Start-Sleep -Milliseconds 200
        foreach ($theme in @('light','dark')) {
            if ($theme -eq 'dark') {
                Click-Control $application.MainWindowHandle 104
                Start-Sleep -Milliseconds 200
            }
            Click-Control $application.MainWindowHandle 118
            Start-Sleep -Milliseconds 1000
            if (-not [SerialCtlUiSmoke]::IsWindowVisible([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 119))) { throw 'SFTP panel did not open.' }
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $false)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-sftp"
            $pathControl = [SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 119)
            [SerialCtlUiSmoke]::PostMessage($pathControl, 0x203, [IntPtr]0, [IntPtr]0) | Out-Null
            $pathDialog = Wait-Dialog $application.Id $null
            [SerialCtlUiSmoke]::Fill($pathDialog, 1402, '/tmp/a path')
            [SerialCtlUiSmoke]::AssertLabels($pathDialog, $expectedUiFace)
            Capture-Window $pathDialog "$architecture-$theme-sftp-path"
            Click-Control $pathDialog 1
            Start-Sleep -Milliseconds 500
            $header = [SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 126)
            $bounds = New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetClientRect($header, [ref]$bounds) | Out-Null
            $originalWidth = $bounds.Right
            $headerScreen=New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetWindowRect($header,[ref]$headerScreen)|Out-Null
            $terminal = [SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 107)
            $beforePaint = [SerialCtlUiSmoke]::GetProp($terminal, 'SerialCtl.PaintSample').ToInt64()
            if ($beforePaint -le 0) {throw 'Controlled SSH stdout was not actually painted before SFTP drag.'}
            [SerialCtlUiSmoke]::SetForegroundWindow($application.MainWindowHandle)|Out-Null
            [SerialCtlUiSmoke]::SetCursorPos(($headerScreen.Right-2),($headerScreen.Top+16))|Out-Null
            [SerialCtlUiSmoke]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
            foreach($frame in 1..10){[SerialCtlUiSmoke]::SetCursorPos(($headerScreen.Right-2-$frame*3),($headerScreen.Top+16))|Out-Null;Start-Sleep -Milliseconds 16;Capture-Desktop "$architecture-$theme-sftp-column-frame-$frame"}
            $afterPaint = [SerialCtlUiSmoke]::GetProp($terminal, 'SerialCtl.PaintSample').ToInt64()
            [SerialCtlUiSmoke]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            Start-Sleep -Milliseconds 300
            if ($afterPaint -le $beforePaint) {throw 'SFTP column drag did not overlap actual sustained stdout painting.'}
            [ordered]@{beforePaint=$beforePaint;afterPaint=$afterPaint;capture='CopyFromScreen, actual mouse, 10 intermediate frames';source='Isolated mock SSH stdout every 10ms, production terminal/SFTP controls; no real server test'} | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory "$architecture-$theme-sftp-sustained.json") -Encoding UTF8
            [SerialCtlUiSmoke]::GetClientRect($header, [ref]$bounds) | Out-Null
            if ($bounds.Right -ge $originalWidth) { throw 'SFTP name column did not resize.' }
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $false)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-sftp-resized"
            if ($ActualMouse) {
                & python (Join-Path $root 'tests/integration/desktop_user_actions.py') --window $application.MainWindowHandle.ToInt64() --pid $application.Id --output $OutputDirectory --prefix "$architecture-$theme" --mode sftp
                if ($LASTEXITCODE -ne 0) { throw 'Actual SFTP column/panel mouse test failed.' }
            }
            Click-Control $application.MainWindowHandle 125
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $true)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-sftp-collapsed"
            Click-Control $application.MainWindowHandle 125
            Start-Sleep -Milliseconds 200
            Click-Control $application.MainWindowHandle 117
            Start-Sleep -Milliseconds 200
        }
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not $application.WaitForExit(10000)) { throw 'Mock application did not close normally.' }
        if ($application.ExitCode -ne 0) { throw 'Mock application failed.' }
    } finally {
        if (-not $application.HasExited) { $application.Kill() }
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\SerialCtl\TrustedHostKeys', $true)
        if ($null -ne $key) { $key.DeleteValue(($testHost + ':22'), $false); $key.Dispose() }
    }
}
Remove-Item -LiteralPath $extract -Recurse -Force
Remove-Item -LiteralPath $mock -Force
foreach ($architecture in @('x64')) { Remove-Item -LiteralPath (Join-Path $OutputDirectory "mock-$architecture") -Recurse -Force }
if ($behaviorFailures.Count -gt 0) { throw ($behaviorFailures -join "`n") }
Write-Host '[PASS] Executed assertions for packaged startup/shutdown, theme/dialog layout, controlled CMD, panel geometry, drafts, and isolated SFTP UI. See per-case JSON for input method and scope.'
Write-Host '[NOT RUN] Actual Windows 7 hardware and field server tests'
