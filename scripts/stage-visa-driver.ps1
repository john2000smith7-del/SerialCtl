[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$root=Split-Path $PSScriptRoot -Parent
$manifest=Get-Content (Join-Path $root 'third_party/ni-visa/files.json') -Raw | ConvertFrom-Json
$archive=Join-Path ([IO.Path]::GetTempPath()) 'SerialCtl-NIVISA1800runtime.zip'
if ($env:SERIALCTL_VISA_CACHE) { $archive = $env:SERIALCTL_VISA_CACHE }
if (-not (Test-Path $archive) -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.sha256) {
    Invoke-WebRequest -Uri $manifest.url -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.sha256) { throw 'Official VISA runtime checksum mismatch' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($archive)
try {
    $expected=@($manifest.files.path | Sort-Object)
    $actual=@($zip.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName } | Sort-Object)
    if (@(Compare-Object $expected $actual).Count) { throw 'VISA archive allowlist mismatch' }
    foreach ($entry in $zip.Entries) {
        if (-not $entry.Name) { continue }
        if ($entry.FullName -match '(^/|\\|(^|/)\.\.(/|$)|:)') { throw 'Invalid driver path' }
        $target=Join-Path $Destination ($entry.FullName.Replace('/','\'))
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
    }
} finally { $zip.Dispose() }
foreach ($file in $manifest.files) {
    $target=Join-Path $Destination ($file.path.Replace('/','\'))
    if ((Get-FileHash $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw "Driver file checksum mismatch: $($file.path)" }
}
Write-Host '[PASS] Original NI-VISA 18.0 Runtime, complete offline installer and licenses'
