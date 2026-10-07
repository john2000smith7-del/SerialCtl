[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64', 'All')]
    [string]$Architecture = 'All',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-SerialCtlRepositoryRoot
$ctestPath = Get-RequiredCommandPath -Name 'ctest.exe'

foreach ($item in @(Resolve-SerialCtlArchitectures -Architecture $Architecture)) {
    $buildDirectory = Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot
    $testFile = Join-Path $buildDirectory 'CTestTestfile.cmake'
    if (-not (Test-Path -LiteralPath $testFile -PathType Leaf)) {
        throw "Tests have not been configured for ${item}: $testFile"
    }

    Invoke-SerialCtlExternalCommand -FilePath $ctestPath -Description "Test $item $Configuration" -Arguments @(
        '--test-dir', $buildDirectory,
        '-C', $Configuration,
        '--output-on-failure',
        '--no-tests=error'
    )
}

Write-Host "[PASS] SerialCtl tests completed: $Architecture / $Configuration"
