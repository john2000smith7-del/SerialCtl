[CmdletBinding()]
param(
    [ValidateSet('x64', 'All')]
    [string]$Architecture = 'All',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [ValidateSet('All', 'Fast', 'Integration', 'Full', 'Module')]
    [string]$Scope = 'All',

    [string]$Module = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$labelArgs = @()
switch ($Scope) {
    'Fast'        { $labelArgs = @('-L', '^tier:fast$') }
    'Integration' { $labelArgs = @('-L', '^tier:(fast|integration)$') }
    'Full'        { $labelArgs = @('-L', '^tier:(fast|integration|desktop_full)$') }
    'Module' {
        if ([string]::IsNullOrWhiteSpace($Module)) {
            throw "-Scope Module requires a non-empty -Module name (e.g. -Module tftp)."
        }
        $escapedModule = [regex]::Escape($Module.Trim())
        $labelArgs = @('-L', "^mod:$escapedModule$")
    }
    default { }
}

$repositoryRoot = Get-SerialCtlRepositoryRoot
$ctestPath = Get-RequiredCommandPath -Name 'ctest.exe'
# Native tests write observations during CTest, before the finally block.
if ($env:SERIALCTL_EVIDENCE_DIR) {
    New-Item -ItemType Directory -Path $env:SERIALCTL_EVIDENCE_DIR -Force | Out-Null
}

foreach ($item in @(Resolve-SerialCtlArchitectures -Architecture $Architecture)) {
    $buildDirectory = Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot
    $testFile = Join-Path $buildDirectory 'CTestTestfile.cmake'
    if (-not (Test-Path -LiteralPath $testFile -PathType Leaf)) {
        throw "Tests have not been configured for ${item}: $testFile"
    }

    try {
    $ctestArgs = @(
        '--test-dir', $buildDirectory,
        '-C', $Configuration,
        '--output-on-failure',
        '--no-tests=error'
    ) + $labelArgs
    Invoke-SerialCtlExternalCommand -FilePath $ctestPath -Description "Test $item $Configuration ($Scope)" -Arguments $ctestArgs
    } finally {
        if($env:SERIALCTL_EVIDENCE_DIR){
            New-Item -ItemType Directory -Path $env:SERIALCTL_EVIDENCE_DIR -Force | Out-Null
            $testLog=Join-Path $buildDirectory 'Testing/Temporary/LastTest.log'
            if(Test-Path -LiteralPath $testLog){Copy-Item $testLog (Join-Path $env:SERIALCTL_EVIDENCE_DIR ('ctest-'+$item+'.txt')) -Force}
            $loggerEvidence=Join-Path $env:SERIALCTL_EVIDENCE_DIR 'logger-export-native.json'
            if(Test-Path -LiteralPath $loggerEvidence){
                $observation=Get-Content -LiteralPath $loggerEvidence -Raw -Encoding UTF8 | ConvertFrom-Json | ConvertTo-Json -Depth 10 -Compress
                Write-Host ('::notice title=Native logger export evidence::'+$observation)
            }
            foreach ($record in @('ssh-lifecycle.json', 'terminal-preview.json', 'serial-realtime-display.json', 'sftp-controller.json')) {
                $path = Join-Path $env:SERIALCTL_EVIDENCE_DIR $record
                if (Test-Path -LiteralPath $path) {
                    $observation = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json | ConvertTo-Json -Depth 10 -Compress
                    Write-Host ('::notice title=Native regression evidence::' + $record + ' ' + $observation)
                }
            }
        }
    }

}

Write-Host "[PASS] SerialCtl tests completed: $Architecture / $Configuration"
