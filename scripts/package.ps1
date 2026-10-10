[CmdletBinding()]
param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',

    [switch]$SkipCheck
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

function Assert-PackageFileSet {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string[]]$Expected
    )

    $actual = @(Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
        Get-SerialCtlRelativePath -BasePath $Root -Path $_.FullName
    } | Sort-Object)
    $expectedSorted = @($Expected | Sort-Object)
    $difference = @(Compare-Object -ReferenceObject $expectedSorted -DifferenceObject $actual)
    if ($difference.Count -gt 0) {
        throw "Package file allowlist mismatch under $Root`: $($difference | Out-String)"
    }
}

function Copy-SerialCtlPackageFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Package input is missing: $Source"
    }
    $destinationDirectory = Split-Path $Destination -Parent
    if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

$repositoryRoot = Get-SerialCtlRepositoryRoot
$version = Get-SerialCtlVersion -RepositoryRoot $repositoryRoot
$cmakePath = Get-RequiredCommandPath -Name 'cmake.exe'
$binaryVerifier = Join-Path $repositoryRoot 'tools\verify_win7_binary.ps1'

if (-not $SkipCheck) {
    & (Join-Path $PSScriptRoot 'check.ps1') -Architecture All -Configuration $Configuration
}

$buildRoot = Get-SerialCtlBuildRoot -RepositoryRoot $repositoryRoot
$releaseWork = Get-SerialCtlReleaseWorkDirectory -RepositoryRoot $repositoryRoot
Remove-SerialCtlSafeDirectory -Path $releaseWork -AllowedParent $buildRoot

$bundleName = "SerialCtl-$($version.Display)"
$stageRoot = Join-Path $releaseWork 'stage'
$bundleRoot = Join-Path $stageRoot $bundleName
$outRoot = Join-Path $releaseWork 'out'
$verifyRoot = Join-Path $releaseWork 'verify'
New-Item -ItemType Directory -Path $bundleRoot, $outRoot | Out-Null

