[CmdletBinding()]
param(
    [ValidateSet('x64', 'All')]
    [string]$Architecture = 'All',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$Clean,

    # Optional production-only compilation. The default still builds all targets.
    [ValidateSet('serialctl')]
    [string]$Target
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
    $buildArguments = @(
        '--build', $buildDirectory,
        '--config', $Configuration
    )
    if ($Target) { $buildArguments += @('--target', $Target) }
    $buildArguments += '--parallel'
    Invoke-SerialCtlExternalCommand -FilePath $cmakePath -Description "Build $item $Configuration" -Arguments $buildArguments
}

Write-Host "[PASS] SerialCtl build completed: $Architecture / $Configuration"
