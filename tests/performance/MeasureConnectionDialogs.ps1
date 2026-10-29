param([Parameter(Mandatory=$true)][string]$CandidateArchive,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
# This fixture resets the test account's preferences. Never use a user's desktop.
if ($env:RUNNER_ENVIRONMENT -ne 'github-hosted') { throw 'Requires an isolated GitHub-hosted desktop.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$candidate = (Resolve-Path $CandidateArchive).Path
$baseline = Join-Path $OutputDirectory 'baseline'
New-Item -ItemType Directory -Force -Path $baseline | Out-Null
gh release download v2.0.10 --repo $env:GITHUB_REPOSITORY --pattern 'SerialCtl-V2.0.10-Win7-x64.zip' --dir $baseline
if ($LASTEXITCODE -ne 0) { throw 'Cannot download accepted baseline.' }
$old = Join-Path $baseline 'SerialCtl-V2.0.10-Win7-x64.zip'
if ((Get-FileHash $old -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd2bc0b00ca4f2917d533f0f16f23b3301b2baf259b69e0af360b17b90420be3b') {
    throw 'Baseline archive identity mismatch.'
}
foreach ($name in @('baseline', 'candidate')) {
    $zip = if ($name -eq 'baseline') { $old } else { $candidate }
    $app = Join-Path $OutputDirectory "$name/app"
    Expand-Archive -LiteralPath $zip -DestinationPath $app
    Set-Content -LiteralPath (Join-Path $app 'update.ini') -Value "[update]`r`nurl=" -Encoding ASCII
}
foreach ($theme in @('light', 'dark')) {
    foreach ($name in @('baseline', 'candidate')) {
        $config = Join-Path $env:APPDATA 'SerialCtl'
        New-Item -ItemType Directory -Force -Path $config | Out-Null
        Set-Content -LiteralPath (Join-Path $config 'settings.ini') -Value "[Layout]`r`nRightPanelCollapsed=0`r`n[Pet]`r`nEnabled=0" -Encoding ASCII
        Write-Host "DIALOG_TIMING_CASE=$name/$theme"
        & ./dialog-bench.exe (Join-Path $OutputDirectory "$name/app/serialctl.exe") $theme |
            Tee-Object -FilePath (Join-Path $OutputDirectory "$name-$theme.log")
        if ($LASTEXITCODE -ne 0) { throw "Invalid dialog sample: $name/$theme exited $LASTEXITCODE" }
    }
}
