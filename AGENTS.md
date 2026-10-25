# SerialCtl development rules

## Context order

Before changing the project, read:

1. `AGENTS.md`
2. `README.md`
3. Relevant documents under `docs/architecture/` and `docs/testing/`
4. Related source code and tests

## Project structure

- `VERSION` is the only machine-readable product version source.
- `CMakeLists.txt` at the repository root is the production build entry.
- `src/` contains the native production application, grouped by responsibility.
- `tests/unit/` contains deterministic model tests; `tests/integration/` contains controlled socket or process integration tests.
- `legacy/` is historical reference only. It must not be built, tested, or packaged as the production application.
- `third_party/` contains pinned dependency inputs, licenses, source archives, patches, and required prebuilt runtime tools.
- `build/` contains disposable intermediate state only.
- `bin/` contains only the latest verified full release ZIP and, once signed, its matching `update.json`. Never copy unverified build output directly into `bin/`.
- Create new directories only when they have a real responsibility. Do not add placeholder directories or `.gitkeep` files.

## Platform and implementation

- SerialCtl must continue to support Windows 7 SP1, Windows 10, and Windows 11.
- The production application is native C++17/Win32 and must not add a .NET runtime dependency.
- Produce x64 builds only (user-approved release policy), the Windows 7 subsystem version 6.01, `/MT`, and YY-Thunks compatibility.
- Do not replace the bundled patched PuTTY programs without reviewing the source version, patches, licenses, Win7 imports, runtime outputs, and SSH/SFTP behavior.
- Preserve compatibility code unless its removal is explicitly approved.

## UI rules

- Every UI change must follow `docs/architecture/UI-DESIGN-GUIDE.md`. Do not introduce one-off colors, spacing, radii, control heights, or modal layouts.
- New buttons must use an existing button role: primary, secondary, text, icon, or danger. Extend the design guide first if a new role is genuinely required.
- New dialogs must use the shared compact grid, the same field styling, the same footer alignment, and content-dependent height.
- Both dark and light themes are required. Verify hover, pressed, focus, checked, and disabled states.
- Terminal selection copies on completion and mouse right-click pastes once. Keep terminal utility actions in the top Settings entry, with global and current-session scope clearly labeled; preserve SFTP and text-edit context menus.

## Tests and release safety

- Use `scripts/check.ps1`, `scripts/test.ps1`, and `scripts/build.ps1` for normal validation.
- Unit and integration tests are mandatory for a release. Hardware and long-running tests must report honestly when they were not run.
- After UI changes, build x64, retain subsystem version 6.01, check for post-Windows-7 imports, and visually inspect the main window plus every changed dialog.
- A release must use an explicit package allowlist, be reopened and verified after compression, and contain no development source (except the explicitly allowlisted Python runtime client), PDB, OBJ, CMake cache, test executable, local configuration, secret, or Git metadata.
- The portable ZIP has no enclosing directory; the x64 executables and runtime documentation are directly at its root; each `serialctl.exe` must remain beside the matching `plink.exe` and `psftp.exe`.
- Do not create a remote repository, push, publish a release, or alter remote state without explicit user confirmation.

## Required future release delivery

- The user requires every future version delivery to include exactly two deployable files: the verified full `SerialCtl-V<version>-Win7-x64.zip` and its matching `update.json`, with the digital signature embedded in that JSON. The user uploads these two files to the existing static update directory. Do not ask the user to generate metadata, run Python, unpack a deployment bundle, or maintain a separate `.sig`/`files` tree.
- Preserve startup automatic checking/downloading and the existing `[update] url=` configuration in `update.ini`. A new settings page or manual update button is not part of this request. Preserve idle installation, data/config/driver protection and failure recovery.
- V2.0.0 implements the ZIP-based signed format; V1.10.0 and earlier do not support it. Test both client and release tooling before every formal delivery. The first supporting client requires manual deployment once.
- Before any future release, verify that a matching signing key is available to the authorized signing process without exposing its contents. Deliver a genuinely signed `update.json` generated from the exact verified ZIP, and give both download links. A checksum sidecar or unsigned JSON does not satisfy the signature requirement. If signing is unavailable, report the concrete blocker rather than publishing an incomplete or falsely signed two-file delivery.
- On 2026-10-25 the user authorized searching for the previous identity and replacing it only if recovery failed. The V2.0.0 private key was absent from the current filesystem and Git history; its public fingerprint remains historical evidence. V2.0.4 starts the replacement identity with public SPKI SHA-256 `1b54f808072ac93c97d98d007a8e8c71d41a17834a0c5d810c614e9d75a05ae7`. Its active private key is `/workspace/.serialctl-signing/serialctl-update-v2.0.4-rsa2048.pem` (directory 0700, file 0600). Reuse it; do not silently generate or overwrite an identity when a workspace is replaced. Old clients require one manual V2.0.4 deployment; later releases reuse this identity and support full-package version jumps.
- At the user's request for durable backup, the only permitted key file in Git is `release-signing/serialctl-update-v2.0.4.encrypted.pem`, an encrypted PKCS#8 copy (AES-256-CBC, PBKDF2-HMAC-SHA256, 600000 iterations). `release-signing/identity.json` binds its digest to the embedded public key. Never read/print private key contents, commit an unencrypted key or recovery password, upload them to CI/the update server, or include any key in application releases. The owner-only recovery ZIP and password are outside Git at `/workspace/serialctl-private-recovery/`; the owner must retain a copy outside the disposable workspace. Recover with `tools/recover_update_key.py`, which verifies the trusted identity and refuses overwrite. The encrypted Git copy alone cannot be decrypted without the owner's recovery password. No external vault is configured; GitHub Secrets access currently returns HTTP 403.

- The user explicitly approved the packaged update.ini default `http://172.16.3.188/external/serial/update.json`. This versioned non-secret template is the only allowed configuration exception; do not package arbitrary local settings. Automatic updates still preserve an existing user INI.
