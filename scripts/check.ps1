[CmdletBinding()]
param(
    [ValidateSet('x64', 'All')]
    [string]$Architecture = 'All',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$SourceOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

function Assert-SerialCtlCondition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

$repositoryRoot = Get-SerialCtlRepositoryRoot
$version = Get-SerialCtlVersion -RepositoryRoot $repositoryRoot

$requiredFiles = @(
    'CMakeLists.txt',
    'README.md',
    'VERSION',
    'VERSION.md',
    'src/app/app.manifest.in',
    'src/app/serialctl.rc.in',
    "docs/package/RELEASE-NOTES-$($version.Display).md",
    'docs/package/README.md',
    'docs/testing/WIN7-HARDWARE-TEST.md',
    'THIRD-PARTY-NOTICES.md',
    'third_party/manifest.json'
)
foreach ($relativePath in $requiredFiles) {
    $fullPath = Join-Path $repositoryRoot $relativePath
    Assert-SerialCtlCondition -Condition (Test-Path -LiteralPath $fullPath -PathType Leaf) `
        -Message "Required project file is missing: $relativePath"
}

$readme = Get-Content -LiteralPath (Join-Path $repositoryRoot 'README.md') -Raw -Encoding UTF8
$versionHistory = Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION.md') -Raw -Encoding UTF8
$packageReadme = Get-Content -LiteralPath (Join-Path $repositoryRoot 'docs\package\README.md') -Raw -Encoding UTF8
$releaseNotes = Get-Content -LiteralPath (Join-Path $repositoryRoot "docs\package\RELEASE-NOTES-$($version.Display).md") -Raw -Encoding UTF8
$manifestTemplate = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src\app\app.manifest.in') -Raw -Encoding UTF8
$resourceTemplate = Get-Content -LiteralPath (Join-Path $repositoryRoot 'src\app\serialctl.rc.in') -Raw -Encoding UTF8

Assert-SerialCtlCondition -Condition ($readme.Contains(('`' + $version.Display + '`'))) `
    -Message 'README.md does not match VERSION'
Assert-SerialCtlCondition -Condition ($readme.Contains((Get-SerialCtlArchiveName -RepositoryRoot $repositoryRoot))) `
    -Message 'README.md does not contain the version-derived release archive name'
Assert-SerialCtlCondition -Condition ($versionHistory.Contains("## $($version.Display)")) `
    -Message 'VERSION.md does not contain the current version heading'
Assert-SerialCtlCondition -Condition ($packageReadme.Contains("# SerialCtl $($version.Display)")) `
    -Message 'The package README does not match VERSION'
Assert-SerialCtlCondition -Condition ($releaseNotes.Contains("# SerialCtl $($version.Display)")) `
    -Message 'The release notes do not match VERSION'
Assert-SerialCtlCondition -Condition ($manifestTemplate.Contains('@SERIALCTL_FILE_VERSION_DOTTED@')) `
    -Message 'The application manifest is not generated from VERSION'
foreach ($token in @('@SERIALCTL_FILE_VERSION_COMMA@', '@SERIALCTL_SEMVER@', '@SERIALCTL_MANIFEST_PATH@')) {
    Assert-SerialCtlCondition -Condition ($resourceTemplate.Contains($token)) `
        -Message "The resource template is missing the version token $token"
}

$scanRoots = @('CMakeLists.txt', 'README.md', 'VERSION.md', 'AGENTS.md', 'src', 'tests', 'docs', 'scripts', 'tools')
$oldVersionPattern = ('V2' + '\.0\.0|2' + '\.0\.0\.0|SerialCtl-' + 'V2')
$oldVersionMatches = New-Object System.Collections.Generic.List[string]
foreach ($scanRoot in $scanRoots) {
    $fullScanRoot = Join-Path $repositoryRoot $scanRoot
    $files = @()
    if (Test-Path -LiteralPath $fullScanRoot -PathType Leaf) {
        $files = @(Get-Item -LiteralPath $fullScanRoot)
    } elseif (Test-Path -LiteralPath $fullScanRoot -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $fullScanRoot -Recurse -File -ErrorAction Stop | Where-Object {
            $_.Extension -in @('.cpp', '.h', '.in', '.md', '.txt', '.ps1', '.cmake', '.json')
        })
    }
    foreach ($file in $files) {
        $match = Select-String -LiteralPath $file.FullName -Pattern $oldVersionPattern -Encoding UTF8 -ErrorAction Stop
        foreach ($item in @($match)) {
            $oldVersionMatches.Add("$($file.FullName):$($item.LineNumber)")
        }
    }
}
Assert-SerialCtlCondition -Condition ($oldVersionMatches.Count -eq 0) `
    -Message "Old product-version text remains outside generated/legacy content: $($oldVersionMatches -join ', ')"

$dependencyManifestPath = Join-Path $repositoryRoot 'third_party\manifest.json'
$dependencyManifest = Get-Content -LiteralPath $dependencyManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-SerialCtlCondition -Condition ($dependencyManifest.hash_algorithm -eq 'SHA-256') `
    -Message 'third_party/manifest.json must use SHA-256'
