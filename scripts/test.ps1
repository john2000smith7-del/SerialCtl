[CmdletBinding()]
param(
    [ValidateSet('x64', 'All')]
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

    try {
    Invoke-SerialCtlExternalCommand -FilePath $ctestPath -Description "Test $item $Configuration" -Arguments @(
        '--test-dir', $buildDirectory,
        '-C', $Configuration,
        '--output-on-failure',
        '--no-tests=error'
    )
    } finally {
        if($env:SERIALCTL_EVIDENCE_DIR){
            New-Item -ItemType Directory -Path $env:SERIALCTL_EVIDENCE_DIR -Force | Out-Null
            $testLog=Join-Path $buildDirectory 'Testing/Temporary/LastTest.log'
            if(Test-Path -LiteralPath $testLog){Copy-Item $testLog (Join-Path $env:SERIALCTL_EVIDENCE_DIR ('ctest-'+$item+'.txt')) -Force}
        }
    }

}

Write-Host "[PASS] SerialCtl tests completed: $Architecture / $Configuration"