$runtimeRelativePaths = New-Object System.Collections.Generic.List[string]
foreach ($architecture in @('x64')) {
    $buildDirectory = Get-SerialCtlBuildDirectory -Architecture $architecture -RepositoryRoot $repositoryRoot
    $runtimeRelative = "."
    $runtimeDestination = Join-Path $bundleRoot ($runtimeRelative.Replace('/', '\'))
    New-Item -ItemType Directory -Path $runtimeDestination -Force | Out-Null
    Invoke-SerialCtlExternalCommand -FilePath $cmakePath -Description "Install $architecture runtime into package staging" -Arguments @(
        '--install', $buildDirectory,
        '--config', $Configuration,
        '--prefix', $runtimeDestination,
        '--component', 'Runtime'
    )
    foreach ($program in @('serialctl.exe', 'plink.exe', 'psftp.exe')) {
        $runtimeRelativePaths.Add($program)
    }
}

$copyMap = [ordered]@{
    'docs/package/README.md' = 'README.md'
    "docs/package/RELEASE-NOTES-$($version.Display).md" = 'RELEASE-NOTES.md'
    'tools/serialctl_client.py' = 'serialctl_client.py'
    'tools/serialctl_api.py' = 'serialctl_api.py'
    'tools/serialctl_ws.py' = 'serialctl_ws.py'
    'docs/package/AI-API.md' = 'AI-API.md'
    'docs/package/POWER.md' = 'POWER.md'
    'third_party/json/LICENSE' = 'JSON-LICENSE.txt'
    'docs/package/AI-SERIAL.md' = 'AI-SERIAL.md'
    'VERSION' = 'VERSION'
    'THIRD-PARTY-NOTICES.md' = 'THIRD-PARTY-NOTICES.md'
    'docs/testing/WIN7-HARDWARE-TEST.md' = 'WIN7-HARDWARE-TEST.md'
    'docs/testing/MANUAL-REGRESSION.md' = 'MANUAL-REGRESSION.md'
    'third_party/putty/LICENCE' = 'PUTTY-LICENCE.txt'
    'third_party/yy-thunks/LICENSE' = 'YY-THUNKS-LICENSE.txt'
}
$commandExamples = @(Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'docs\package') -Filter '*.txt' -File)
if ($commandExamples.Count -ne 1) {
    throw 'docs/package must contain exactly one command-example TXT file'
}
$commandExampleRelative = Get-SerialCtlRelativePath -BasePath $repositoryRoot -Path $commandExamples[0].FullName
$copyMap[$commandExampleRelative] = "$($commandExamples[0].Name)"
foreach ($sourceRelative in $copyMap.Keys) {
    $destinationRelative = [string]$copyMap[$sourceRelative]
    Copy-SerialCtlPackageFile -Source (Join-Path $repositoryRoot $sourceRelative) `
        -Destination (Join-Path $bundleRoot ($destinationRelative.Replace('/', '\')))
}

$driverRoot = Join-Path $bundleRoot 'drivers/ni-visa18'
& (Join-Path $PSScriptRoot 'stage-visa-driver.ps1') -Destination $driverRoot
$driverManifest = Get-Content (Join-Path $repositoryRoot 'third_party/ni-visa/files.json') -Raw | ConvertFrom-Json
$driverPaths = @($driverManifest.files | ForEach-Object { 'drivers/ni-visa18/' + $_.path })

$sourceState = Get-SerialCtlSourceState -RepositoryRoot $repositoryRoot
$payloadRecords = @()
foreach ($architecture in @('x64')) {
    $payloadFiles = @()
    foreach ($program in @('serialctl.exe', 'plink.exe', 'psftp.exe')) {
        $relativePath = $program
        $fullPath = Join-Path $bundleRoot ($relativePath.Replace('/', '\'))
        $item = Get-Item -LiteralPath $fullPath
        $payloadFiles += [ordered]@{
            path = $relativePath
            size = [long]$item.Length
            sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $payloadRecords += [ordered]@{
        os = 'windows'
        architecture = $architecture
        implementation = 'native-cpp17-win32'
        minimum_os = 'Windows 7 SP1'
        directory = "."
        files = $payloadFiles
    }
}

$dependencyManifest = Get-Content -LiteralPath (Join-Path $repositoryRoot 'third_party\manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$manifest = [ordered]@{
    schema_version = 1
    product = 'SerialCtl'
    version = $version.Display
    semantic_version = $version.Semantic
    package = [ordered]@{
        kind = 'portable-bundle'
        archive = Get-SerialCtlArchiveName -RepositoryRoot $repositoryRoot
        generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    source = [ordered]@{
        revision = $sourceState.Revision
        state = $sourceState.State
    }
    build = [ordered]@{
        configuration = $Configuration
        runtime = 'static-msvc-mt'
        pe_subsystem_version = '6.01'
    }
    optional_driver = [ordered]@{ name = "NI-VISA Runtime"; version = "18.0"; source_url = $driverManifest.url; source_archive_sha256 = $driverManifest.sha256 }
    payloads = $payloadRecords
    dependencies = @($dependencyManifest.dependencies | ForEach-Object {
        [ordered]@{ name = $_.name; version = $_.version }
    })
    verification = [ordered]@{
        automated_tests = 'passed-before-packaging'
        archive_reopened = $true
        windows_7_hardware = 'not-run'
    }
}
$manifestPath = Join-Path $bundleRoot 'MANIFEST.json'
$manifestJson = $manifest | ConvertTo-Json -Depth 10
Write-SerialCtlUtf8NoBom -Path $manifestPath -Content ($manifestJson + "`r`n")

$expectedBeforeChecksums = @($runtimeRelativePaths) + @($copyMap.Values | ForEach-Object { [string]$_ }) + @('MANIFEST.json') + @($driverPaths)
Assert-PackageFileSet -Root $bundleRoot -Expected $expectedBeforeChecksums

$checksumLines = @(Get-ChildItem -LiteralPath $bundleRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relativePath = Get-SerialCtlRelativePath -BasePath $bundleRoot -Path $_.FullName
    $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relativePath"
})
$checksumPath = Join-Path $bundleRoot 'SHA256SUMS.txt'
Write-SerialCtlUtf8NoBom -Path $checksumPath -Content (($checksumLines -join "`r`n") + "`r`n")

$expectedFiles = @($expectedBeforeChecksums) + @('SHA256SUMS.txt')
Assert-PackageFileSet -Root $bundleRoot -Expected $expectedFiles

$candidateArchive = Get-SerialCtlCandidateArchivePath -RepositoryRoot $repositoryRoot
if (Test-Path -LiteralPath $candidateArchive) {
    Remove-Item -LiteralPath $candidateArchive -Force
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($bundleRoot, $candidateArchive, [System.IO.Compression.CompressionLevel]::Optimal, $false)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($candidateArchive)
try {
    $entryNames = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($entryName in $entryNames) {
        if ($entryName -match '(^|/)\.\.(/|$)' -or `
            $entryName -match '^/' -or $entryName -match '^[A-Za-z]:') {
            throw "Unsafe ZIP entry: $entryName"
        }
    }
    $duplicates = @($entryNames | Group-Object { $_.ToLowerInvariant() } | Where-Object { $_.Count -gt 1 })
    if ($duplicates.Count -gt 0) {
        throw "Duplicate ZIP entries: $($duplicates.Name -join ', ')"
    }
    $fileEntries = @($zip.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) } | ForEach-Object {
        $_.FullName.Replace('\', '/')
    } | Sort-Object)
    $expectedEntries = @($expectedFiles | ForEach-Object { "$_" } | Sort-Object)
    $entryDifference = @(Compare-Object -ReferenceObject $expectedEntries -DifferenceObject $fileEntries)
    if ($entryDifference.Count -gt 0) {
        throw "ZIP entry allowlist mismatch: $($entryDifference | Out-String)"
    }
} finally {
    $zip.Dispose()
}

