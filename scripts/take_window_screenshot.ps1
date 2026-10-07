param([Parameter(Mandatory = $true)][int]$WindowHandle)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$profileDirectory = [Environment]::GetFolderPath('UserProfile')
$picturesDirectory = Join-Path $profileDirectory 'Pictures'
$screenshotDirectory = Join-Path $picturesDirectory 'Screenshots'
if (-not (Test-Path -LiteralPath $screenshotDirectory)) {
    if (Test-Path -LiteralPath $picturesDirectory) {
        $screenshotDirectory = $picturesDirectory
    } else {
        $screenshotDirectory = $profileDirectory
    }
}
$outputPath = Join-Path $screenshotDirectory ("serialctl-ui-{0}.png" -f (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'))

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SerialCtlScreenshotNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();
}
"@

[SerialCtlScreenshotNative]::SetProcessDPIAware() | Out-Null
$rectangle = New-Object SerialCtlScreenshotNative+RECT
if (-not [SerialCtlScreenshotNative]::GetWindowRect([IntPtr]$WindowHandle, [ref]$rectangle)) {
    throw 'Failed to get SerialCtl window bounds.'
}
$width = $rectangle.Right - $rectangle.Left
$height = $rectangle.Bottom - $rectangle.Top
$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen(
        (New-Object System.Drawing.Point($rectangle.Left, $rectangle.Top)),
        [System.Drawing.Point]::Empty,
        (New-Object System.Drawing.Size($width, $height)))
    $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}

Write-Output $outputPath
