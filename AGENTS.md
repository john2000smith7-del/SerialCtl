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
- `bin/` contains only the single latest verified release archive. Never copy unverified build output directly into `bin/`.
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
- This is an accepted delivery requirement, not a claim that V1.10.0 supports the new format. Implement and test the ZIP-based signed format in the client and release tooling before claiming it works. The first supporting client requires manual deployment once.
- Before any future release, verify that a matching signing key is available to the authorized signing process without exposing its contents. Deliver a genuinely signed `update.json` generated from the exact verified ZIP, and give both download links. A checksum sidecar or unsigned JSON does not satisfy the signature requirement. If signing is unavailable, report the concrete blocker rather than publishing an incomplete or falsely signed two-file delivery.
