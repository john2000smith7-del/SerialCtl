[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Add-Type -AssemblyName System.Drawing
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
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr window,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
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
$root = Get-SerialCtlRepositoryRoot
$version = Get-SerialCtlVersion
$extract = Join-Path $OutputDirectory 'payload'
Expand-Archive -LiteralPath (Join-Path $root ('bin/' + (Get-SerialCtlArchiveName))) -DestinationPath $extract -Force
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
        foreach (string arg in args) if (arg == "-hostkey") { while (Console.ReadLine() != null) {} return 0; }
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
                [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]104, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 300
            }
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle, 0, 0, 980, 620, $true) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $false)
            Capture-Window $application.MainWindowHandle "$architecture-packaged-$theme"
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]125, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $true)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-collapsed"
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]125, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 200
            foreach ($mode in 0..3) {
                [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr](100+$mode), [IntPtr]::Zero) | Out-Null
                $dialog = Wait-Dialog $application.Id $titles[$mode]
                Start-Sleep -Milliseconds 300
                [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
                Capture-Window $dialog "$architecture-$theme-connection-$mode"
                if ($mode -eq 3) {
                    [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]9010, [IntPtr]::Zero) | Out-Null
                    $portDialog = Wait-Dialog $application.Id '高级共享端口'
                    [SerialCtlUiSmoke]::Fill($portDialog, 1402, '7000')
                    [SerialCtlUiSmoke]::AssertLabels($portDialog, $expectedUiFace)
                    Capture-Window $portDialog "$architecture-$theme-shared-port"
                    [SerialCtlUiSmoke]::PostMessage($portDialog, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
                    Start-Sleep -Milliseconds 200
                }
                [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 200
            }
            # New power pane occupies the central terminal area. It keeps independent connection/output actions.
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]301, [IntPtr]::Zero) | Out-Null
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
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]10,[IntPtr]::Zero) | Out-Null
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
            [SerialCtlUiSmoke]::PostMessage($dialog,0x111,[IntPtr]2,[IntPtr]::Zero) | Out-Null
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
                [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]$tab,[IntPtr]::Zero)|Out-Null
                Start-Sleep -Milliseconds 200
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-tab-$tab"
                if($tab -eq 91) { [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,40,0,499,[int](($powerWidth-184)/2),36) }
                if($tab -eq 92) { [SerialCtlUiSmoke]::AssertPowerBounds($powerPane,202,132,451,[int](($powerWidth-144)/2),28) }
            }
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]92,[IntPtr]::Zero)|Out-Null
            foreach ($category in @(191,192,193)) {
                [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]$category,[IntPtr]::Zero)|Out-Null
                Start-Sleep -Milliseconds 150
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-settings-$category"
            }
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]90,[IntPtr]::Zero)|Out-Null
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
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]91,[IntPtr]::Zero)|Out-Null
            foreach ($automation in @(111,112,113,114,110)) {
                [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]$automation,[IntPtr]::Zero)|Out-Null
                Start-Sleep -Milliseconds 150
                Capture-Window $application.MainWindowHandle "$architecture-$theme-power-automation-$automation"
            }
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]123,[IntPtr]::Zero)|Out-Null
            $dialog=Wait-Dialog $application.Id '编辑电源步骤'
            Capture-Window $dialog "$architecture-$theme-power-sequence-editor"
            [SerialCtlUiSmoke]::PostMessage($dialog,0x111,[IntPtr]2,[IntPtr]::Zero)|Out-Null
            Start-Sleep -Milliseconds 150
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,980,620,$true)|Out-Null
            Start-Sleep -Milliseconds 150
            Capture-Window $application.MainWindowHandle "$architecture-$theme-power-minimum"
            [SerialCtlUiSmoke]::MoveWindow($application.MainWindowHandle,0,0,1180,760,$true)|Out-Null
            [SerialCtlUiSmoke]::PostMessage($powerPane,0x111,[IntPtr]90,[IntPtr]::Zero)|Out-Null
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle,0x111,[IntPtr]300,[IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 700
            Capture-Window $application.MainWindowHandle "$architecture-$theme-cmd"
            if ([SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle,302) -ne [IntPtr]::Zero) { throw 'Obsolete API toolbar entry exists.' }
            # The exact packaged GUI starts its fixed-scope service automatically.
            $found = @((& python (Join-Path $native 'serialctl_api.py') 127.0.0.1 discover | ConvertFrom-Json))
            if ($found.Count -ne 1) { throw 'Automatic API discovery failed.' }
            $script:apiBase = 'http://127.0.0.1:' + $found[0].port + '/api/v1'
            function Request-TestApi([string]$Path,$Body=$null) {
                if ($null -eq $Body) { return Invoke-RestMethod -Uri ($apiBase+$Path) -TimeoutSec 10 }
                return Invoke-RestMethod -Uri ($apiBase+$Path) -Method Post -ContentType 'application/json' -Body ($Body|ConvertTo-Json -Compress) -TimeoutSec 10
            }
            function Assert-TestApiDenied([string]$Path,$Body=@{}) {
                $denied=$false
                try { Request-TestApi $Path $Body | Out-Null }
                catch { if ($_.Exception.Response.StatusCode.value__ -eq 403) { $denied=$true } else { throw } }
                if (-not $denied) { throw ('Unexpected API capability: '+$Path) }
            }
            $resources = (Request-TestApi '/resources').resources
            $cmdResource = @($resources | Where-Object { $_.kind -eq 'cmd' -and $_.connected })[-1].id
            if (-not $cmdResource) { throw 'New CMD session not automatically available.' }
            $powerState = Request-TestApi '/power-supplies/power-1'
            if ($powerState.connected -or $powerState.selectedChannels.Count -ne 1 -or $powerState.selectedChannels[0] -ne 1) { throw 'Unexpected actual power connection/selection state.' }
            foreach ($endpoint in @('connect','disconnect','channels/parameters','channels/protection','task','stop')) {
                Assert-TestApiDenied ('/power-supplies/power-1/'+$endpoint)
            }
            Assert-TestApiDenied '/power-supplies/power-1/channels/output' @{channels=@(2);enabled=$true}
            $command = [Text.Encoding]::ASCII.GetBytes("echo SERIALCTL_REMOTE_VISIBLE`r`n")
            Request-TestApi ('/sessions/'+$cmdResource+'/input') @{ data=[Convert]::ToBase64String($command) } | Out-Null
            $received = ''
            for ($attempt=0;$attempt -lt 50;$attempt++) {
                $events = Request-TestApi ('/sessions/'+$cmdResource+'/events?after=0')
                $received = (@($events.events | Where-Object { $_.type -eq 'output' } | ForEach-Object {
                    [Text.Encoding]::ASCII.GetString([Convert]::FromBase64String($_.data))
                }) -join '')
                if ($received.Contains('SERIALCTL_REMOTE_VISIBLE')) { break }
                Start-Sleep -Milliseconds 100
            }
            if (-not $received.Contains('SERIALCTL_REMOTE_VISIBLE')) { throw 'GUI API CMD stream lost command output.' }
            Capture-Window $application.MainWindowHandle "$architecture-$theme-api-cmd-visible"
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle,$false)
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]115, [IntPtr]::Zero) | Out-Null
            $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5re75Yqg5bi455So5ZG95Luk')))
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-command"
            [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]1104, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-macro"
            for ($step=3; $step -le 10; $step++) {
                [SerialCtlUiSmoke]::PostMessage($dialog,0x111,[IntPtr]1104,[IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 70
            }
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-macro-ten"
            [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 200
        }
        # Draft edits and reorder must not touch commands.txt until explicit Save.
        $commandsFile = Join-Path $settingsDirectory 'commands.txt'
        $before = (Get-FileHash $commandsFile -Algorithm SHA256).Hash
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]115, [IntPtr]::Zero) | Out-Null
        $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5re75Yqg5bi455So5ZG95Luk')))
        [SerialCtlUiSmoke]::Fill($dialog, 1101, 'UI draft command')
        [SerialCtlUiSmoke]::Fill($dialog, 1102, 'echo ui-draft')
        [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 300
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
        [SerialCtlUiSmoke]::PostMessage($confirm, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 200
        $application.Refresh()
        if ($application.HasExited) { throw 'Cancel did not preserve draft window.' }
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]132, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 300
        if ((Get-FileHash $commandsFile -Algorithm SHA256).Hash -eq $before) { throw 'Explicit Save did not persist draft.' }
        if (-not (Get-Content $commandsFile -Raw).Contains('echo ui-draft')) { throw 'Saved command missing.' }
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not $application.WaitForExit(10000)) { throw 'Packaged application did not close normally.' }
        if ($application.ExitCode -ne 0) { throw 'Packaged application exit code was not zero.' }
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
                [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]104, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 200
            }
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]100, [IntPtr]::Zero) | Out-Null
            $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu6IFNTSCDov57mjqU=')))
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::Fill($dialog, 1014, $testHost)
            [SerialCtlUiSmoke]::Fill($dialog, 1018, 'test')
            Start-Sleep -Milliseconds 200
            [SerialCtlUiSmoke]::AssertLabels($dialog, $expectedUiFace)
            Capture-Window $dialog "$architecture-$theme-ssh-dialog"
            [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
            $confirmation = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('56Gu6K6kIFNTSCDkuLvmnLrouqvku70=')))
            Start-Sleep -Milliseconds 300
            Capture-Window $confirmation "$architecture-$theme-host-key"
            $answer = if ($theme -eq 'light') { 7 } else { 6 }
            [SerialCtlUiSmoke]::PostMessage($confirmation, 0x111, [IntPtr]$answer, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 1000
            $result = [IntPtr]::Zero
            if ([SerialCtlUiSmoke]::SendMessageTimeout($application.MainWindowHandle, 0, [IntPtr]::Zero,
                    [IntPtr]::Zero, 2, 2000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Main window became unresponsive.' }
        }
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle,0x111,[IntPtr]104,[IntPtr]::Zero)|Out-Null
        Start-Sleep -Milliseconds 200
        foreach ($theme in @('light','dark')) {
            if ($theme -eq 'dark') {
                [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]104, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 200
            }
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]118, [IntPtr]::Zero) | Out-Null
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
            [SerialCtlUiSmoke]::PostMessage($pathDialog, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 500
            $header = [SerialCtlUiSmoke]::GetDlgItem($application.MainWindowHandle, 126)
            $bounds = New-Object SerialCtlUiSmoke+RECT
            [SerialCtlUiSmoke]::GetClientRect($header, [ref]$bounds) | Out-Null
            $originalWidth = $bounds.Right
            [SerialCtlUiSmoke]::PostMessage($header, 0x201, [IntPtr]1, [IntPtr]((16 -shl 16) -bor ($originalWidth-2))) | Out-Null
            [SerialCtlUiSmoke]::PostMessage($header, 0x200, [IntPtr]1, [IntPtr]((16 -shl 16) -bor ($originalWidth-26))) | Out-Null
            [SerialCtlUiSmoke]::PostMessage($header, 0x202, [IntPtr]0, [IntPtr]((16 -shl 16) -bor ($originalWidth-26))) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::GetClientRect($header, [ref]$bounds) | Out-Null
            if ($bounds.Right -ge $originalWidth) { throw 'SFTP name column did not resize.' }
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $false)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-sftp-resized"
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]125, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::AssertPanel($application.MainWindowHandle, $true)
            Capture-Window $application.MainWindowHandle "$architecture-$theme-sftp-collapsed"
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]125, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 200
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]117, [IntPtr]::Zero) | Out-Null
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
Write-Host '[PASS] x64 startup/shutdown, light default and both themes, monochrome toolbar, power three-column layout and automatic sidebar collapse, USB/RS232-only connection and sequence dialogs, all power modes and categories, persistent CMD, automatic fixed-scope API and real GUI CMD round-trip, all connection and command dialogs, label metrics, narrow/collapsed command and mock SFTP panels, host-key confirmation, command drafts and Save, manual SFTP path and persisted column resize'
Write-Host '[NOT RUN] Actual Windows 7 hardware and field server tests'