foreach ($dependency in @($dependencyManifest.dependencies)) {
    foreach ($file in @($dependency.files)) {
        $controlledPath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot ([string]$file.path)))
        Assert-SerialCtlCondition -Condition (Test-SerialCtlPathWithin -Path $controlledPath -Parent $repositoryRoot) `
            -Message "Third-party manifest path escapes the repository: $($file.path)"
        Assert-SerialCtlCondition -Condition (Test-Path -LiteralPath $controlledPath -PathType Leaf) `
            -Message "Controlled third-party file is missing: $($file.path)"
        $actualHash = (Get-FileHash -LiteralPath $controlledPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-SerialCtlCondition -Condition ($actualHash -eq ([string]$file.sha256).ToLowerInvariant()) `
            -Message "Third-party hash mismatch: $($file.path)"
    }
}
Write-Host "[PASS] Source/version/dependency checks: $($version.Display)"

$sourceState = Get-SerialCtlSourceState -RepositoryRoot $repositoryRoot
if ($sourceState.State -eq 'unversioned') {
    Write-Host '[NOT RUN] Git provenance: SerialCtl is not yet an independent repository'
} elseif ($sourceState.State -ne 'clean') {
    Write-Host "[WARN] Git provenance: source state is $($sourceState.State) at $($sourceState.Revision)"
} else {
    Write-Host "[PASS] Git provenance: $($sourceState.Revision)"
}

if ($SourceOnly) {
    Write-Host '[PASS] Source-only checks completed'
    return
}

$binaryVerifier = Join-Path $repositoryRoot 'tools\verify_win7_binary.ps1'
$serialCtlDlls = @(
    'ADVAPI32.dll',
    'COMCTL32.dll',
    'COMDLG32.dll',
    'GDI32.dll',
    'KERNEL32.dll',
    'ole32.dll',
    'SHELL32.dll',
    'USER32.dll',
    'WS2_32.dll'
)
$puttyDlls = @('ADVAPI32.dll', 'KERNEL32.dll', 'USER32.dll')

foreach ($item in @(Resolve-SerialCtlArchitectures -Architecture $Architecture)) {
    $outputDirectory = Get-SerialCtlBuildOutputDirectory -Architecture $item `
        -Configuration $Configuration -RepositoryRoot $repositoryRoot
    $serialCtlPath = Join-Path $outputDirectory 'serialctl.exe'
    & $binaryVerifier -Path $serialCtlPath -Architecture $item -Subsystem GUI `
        -ExpectedProductVersion $version.FileVersion -AllowedDll $serialCtlDlls

    foreach ($program in @('plink.exe', 'psftp.exe')) {
        $programPath = Join-Path $outputDirectory $program
        & $binaryVerifier -Path $programPath -Architecture $item -Subsystem CUI -AllowedDll $puttyDlls
        $vendorPath = Join-Path $repositoryRoot "third_party\putty\prebuilt\$item\$program"
        $outputHash = (Get-FileHash -LiteralPath $programPath -Algorithm SHA256).Hash
        $vendorHash = (Get-FileHash -LiteralPath $vendorPath -Algorithm SHA256).Hash
        Assert-SerialCtlCondition -Condition ($outputHash -eq $vendorHash) `
            -Message "Built $item $program does not match the pinned runtime input"
    }

    $testPrograms = @{
        'shared_serial_protocol_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'tcp_connection_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'terminal_model_test.exe' = @('KERNEL32.dll')
        'terminal_decoder_test.exe' = @('KERNEL32.dll')
        'putty_host_key_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll')
        'multi_serial_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'serial_share_backpressure_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'sftp_model_test.exe' = @('KERNEL32.dll')
    }
    foreach ($testName in $testPrograms.Keys) {
        & $binaryVerifier -Path (Join-Path $outputDirectory $testName) -Architecture $item `
            -Subsystem CUI -AllowedDll $testPrograms[$testName]
    }
}

Write-Host "[PASS] Binary compatibility checks completed: $Architecture / $Configuration"
Write-Host '[NOT RUN] Windows 7 SP1 hardware/VM, serial adapter, SSH/SFTP server, and long-running field tests'
