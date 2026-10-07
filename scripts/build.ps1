[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'All')]
    [string]$Architecture = 'All',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-SerialCtlRepositoryRoot
$buildRoot = Get-SerialCtlBuildRoot -RepositoryRoot $repositoryRoot
$cmakePath = Get-RequiredCommandPath -Name 'cmake.exe'
$generator = Get-SerialCtlVisualStudioGenerator -CMakePath $cmakePath

foreach ($item in @(Resolve-SerialCtlArchitectures -Architecture $Architecture)) {
    $buildDirectory = Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot
    if ($Clean) {
        Remove-SerialCtlSafeDirectory -Path $buildDirectory -AllowedParent $buildRoot
    }

    $platform = Get-SerialCtlCMakePlatform -Architecture $item
    Invoke-SerialCtlExternalCommand -FilePath $cmakePath -Description "Configure $item" -Arguments @(
        '-S', $repositoryRoot,
        '-B', $buildDirectory,
        '-G', $generator,
        '-A', $platform,
        '-DBUILD_TESTING=ON'
    )
    Invoke-SerialCtlExternalCommand -FilePath $cmakePath -Description "Build $item $Configuration" -Arguments @(
        '--build', $buildDirectory,
        '--config', $Configuration,
        '--parallel'
    )
}

Write-Host "[PASS] SerialCtl build completed: $Architecture / $Configuration"
