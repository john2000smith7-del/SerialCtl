# SerialCtl PuTTY builds

SerialCtl ships patched PuTTY 0.85 `plink.exe` and `psftp.exe` binaries for
x86 and x64. The repository keeps the exact upstream source archive, the
SerialCtl patches, and the verified prebuilt runtime files in separate
directories:

- `source/putty-src-0.85.zip`: upstream PuTTY 0.85 source archive.
- `patches/`: SerialCtl-specific source patches.
- `prebuilt/x86/`: verified 32-bit `plink.exe` and `psftp.exe`.
- `prebuilt/x64/`: verified 64-bit `plink.exe` and `psftp.exe`.
- `LICENCE`: unmodified PuTTY licence.

The expected SHA-256 values for these controlled files are recorded in
`third_party/manifest.json`.

## SerialCtl patches

Apply the patches in this order:

1. `patches/serialctl-resize.patch`
2. `patches/serialctl-sftp-list.patch`
3. `patches/serialctl-sftp-exact-rename.patch`
4. `patches/serialctl-sftp-utf8-local.patch`

The resize patch adds two private options used only between `serialctl.exe`
and its bundled `plink.exe`:

- `-serialctl-resize-event HANDLE`
- `-serialctl-resize-map HANDLE`

The inherited event wakes Plink's Windows event loop. The inherited shared
mapping contains two `LONG` values (columns and rows), and Plink forwards the
latest values through PuTTY's existing `backend_size` API. SerialCtl hosts
Plink through pipes rather than a Windows console, so the stock Plink binary
cannot otherwise observe terminal-size changes.

The machine-list patch adds `-serialctl-machine-list` to `psftp.exe`. In this
mode, `pwd` and `ls` emit exact remote-path bytes as hexadecimal records next
to numeric SFTP attributes. This avoids guessing names from human-readable
listings and safely handles spaces, wildcards, backslashes, quotes, and
control characters.

The exact-operation patch adds private rename, replace, remove, and rmdir
commands that bypass PSFTP wildcard expansion, `REALPATH` final-component
resolution, and "move into directory" behavior. Operations on symbolic links
therefore target the link entry itself. The patch also supports the OpenSSH
`posix-rename@openssh.com` extension for atomic replacement after upload;
unsupported servers fail without deleting the original remote file.

The UTF-8 local-path patch preserves stock PSFTP behavior by default and uses
Unicode Win32 APIs for local upload and download paths in SerialCtl's private
mode. This keeps Chinese and other paths outside the active legacy code page
usable on Windows 7.

## Reproducing the binaries

Run the following commands from the repository root in PowerShell. All
extracted source and compiler output stays under the ignored `build/`
directory. Initializing the extracted source as a temporary Git repository
ensures that `git apply` cannot target the SerialCtl source tree.

```powershell
$repoRoot = (Get-Location).Path
$puttySource = Join-Path $repoRoot 'build\putty-source-0.85'

Expand-Archive `
    -LiteralPath (Join-Path $repoRoot 'third_party\putty\source\putty-src-0.85.zip') `
    -DestinationPath $puttySource

git -C $puttySource init --quiet
git -C $puttySource apply (Join-Path $repoRoot 'third_party\putty\patches\serialctl-resize.patch')
git -C $puttySource apply (Join-Path $repoRoot 'third_party\putty\patches\serialctl-sftp-list.patch')
git -C $puttySource apply (Join-Path $repoRoot 'third_party\putty\patches\serialctl-sftp-exact-rename.patch')
git -C $puttySource apply (Join-Path $repoRoot 'third_party\putty\patches\serialctl-sftp-utf8-local.patch')

cmake -S $puttySource -B (Join-Path $repoRoot 'build\putty-x64') -A x64 `
    -DPUTTY_GSSAPI=OFF `
    '-DPUTTY_SUBSYSTEM_VERSION=6.01' `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build (Join-Path $repoRoot 'build\putty-x64') `
    --config Release --target plink psftp

cmake -S $puttySource -B (Join-Path $repoRoot 'build\putty-x86') -A Win32 `
    -DPUTTY_GSSAPI=OFF `
    '-DPUTTY_SUBSYSTEM_VERSION=6.01' `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build (Join-Path $repoRoot 'build\putty-x86') `
    --config Release --target plink psftp
```

The rebuilt files are produced under `build/putty-x64/Release/` and
`build/putty-x86/Release/`. Do not replace files under `prebuilt/` until both
architectures have passed the Windows 7 subsystem/import checks and the
corresponding hashes in `third_party/manifest.json` are updated.
