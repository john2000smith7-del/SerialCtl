[CmdletBinding()]
param(
    [switch]$RepositoryHygiene
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-SerialCtlRepositoryRoot
$buildRoot = Get-SerialCtlBuildRoot -RepositoryRoot $repositoryRoot
Remove-SerialCtlSafeDirectory -Path $buildRoot -AllowedParent $repositoryRoot
Write-Host '[PASS] Disposable build directory is absent'

if (-not $RepositoryHygiene) {
    return
}

$generatedDirectories = @(
    'legacy\csharp\SerialCtl.Win7\bin',
    'legacy\csharp\SerialCtl.Win7\obj',
    'third_party\yy-thunks\_rels',
    'third_party\yy-thunks\build',
    'third_party\yy-thunks\package'
)
foreach ($relativePath in $generatedDirectories) {
    $fullPath = Join-Path $repositoryRoot $relativePath
    Remove-SerialCtlSafeDirectory -Path $fullPath -AllowedParent $repositoryRoot
}

$expandedPackageFiles = @(
    'third_party\yy-thunks\.signature.p7s',
    'third_party\yy-thunks\[Content_Types].xml',
    'third_party\yy-thunks\YY-Thunks.nuspec'
)
foreach ($relativePath in $expandedPackageFiles) {
    $fullPath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $relativePath))
    if (-not (Test-SerialCtlPathWithin -Path $fullPath -Parent $repositoryRoot)) {
        throw "Refusing to remove a file outside the repository: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if ($item.PSIsContainer) {
            throw "Expected a generated file but found a directory: $fullPath"
        }
        Remove-Item -LiteralPath $fullPath -Force
    }
}

$obsoleteEmptyTrees = @(
    'cpp',
    'third_party\putty\x86',
    'third_party\putty\x64'
)
foreach ($relativePath in $obsoleteEmptyTrees) {
    $fullPath = Join-Path $repositoryRoot $relativePath
    if (Test-Path -LiteralPath $fullPath -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $fullPath -Recurse -File -Force)
        if ($files.Count -gt 0) {
            throw "Refusing to remove a migrated directory that is no longer empty: $relativePath"
        }
        Remove-SerialCtlSafeDirectory -Path $fullPath -AllowedParent $repositoryRoot
    }
}

Write-Host '[PASS] Legacy compiler output, extracted dependency copies, and obsolete empty trees are absent'