Expand-Archive -LiteralPath $candidateArchive -DestinationPath $verifyRoot
$verifiedBundleRoot = $verifyRoot
Assert-PackageFileSet -Root $verifiedBundleRoot -Expected $expectedFiles

foreach ($relativePath in $expectedFiles) {
    $stagedPath = Join-Path $bundleRoot ($relativePath.Replace('/', '\'))
    $verifiedPath = Join-Path $verifiedBundleRoot ($relativePath.Replace('/', '\'))
    $stagedHash = (Get-FileHash -LiteralPath $stagedPath -Algorithm SHA256).Hash
    $verifiedHash = (Get-FileHash -LiteralPath $verifiedPath -Algorithm SHA256).Hash
    if ($stagedHash -ne $verifiedHash) {
        throw "Reopened package differs from staging: $relativePath"
    }
}

$checksumRecords = @{}
foreach ($line in @(Get-Content -LiteralPath (Join-Path $verifiedBundleRoot 'SHA256SUMS.txt') -Encoding UTF8)) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') {
        throw "Invalid SHA256SUMS record: $line"
    }
    if ($checksumRecords.ContainsKey($Matches[2])) {
        throw "Duplicate SHA256SUMS path: $($Matches[2])"
    }
    $checksumRecords[$Matches[2]] = $Matches[1]
}
$checksumExpected = @($expectedFiles | Where-Object { $_ -ne 'SHA256SUMS.txt' } | Sort-Object)
$checksumActual = @($checksumRecords.Keys | Sort-Object)
$checksumDifference = @(Compare-Object -ReferenceObject $checksumExpected -DifferenceObject $checksumActual)
if ($checksumDifference.Count -gt 0) {
    throw "SHA256SUMS coverage mismatch: $($checksumDifference | Out-String)"
}
foreach ($relativePath in $checksumActual) {
    $fullPath = Join-Path $verifiedBundleRoot ($relativePath.Replace('/', '\'))
    $actualHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $checksumRecords[$relativePath]) {
        throw "SHA256SUMS verification failed: $relativePath"
    }
}

$serialCtlDlls = @('ADVAPI32.dll', 'COMCTL32.dll', 'COMDLG32.dll', 'GDI32.dll', 'gdiplus.dll', 'KERNEL32.dll', 'ole32.dll', 'SHELL32.dll', 'USER32.dll', 'WS2_32.dll')
$puttyDlls = @('ADVAPI32.dll', 'KERNEL32.dll', 'USER32.dll')
foreach ($architecture in @('x64')) {
    $runtimeRoot = Join-Path $verifiedBundleRoot "."
    & $binaryVerifier -Path (Join-Path $runtimeRoot 'serialctl.exe') -Architecture $architecture `
        -Subsystem GUI -ExpectedProductVersion $version.FileVersion -AllowedDll $serialCtlDlls
    foreach ($program in @('plink.exe', 'psftp.exe')) {
        $verifiedProgram = Join-Path $runtimeRoot $program
        & $binaryVerifier -Path $verifiedProgram -Architecture $architecture -Subsystem CUI -AllowedDll $puttyDlls
        $vendorProgram = Join-Path $repositoryRoot "third_party\putty\prebuilt\$architecture\$program"
        if ((Get-FileHash -LiteralPath $verifiedProgram -Algorithm SHA256).Hash -ne `
            (Get-FileHash -LiteralPath $vendorProgram -Algorithm SHA256).Hash) {
            throw "Packaged $architecture $program does not match the pinned runtime"
        }
    }
}

$verifiedManifest = Get-Content -LiteralPath (Join-Path $verifiedBundleRoot 'MANIFEST.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($verifiedManifest.product -ne 'SerialCtl' -or $verifiedManifest.version -ne $version.Display) {
    throw 'Reopened MANIFEST.json does not identify the expected product/version'
}

$archiveHash = (Get-FileHash -LiteralPath $candidateArchive -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "[PASS] Candidate archive was reopened and verified: $candidateArchive"
Write-Host "[PASS] Candidate SHA-256: $archiveHash"
Write-Output $candidateArchive
