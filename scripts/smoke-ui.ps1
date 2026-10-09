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
    public static IntPtr Dialog(uint processId, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr ignored) {
            uint pid; GetWindowThreadProcessId(window, out pid);
            var name = new StringBuilder(64); GetClassName(window, name, name.Capacity);
            var caption = new StringBuilder(256); GetWindowText(window, caption, caption.Capacity);
            if (pid == processId && IsWindowVisible(window) && name.ToString() == "#32770" && (title == null || caption.ToString() == title)) { found = window; return false; }
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
        foreach (string arg in args) if (arg == "-hostkey") return 0;
        Console.WriteLine("The host key is not cached for this server\nkey fingerprint is:\nssh-ed25519 255 SHA256:ui-smoke-test");
        return 1;
    }
}
'@
foreach ($architecture in @('x86','x64')) {
    $native = Join-Path $extract "SerialCtl-$($version.Display)/windows/$architecture/native"
    # First launch the exact packaged program with its exact packaged dependencies.
    $application = Start-Process -FilePath (Join-Path $native 'serialctl.exe') -PassThru
    try {
        if (-not $application.WaitForInputIdle(10000)) { throw 'Packaged application did not become idle.' }
        $application.Refresh()
        if ($application.HasExited -or $application.MainWindowHandle -eq 0) { throw 'Packaged application did not open a window.' }
        Capture-Window $application.MainWindowHandle "$architecture-packaged-dark"
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]104, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 200
        Capture-Window $application.MainWindowHandle "$architecture-packaged-light"
        [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        if (-not $application.WaitForExit(10000)) { throw 'Packaged application did not close normally.' }
        if ($application.ExitCode -ne 0) { throw 'Packaged application exit code was not zero.' }
    } finally { if (-not $application.HasExited) { $application.Kill() } }
    # Exercise the asynchronous first-host dialog without any real network/credentials.
    $sandbox = Join-Path $OutputDirectory "mock-$architecture"
    New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
    Copy-Item (Join-Path $native '*.exe') $sandbox
    Copy-Item $mock (Join-Path $sandbox 'plink.exe') -Force
    $application = Start-Process -FilePath (Join-Path $sandbox 'serialctl.exe') -PassThru
    $testHost = "serialctl-ui-$architecture-$($env:GITHUB_RUN_ID).invalid"
    try {
        if (-not $application.WaitForInputIdle(10000)) { throw 'UI mock application did not become idle.' }
        $application.Refresh()
        foreach ($theme in @('dark','light')) {
            if ($theme -eq 'light') {
                [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]104, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 200
            }
            [SerialCtlUiSmoke]::PostMessage($application.MainWindowHandle, 0x111, [IntPtr]100, [IntPtr]::Zero) | Out-Null
            $dialog = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('5paw5bu6IFNTSCDov57mjqU=')))
            Start-Sleep -Milliseconds 300
            [SerialCtlUiSmoke]::Fill($dialog, 1014, $testHost)
            [SerialCtlUiSmoke]::Fill($dialog, 1018, 'test')
            Start-Sleep -Milliseconds 200
            Capture-Window $dialog "$architecture-$theme-ssh-dialog"
            [SerialCtlUiSmoke]::PostMessage($dialog, 0x111, [IntPtr]1, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
            $confirmation = Wait-Dialog $application.Id ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('56Gu6K6kIFNTSCDkuLvmnLrouqvku70=')))
            Capture-Window $confirmation "$architecture-$theme-host-key"
            $answer = if ($theme -eq 'dark') { 7 } else { 6 }
            [SerialCtlUiSmoke]::PostMessage($confirmation, 0x111, [IntPtr]$answer, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 1000
            $result = [IntPtr]::Zero
            if ([SerialCtlUiSmoke]::SendMessageTimeout($application.MainWindowHandle, 0, [IntPtr]::Zero,
                    [IntPtr]::Zero, 2, 2000, [ref]$result) -eq [IntPtr]::Zero) { throw 'Main window became unresponsive.' }
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
foreach ($architecture in @('x86','x64')) { Remove-Item -LiteralPath (Join-Path $OutputDirectory "mock-$architecture") -Recurse -Force }
Write-Host '[PASS] x86/x64 packaged startup/shutdown, both themes, SSH dialogs and first-host confirmation'
Write-Host '[NOT RUN] Actual Windows 7 hardware and field server tests'
