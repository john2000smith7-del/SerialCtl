[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x86', 'x64')]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [ValidateSet('GUI', 'CUI')]
    [string]$Subsystem,

    [string]$ExpectedProductVersion,

    [string[]]$AllowedDll = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\common.ps1')

$resolvedPath = [System.IO.Path]::GetFullPath($Path)
if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
    throw "Binary is missing: $resolvedPath"
}

$dumpbinPath = Get-SerialCtlDumpbinPath
$headers = @(& $dumpbinPath /headers $resolvedPath 2>&1 | ForEach-Object { [string]$_ })
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /headers failed for $resolvedPath"
}
$headerText = $headers -join "`n"

$machinePattern = if ($Architecture -eq 'x86') { '(?im)^\s*14C machine \(x86\)' } else { '(?im)^\s*8664 machine \(x64\)' }
if ($headerText -notmatch $machinePattern) {
    throw "$resolvedPath does not have the expected $Architecture PE machine type"
}
if ($headerText -notmatch '(?im)^\s*6\.01 subsystem version\s*$') {
    throw "$resolvedPath does not declare PE subsystem version 6.01"
}

$subsystemPattern = if ($Subsystem -eq 'GUI') {
    '(?im)^\s*2 subsystem \(Windows GUI\)\s*$'
} else {
    '(?im)^\s*3 subsystem \(Windows CUI\)\s*$'
}
if ($headerText -notmatch $subsystemPattern) {
    throw "$resolvedPath does not use the expected Windows $Subsystem subsystem"
}

$imports = @(& $dumpbinPath /imports $resolvedPath 2>&1 | ForEach-Object { [string]$_ })
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /imports failed for $resolvedPath"
}
$importText = $imports -join "`n"

$importedDlls = @($imports | ForEach-Object {
    if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
        $Matches[1].ToUpperInvariant()
    }
} | Sort-Object -Unique)

if ($AllowedDll.Count -gt 0) {
    $allowed = @($AllowedDll | ForEach-Object { $_.ToUpperInvariant() } | Sort-Object -Unique)
    $unexpected = @($importedDlls | Where-Object { $allowed -notcontains $_ })
    $missing = @($allowed | Where-Object { $importedDlls -notcontains $_ })
    if ($unexpected.Count -gt 0 -or $missing.Count -gt 0) {
        throw ("Unexpected DLL import set for {0}. Unexpected: [{1}]. Missing: [{2}]" -f `
            $resolvedPath, ($unexpected -join ', '), ($missing -join ', '))
    }
}

$forbiddenDllPatterns = @(
    '^API-MS-WIN-',
    '^EXT-MS-WIN-',
    '^MSCOREE\.DLL$',
    '^MSVCP[0-9]+\.DLL$',
    '^VCRUNTIME[0-9_]*\.DLL$',
    '^UCRTBASE\.DLL$'
)
foreach ($dll in $importedDlls) {
    foreach ($pattern in $forbiddenDllPatterns) {
        if ($dll -match $pattern) {
            throw "$resolvedPath imports a disallowed runtime or API-set DLL: $dll"
        }
    }
}

$postWindows7Apis = @(
    'AdjustWindowRectExForDpi',
    'CreateFile2',
    'GetDpiForSystem',
    'GetDpiForWindow',
    'GetFirmwareType',
    'GetPackageFullName',
    'GetProcessInformation',
    'GetSystemCpuSetInformation',
    'GetSystemMetricsForDpi',
    'GetSystemTimePreciseAsFileTime',
    'GetTempPath2A',
    'GetTempPath2W',
    'GetThreadInformation',
    'GetPointerInfo',
    'IsWow64Process2',
    'PrefetchVirtualMemory',
    'SetProcessDpiAwarenessContext',
    'SetThreadDescription',
    'SetThreadDpiAwarenessContext',
    'SetThreadInformation'
)
foreach ($api in $postWindows7Apis) {
    if ($importText -match ("(?m)^\s+[0-9A-F]+\s+" + [regex]::Escape($api) + "\s*$")) {
        throw "$resolvedPath has a hard import that is newer than Windows 7: $api"
    }
}

if (-not [string]::IsNullOrWhiteSpace($ExpectedProductVersion)) {
    if ($ExpectedProductVersion -notmatch '^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)$') {
        throw "ExpectedProductVersion must contain four numeric fields: $ExpectedProductVersion"
    }
    $expectedParts = @(
        [int]$Matches[1],
        [int]$Matches[2],
        [int]$Matches[3],
        [int]$Matches[4]
    )
    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($resolvedPath)
    $actualFileParts = @(
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart
    )
    $actualProductParts = @(
        $versionInfo.ProductMajorPart,
        $versionInfo.ProductMinorPart,
        $versionInfo.ProductBuildPart,
        $versionInfo.ProductPrivatePart
    )
    if (($actualFileParts -join '.') -ne ($expectedParts -join '.')) {
        throw "$resolvedPath FileVersion is $($actualFileParts -join '.'), expected $ExpectedProductVersion"
    }
    if (($actualProductParts -join '.') -ne ($expectedParts -join '.')) {
        throw "$resolvedPath ProductVersion is $($actualProductParts -join '.'), expected $ExpectedProductVersion"
    }
}

Write-Host "[PASS] $Architecture / $Subsystem / Win7 PE: $resolvedPath"
