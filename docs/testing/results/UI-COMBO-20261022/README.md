# Shared dropdown repaint regression — 2026-10-22

## Cause and change

Baseline: `ef00718` on `feature/ai-assistant-v1`. Native COMBOBOX selection and focus transitions can paint synchronously outside `WM_PAINT`. The shared field subclass previously restored only some transitions asynchronously, and the collapsed `ODS_COMBOBOXEDIT` owner-draw callback returned without painting. Native rectangular pixels could remain over the themed rounded field until focus loss or another repaint.

`UiStyle.h` now uses one buffered renderer for collapsed combo paint, print and owner drawing. After native input/selection transitions it synchronously restores the affected control, preserving native input and notifications. Hover restoration runs on entry/exit, not every mouse move. Adding individual list entries does not force a field repaint.

The common path serves AI model/target fields, Settings encoding/line-ending fields, serial connection parameters and power controls. No server, protocol, device operation or credential behavior changed.

## Verification

- The new `ui_combo_paint` test built against the unchanged baseline header fails immediately after keyboard selection with focus retained; see `baseline.txt`.
- The updated application and tests cross-compile for x64. The application PE retains subsystem 6.01. This smoke build uses LLVM-MinGW; production MSVC `/MT` and YY-Thunks settings remain unchanged.
- `ai_pane`, `ui_buffered_paint` and `ui_combo_paint` pass sequentially under Wine 8/Xvfb; see `tests.txt`.
- The combo test compares visible pixels immediately after native transitions with a forced redraw, before processing queued paint or moving focus. It exercises plain/owner-drawn dropdowns, keyboard and popup mouse-message selection, programmatic and reentrant updates, hover across the arrow, enabled/focus states, both palettes, 96/144/192 renderer scaling and repeated-paint GDI lifetime. Renderer scaling is not certification of OS DPI behavior.
- Visually inspected the actual main window and Settings fields in both palettes, serial parameter fields and power mode field under Wine. Selection retained its rounded border and focus outline. No hardware connection or power action was performed. Cross-resource static Chinese text has an existing encoding limitation in this environment.
- Source/version/dependency checks pass. The source-check script's Git provenance step was not run because this Linux environment has `git`, not its expected `git.exe`.

An earlier run started the actual application concurrently in the same Wine prefix: `ai_pane` printed success but its runner timed out during exit. It passed in isolation afterward (`isolated-ai-pane.txt`), and all three tests passed in the final sequential run with the application closed.

Real Windows 7/10/11 validation, physical mouse interaction on Windows, real AI/SSH servers and serial hardware were not tested here. These checks establish the reproduced retained-pixel regression and its fix in Wine; they do not establish that every transient display frame is flicker-free on all Windows systems. No package was published for this change.
