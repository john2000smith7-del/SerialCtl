param([Parameter(Mandatory=$true)][string]$CandidateArchive,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
# This fixture resets the test account's preferences. Never use a user's desktop.
if ($env:RUNNER_ENVIRONMENT -ne 'github-hosted') { throw 'Requires an isolated GitHub-hosted desktop.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$candidate = (Resolve-Path $CandidateArchive).Path
$baseline = Join-Path $OutputDirectory 'baseline'
New-Item -ItemType Directory -Force -Path $baseline | Out-Null
gh release download v2.0.16 --repo $env:GITHUB_REPOSITORY --pattern 'SerialCtl-V2.0.16-Win7-x64.zip' --dir $baseline
if ($LASTEXITCODE -ne 0) { throw 'Cannot download accepted baseline.' }
$old = Join-Path $baseline 'SerialCtl-V2.0.16-Win7-x64.zip'
if ((Get-FileHash $old -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'c5609bcdc52753045a90a7839529fa9f9f475067f7d914dcf647030025a47a65') {
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
        $paintMode = 'first-paint'
        & ./dialog-bench.exe (Join-Path $OutputDirectory "$name/app/serialctl.exe") $theme $paintMode |
            Tee-Object -FilePath (Join-Path $OutputDirectory "$name-$theme.log")
        if ($LASTEXITCODE -ne 0) { throw "Invalid dialog sample: $name/$theme exited $LASTEXITCODE" }
    }
}
foreach ($name in @('baseline', 'candidate')) {
    $config = Join-Path $env:APPDATA 'SerialCtl'
    Set-Content -LiteralPath (Join-Path $config 'settings.ini') -Value "[Layout]`r`nRightPanelCollapsed=0`r`n[Pet]`r`nEnabled=0" -Encoding ASCII
    Write-Host "DIALOG_TIMING_CASE=$name/light/continuous-terminal"
    $paintMode = 'first-paint'
    & ./dialog-bench.exe (Join-Path $OutputDirectory "$name/app/serialctl.exe") light $paintMode traffic |
        Tee-Object -FilePath (Join-Path $OutputDirectory "$name-traffic.log")
    if ($LASTEXITCODE -ne 0) { throw "Invalid continuous-output sample: $name exited $LASTEXITCODE" }
}

python tests/performance/summarize_dialog_latency.py --log-dir $OutputDirectory --output (Join-Path $OutputDirectory 'dialog-summary.json') --require-warm-p95-ms 100
if ($LASTEXITCODE -ne 0) { throw 'Full-paint / responsiveness P95 performance acceptance failed; no samples may be discarded.' }
