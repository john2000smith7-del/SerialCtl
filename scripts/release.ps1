[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-SerialCtlRepositoryRoot
$buildRoot = Get-SerialCtlBuildRoot -RepositoryRoot $repositoryRoot
$releaseWork = Get-SerialCtlReleaseWorkDirectory -RepositoryRoot $repositoryRoot
$binRoot = Join-Path $repositoryRoot 'bin'
$archiveName = Get-SerialCtlArchiveName -RepositoryRoot $repositoryRoot
$finalArchive = Join-Path $binRoot $archiveName

Write-Host '==> Preflight source/version/dependency checks'
if ($env:GITHUB_ACTIONS) { Write-Host '::notice title=Release stage::Source and dependency checks' }
& (Join-Path $PSScriptRoot 'check.ps1') -Architecture All -Configuration Release -SourceOnly

Write-Host '==> Clean x64 Release build'
if ($env:GITHUB_ACTIONS) { Write-Host '::notice title=Release stage::Clean MSVC x64 Release build' }
& (Join-Path $PSScriptRoot 'build.ps1') -Architecture All -Configuration Release -Clean

Write-Host '==> x64 automated tests'
if ($env:GITHUB_ACTIONS) { Write-Host '::notice title=Release stage::MSVC build passed; running all CTest cases' }
& (Join-Path $PSScriptRoot 'test.ps1') -Architecture All -Configuration Release

Write-Host '==> PE, version, dependency, and Win7 compatibility checks'
if ($env:GITHUB_ACTIONS) { Write-Host '::notice title=Release stage::All CTest passed; checking PE/Win7 imports' }
& (Join-Path $PSScriptRoot 'check.ps1') -Architecture All -Configuration Release

Write-Host '==> Explicit-allowlist package and reopen verification'
if ($env:GITHUB_ACTIONS) { Write-Host '::notice title=Release stage::PE checks passed; packaging and reopening full ZIP' }
& (Join-Path $PSScriptRoot 'package.ps1') -Configuration Release -SkipCheck
$candidateArchive = Get-SerialCtlCandidateArchivePath -RepositoryRoot $repositoryRoot
if (-not (Test-Path -LiteralPath $candidateArchive -PathType Leaf)) {
    throw "Verified candidate archive is missing: $candidateArchive"
}
$candidateHash = (Get-FileHash -LiteralPath $candidateArchive -Algorithm SHA256).Hash

Write-Host '==> Native extraction of the exact full release ZIP (temporary test signature)'
$archiveTest = Join-Path (Get-SerialCtlBuildOutputDirectory -Architecture x64 -Configuration Release -RepositoryRoot $repositoryRoot) 'update_client_test.exe'
python (Join-Path $repositoryRoot 'tests/integration/test_full_update_archive.py') $candidateArchive $archiveTest
if ($LASTEXITCODE -ne 0) {
    throw 'The full release ZIP failed native update extraction verification'
}

if (-not (Test-Path -LiteralPath $binRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $binRoot | Out-Null
}
$backupRoot = Join-Path $releaseWork 'previous-bin'
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

$previousItems = @(Get-ChildItem -LiteralPath $binRoot -Force)
foreach ($item in $previousItems) {
    if (-not (Test-SerialCtlPathWithin -Path $item.FullName -Parent $binRoot)) {
        throw "Refusing to move an unexpected bin item: $($item.FullName)"
    }
    Move-Item -LiteralPath $item.FullName -Destination $backupRoot
}

try {
    Move-Item -LiteralPath $candidateArchive -Destination $finalArchive
    $finalHash = (Get-FileHash -LiteralPath $finalArchive -Algorithm SHA256).Hash
    if ($finalHash -ne $candidateHash) {
        throw 'The archive changed while being promoted into bin'
    }

    $finalItems = @(Get-ChildItem -LiteralPath $binRoot -Force)
    if ($finalItems.Count -ne 1 -or $finalItems[0].PSIsContainer -or `
        $finalItems[0].Name -ne $archiveName) {
        throw 'bin does not contain exactly the expected verified archive'
    }
} catch {
    $promotionError = $_
    if (Test-Path -LiteralPath $finalArchive -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $candidateArchive)) {
            Move-Item -LiteralPath $finalArchive -Destination $candidateArchive
        } else {
            Remove-Item -LiteralPath $finalArchive -Force
        }
    }
    foreach ($item in @(Get-ChildItem -LiteralPath $backupRoot -Force -ErrorAction SilentlyContinue)) {
        Move-Item -LiteralPath $item.FullName -Destination $binRoot
    }
    throw $promotionError
}

Write-Host '==> Remove disposable build state after successful promotion'
Remove-SerialCtlSafeDirectory -Path $buildRoot -AllowedParent $repositoryRoot

$publishedHash = (Get-FileHash -LiteralPath $finalArchive -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "[PASS] Local release ready: $finalArchive"
Write-Host "[PASS] SHA-256: $publishedHash"
Write-Host '[NOT RUN] Windows 7 SP1 real-machine/VM and physical serial-device acceptance'
