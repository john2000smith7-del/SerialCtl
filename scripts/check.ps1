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
    'docs/package/AUTO-UPDATE.md',
    'docs/package/update.ini',
    'docs/architecture/AUTO-UPDATE.md',
    'docs/testing/AUTO-UPDATE-ACCEPTANCE.md',
    'tools/new_update_feed.py',
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

# Only the versioned, user-approved non-secret update URL is allowed in a release.
$updateTemplateLines = @(Get-Content -LiteralPath (Join-Path $repositoryRoot 'docs/package/update.ini') -Encoding UTF8 |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith(';') })
Assert-SerialCtlCondition -Condition (($updateTemplateLines -join "`n") -ceq "[update]`nurl=http://172.16.3.188/external/serial/update.json") `
    -Message 'The packaged update.ini must contain only [update] and the user-approved update URL'

# V2.0.0 is now an explicitly requested production version. Version consistency
# is checked above; historical version mentions are legitimate documentation.

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
    'gdiplus.dll',
    'KERNEL32.dll',
    'MSIMG32.dll',
    'ole32.dll',
    'SETUPAPI.dll',
    'SHELL32.dll',
    'USER32.dll',
    'WINHTTP.dll',
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
        'cancellable_io_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'OLE32.dll')
        'websocket_deadline_test.exe' = @('KERNEL32.dll', 'WS2_32.dll', 'ADVAPI32.dll')
        'sftp_controller_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'OLE32.dll')
        'ai_device_bridge_test.exe' = @('KERNEL32.dll', 'OLE32.dll', 'SHELL32.dll')
        'command_macro_test.exe' = @('KERNEL32.dll', 'OLE32.dll')
        'terminal_preview_test.exe' = @('KERNEL32.dll')
        'tool_presentation_test.exe' = @('KERNEL32.dll')
        'gui_instance_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'USER32.dll')
        'ai_pet_model_test.exe' = @('KERNEL32.dll')
        'ai_pet_pane_test.exe' = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'gdiplus.dll', 'COMCTL32.dll', 'OLE32.dll', 'MSIMG32.dll')
        'ai_protocol_test.exe' = @('KERNEL32.dll')
        'ai_device_test.exe' = @('KERNEL32.dll', 'USER32.dll', 'WS2_32.dll', 'COMCTL32.dll', 'ADVAPI32.dll', 'gdiplus.dll', 'COMDLG32.dll', 'GDI32.dll', 'OLE32.dll', 'SHELL32.dll', 'MSIMG32.dll', 'SETUPAPI.dll', 'WINHTTP.dll')
        'ui_buffered_paint_test.exe' = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'gdiplus.dll', 'OLE32.dll', 'MSIMG32.dll')
        'ui_combo_paint_test.exe' = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'gdiplus.dll', 'COMCTL32.dll', 'OLE32.dll', 'MSIMG32.dll')
        # Rounded viewport thumbs no longer load PNG artwork through OLE streams.
        'ui_dialog_viewport_test.exe' = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'gdiplus.dll', 'COMCTL32.dll', 'OLE32.dll', 'MSIMG32.dll')
        'update_client_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'WINHTTP.dll', 'WS2_32.dll')
        'update_installer_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'SHELL32.dll', 'USER32.dll')
        'power_service_test.exe' = @('KERNEL32.dll', 'SHELL32.dll')
        'power_connection_gate_test.exe' = @('KERNEL32.dll', 'SHELL32.dll', 'ADVAPI32.dll')
        'session_logger_test.exe' = @('ADVAPI32.dll', 'KERNEL32.dll', 'SHELL32.dll')
        'cmd_connection_test.exe' = @('KERNEL32.dll')
        'api_server_test.exe' = @('KERNEL32.dll', 'WS2_32.dll', 'ADVAPI32.dll')
        'shared_serial_protocol_test.exe' = @('KERNEL32.dll', 'WS2_32.dll', 'ADVAPI32.dll')
        'udp_serial_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'tcp_connection_test.exe' = @('KERNEL32.dll', 'WS2_32.dll')
        'connection_profile_test.exe' = @('KERNEL32.dll')
        'terminal_model_test.exe' = @('KERNEL32.dll')
        'terminal_decoder_test.exe' = @('KERNEL32.dll')
        'putty_host_key_test.exe' = @('KERNEL32.dll', 'ADVAPI32.dll', 'ole32.dll')
        'multi_serial_test.exe' = @('KERNEL32.dll', 'WS2_32.dll', 'ADVAPI32.dll')
        # PSAPI_VERSION=2 maps the metrics call to K32GetProcessMemoryInfo,
        # provided by KERNEL32 on Windows 7; retain the exact import check.
        'serial_share_backpressure_test.exe' = @('KERNEL32.dll', 'WS2_32.dll', 'ADVAPI32.dll')
        'sftp_model_test.exe' = @('KERNEL32.dll')
    }
    # Report every test-binary mismatch in the same build. Keep the verifier's
    # exact DLL/API/subsystem checks and fail before packaging if any fail.
    $testBinaryFailures = New-Object 'System.Collections.Generic.List[string]'
    $testBinaries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($testName in $testPrograms.Keys) {
        $testBinaries.Add(@{ Path = Join-Path $outputDirectory $testName; Dlls = $testPrograms[$testName] })
    }
    $fixtureDirectory = Join-Path (Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot) "ai-fixture\$Configuration"
    $testBinaries.Add(@{ Path = Join-Path $fixtureDirectory 'ai_client_test.exe'; Dlls = @('KERNEL32.dll', 'ADVAPI32.dll', 'ole32.dll') })
    $testBinaries.Add(@{ Path = Join-Path $fixtureDirectory 'ai_pane_test.exe'; Dlls = @('KERNEL32.dll', 'ADVAPI32.dll', 'ole32.dll', 'COMCTL32.dll', 'USER32.dll', 'GDI32.dll', 'gdiplus.dll', 'MSIMG32.dll') })
    $testBinaries.Add(@{ Path = Join-Path $fixtureDirectory 'plink.exe'; Dlls = @('KERNEL32.dll') })
    $sftpFixture = Join-Path (Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot) "sftp-fixture\$Configuration"
    $sshFixture = Join-Path (Get-SerialCtlBuildDirectory -Architecture $item -RepositoryRoot $repositoryRoot) "ssh-fixture\$Configuration"
    $testBinaries.Add(@{ Path = Join-Path $sshFixture 'ssh_lifecycle_test.exe'; Dlls = @('KERNEL32.dll', 'ADVAPI32.dll', 'OLE32.dll') })
    $testBinaries.Add(@{ Path = Join-Path $sshFixture 'plink.exe'; Dlls = @('KERNEL32.dll') })
    $testBinaries.Add(@{ Path = Join-Path $sftpFixture 'sftp_client_test.exe'; Dlls = @('KERNEL32.dll', 'ADVAPI32.dll', 'OLE32.dll') })
    foreach ($program in @('plink.exe', 'psftp.exe')) {
        $testBinaries.Add(@{ Path = Join-Path $sftpFixture $program; Dlls = @('KERNEL32.dll') })
    }
    foreach ($testBinary in $testBinaries) {
        try {
            & $binaryVerifier -Path $testBinary.Path -Architecture $item -Subsystem CUI -AllowedDll $testBinary.Dlls
        } catch {
            $testBinaryFailures.Add($_.Exception.Message)
            Write-Host ('[FAIL] ' + $_.Exception.Message)
        }
    }
    if ($testBinaryFailures.Count -gt 0) {
        throw ("Test binary compatibility checks failed:`n" + ($testBinaryFailures -join "`n"))
    }
}

Write-Host "[PASS] Binary compatibility checks completed: $Architecture / $Configuration"
Write-Host '[NOT RUN] Windows 7 SP1 hardware/VM, serial adapter, SSH/SFTP server, and long-running field tests'
