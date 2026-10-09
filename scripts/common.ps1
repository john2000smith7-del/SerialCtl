Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SerialCtlRepositoryRoot {
    $root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt') -PathType Leaf)) {
        throw "SerialCtl repository root could not be resolved from $PSScriptRoot"
    }
    return $root
}

function Get-SerialCtlVersion {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    $versionPath = Join-Path $RepositoryRoot 'VERSION'
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw "VERSION is missing: $versionPath"
    }

    $records = @{}
    foreach ($line in @(Get-Content -LiteralPath $versionPath -Encoding UTF8)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^(PROJECT_VERSION|CPP_VERSION)=(V[0-9]+\.[0-9]+\.[0-9]+)$') {
            throw "VERSION contains an unsupported record: $line"
        }
        if ($records.ContainsKey($Matches[1])) {
            throw "VERSION contains duplicate $($Matches[1]) records"
        }
        $records[$Matches[1]] = $Matches[2]
    }

    if (-not $records.ContainsKey('PROJECT_VERSION') -or -not $records.ContainsKey('CPP_VERSION')) {
        throw 'VERSION must contain PROJECT_VERSION and CPP_VERSION records'
    }
    if ($records['PROJECT_VERSION'] -ne $records['CPP_VERSION']) {
        throw 'PROJECT_VERSION and CPP_VERSION must match'
    }

    $displayVersion = [string]$records['PROJECT_VERSION']
    $semanticVersion = $displayVersion.Substring(1)
    return [pscustomobject]@{
        Display     = $displayVersion
        Semantic    = $semanticVersion
        FileVersion = "$semanticVersion.0"
    }
}

function Resolve-SerialCtlArchitectures {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('x64', 'All')]
        [string]$Architecture
    )

    if ($Architecture -eq 'All') {
        return @('x64')
    }
    return @($Architecture)
}

function Get-SerialCtlCMakePlatform {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('x64')]
        [string]$Architecture
    )

    return 'x64'
}

function Get-SerialCtlBuildRoot {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )
    return (Join-Path $RepositoryRoot 'build')
}

function Get-SerialCtlBuildDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('x64')]
        [string]$Architecture,

        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    $platformRoot = Join-Path (Get-SerialCtlBuildRoot -RepositoryRoot $RepositoryRoot) (Join-Path 'windows' $Architecture)
    return (Join-Path $platformRoot 'cmake')
}

function Get-SerialCtlBuildOutputDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('x64')]
        [string]$Architecture,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
        [string]$Configuration,

        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    return (Join-Path (Get-SerialCtlBuildDirectory -Architecture $Architecture -RepositoryRoot $RepositoryRoot) $Configuration)
}

function Get-SerialCtlReleaseWorkDirectory {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )
    return (Join-Path (Get-SerialCtlBuildRoot -RepositoryRoot $RepositoryRoot) 'release')
}

function Get-SerialCtlArchiveName {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    $version = Get-SerialCtlVersion -RepositoryRoot $RepositoryRoot
    return "SerialCtl-$($version.Display)-Win7-x64.zip"
}

function Get-SerialCtlCandidateArchivePath {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    return (Join-Path (Join-Path (Get-SerialCtlReleaseWorkDirectory -RepositoryRoot $RepositoryRoot) 'out') (Get-SerialCtlArchiveName -RepositoryRoot $RepositoryRoot))
}

function Get-RequiredCommandPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) {
        throw "Required command is not available: $Name"
    }
    if ($command.CommandType -eq 'Application') {
        return $command.Source
    }
    return $command.Path
}

function Get-SerialCtlDumpbinPath {
    $existing = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $existing) {
        return $existing.Source
    }

    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswherePath -PathType Leaf)) {
        throw 'dumpbin.exe was not found and vswhere.exe is unavailable'
    }

    $matches = @(& $vswherePath -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' 2>$null)
    if ($LASTEXITCODE -ne 0 -or $matches.Count -eq 0) {
        throw 'A Visual Studio C++ installation containing dumpbin.exe was not found'
    }

    $path = ([string]$matches[0]).Trim()
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Visual Studio reported a missing dumpbin.exe: $path"
    }
    return $path
}

function Invoke-SerialCtlExternalCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Write-Host "==> $Description"
    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Description failed with exit code $exitCode"
    }
}

function Get-SerialCtlVisualStudioGenerator {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CMakePath
    )

    $helpOutput = @(& $CMakePath --help 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Unable to query CMake generators (exit code $exitCode)"
    }

    foreach ($lineObject in $helpOutput) {
        $line = [string]$lineObject
        if ($line -match '^\s*\*?\s*(Visual Studio [0-9]+ [0-9]+)\s+=') {
            return $Matches[1]
        }
    }
    throw 'No Visual Studio CMake generator with x64 support was found'
}

function Test-SerialCtlPathWithin {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Parent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $prefix = $fullParent + [System.IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-SerialCtlSafeDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$AllowedParent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($AllowedParent)
    if (-not (Test-SerialCtlPathWithin -Path $fullPath -Parent $fullParent)) {
        throw "Refusing to remove a directory outside the approved parent: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        $item = Get-Item -LiteralPath $fullPath -Force
        if (-not $item.PSIsContainer) {
            throw "Refusing to remove a non-directory path as a tree: $fullPath"
        }
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

function Write-SerialCtlUtf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Content
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Get-SerialCtlRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $base = [System.IO.Path]::GetFullPath($BasePath).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-SerialCtlPathWithin -Path $fullPath -Parent $base)) {
        throw "$fullPath is not inside $base"
    }
    return $fullPath.Substring($base.Length + 1).Replace('\', '/')
}

function Get-SerialCtlSourceState {
    param(
        [string]$RepositoryRoot = (Get-SerialCtlRepositoryRoot)
    )

    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $gitCommand) {
        return [pscustomobject]@{ Revision = $null; State = 'unversioned' }
    }

    $topLevel = @(& $gitCommand.Source -C $RepositoryRoot rev-parse --show-toplevel 2>$null)
    if ($LASTEXITCODE -ne 0 -or $topLevel.Count -eq 0) {
        return [pscustomobject]@{ Revision = $null; State = 'unversioned' }
    }

    $resolvedTopLevel = [System.IO.Path]::GetFullPath(([string]$topLevel[0]).Trim())
    $resolvedRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
    if (-not $resolvedTopLevel.Equals($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{ Revision = $null; State = 'unversioned' }
    }

    $revisionOutput = @(& $gitCommand.Source -C $RepositoryRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or $revisionOutput.Count -eq 0) {
        return [pscustomobject]@{ Revision = $null; State = 'unversioned' }
    }

    # bin is a verified release output, not source input. Exclude it when
    # recording whether the source revision used for a package was clean.
    $statusOutput = @(& $gitCommand.Source -C $RepositoryRoot status --porcelain `
        --untracked-files=normal -- . ':(exclude)bin/*' 2>$null)
    if ($LASTEXITCODE -ne 0) {
        return [pscustomobject]@{ Revision = ([string]$revisionOutput[0]).Trim(); State = 'unknown' }
    }

    $state = 'clean'
    if ($statusOutput.Count -gt 0) {
        $state = 'dirty'
    }
    return [pscustomobject]@{
        Revision = ([string]$revisionOutput[0]).Trim()
        State    = $state
    }
}
