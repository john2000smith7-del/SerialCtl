# Native Windows interaction source cross-review

Reviewed 2026-10-14, read-only product review. No repository files were edited. The review read `AGENTS.md`, `README.md`, `docs/architecture/UI-DESIGN-GUIDE.md`, the relevant native source, and the real-input harnesses. `git diff 0454b51 -- src` was empty. The reviewed files have identical blob hashes at `0454b51` and in the working tree:

| File | Git blob |
| --- | --- |
| `src/app/MainWindow.cpp` | `2006a8247d3c75b9b1bf87fbb789792d6d4e4a21` |
| `src/app/UiStyle.h` | `819388b08c677cc024a512e0cf511d2e13ba7bda` |

The source references below use `/workspace/serialctl-main`. Later test commits do not change these product-source references. These findings concern the observed Windows Server 2022, 96-DPI light-theme run; they do not establish dark-theme, other-DPI, or Windows 7 results.

## 1. Failed rapid sidebar reversal: moving target is the leading hypothesis

**Observed:** `attempt-5/x64-light-desktop-user-actions.json`, operation `sidebar-rapid-reversal`, records the following:

| Event | Seconds from test start | Evidence |
| --- | --- | --- |
| Click 0 target | 18.558353 | Button bounds `[1156,173,1184,201]`; point `[1161,187]` hits HWND 66350. |
| Click 0 down / up | 18.561197 / 18.569534 | Real `mouse_event` down/up. |
| Caption changes | 18.572742 | `折叠侧栏` → `展开侧栏`. |
| Click 1 target sampled | 18.638027 | Bounds `[1098,173,1126,201]`; point `[1120,187]` hits the same HWND 66350. |
| Click 1 down / up | 18.656166 / 18.664680 | There is about 18 ms between target sampling and down, then an 8.5 ms press. |
| Caption check | 18.727773 | Caption remains `展开侧栏`; assertion fails. |
| Following geometry snapshot | 18.727886 | Button bounds are now `[1158,173,1186,201]`. |

The two points are 41 px apart; the recorded system double-click width is 4 px. This substantially weakens the earlier same-point native-double-click explanation for **this** failed input pair. It does not establish which messages Windows delivered. `WindowFromPoint` was checked before down, not at delivery or release. The test proves that the prechecked point hit the visible toggle and the ensuing input did not change its caption. It does **not** prove that both press and release landed on the button.

**Source facts (high confidence):**

- `MainWindow.cpp:2328–2329` creates a native owner-drawn Button. `ButtonSubclassProc`, `1385–1402`, handles hover/focus bookkeeping and otherwise delegates to `DefSubclassProc`; it does not stabilize or specially handle the toggle's mouse gesture.
- `MainWindow.cpp:2043–2058` invokes `SetRightPanelCollapsed` only for `BN_CLICKED`. A native `BN_DOUBLECLICKED` is not handled here. This remains a plausible explanation for older closely spaced same-position clicks, but the attempt-5 41 px separation means it should not be reused as the proven explanation for that run.
- `SetRightPanelCollapsed`, `4914–4928`, immediately flips the Boolean target and caption, records the current animated width as the new starting width, and starts/restarts a 16 ms timer. It allows reversal while animating and has no animation-time disable gate (apart from the separate power-page guard).
- `WM_TIMER`, `1959–1966`, interpolates sidebar width over 200 ms with cubic easing, calls `LayoutControls`, and invalidates the parent.
- `LayoutControls`, **`2428–2431`**, changes the toggle-position formula whenever `rightPanelAnimating_` is true. A settled expanded toggle sits at the right inner edge; during animation it instead sits at the center of the whole changing sidebar width. This creates an initial positional jump and then a moving 28 px hit target. At completion, expanding also switches back to the right-edge formula. The source behavior agrees with the much larger intermediate motion despite settled endpoints being only 6 px apart in this run.
- The button remains enabled outside the power page (`2421`) and explicitly visible (`2498`, `4945`), so clicks during animation are allowed by the product.

**Assessment:** Medium-to-high confidence that animated target motion explains the failed gesture; **root cause is not proven**. It can move off the sampled point before mouse-down or after down but before up. The current evidence cannot distinguish these cases, nor exclude another delivery/notification issue. Caption updates are synchronous in the toggle handler, so their absence is useful evidence that the desired handler did not run; it is not a trace of the native button messages.

**Candidate remedy:** Make the visible toggle's screen position independent of animated sidebar width, ideally using a stable right-side anchor for both directions and reversal. Keep the shared 28 px control size and centered 48 px collapsed rail required by the design guide; check the expanded header spacing as part of this change. Merely interpolating between the settled positions removes the large jump but still leaves a moving press target. Freezing position while the button owns mouse capture can protect down/up, but alone cannot repair the sampling-to-down race. Disabling the toggle during the 200 ms animation would remove reversal behavior and should not be used as a silent test workaround.

**Meaningful regression / diagnosis:** Preserve the failing artifact. Use actual system mouse input, track fresh bounds, and record high-resolution target/caption observations before down and after up. Add passive UI-thread message tracing in a separate diagnostic run for `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`, capture changes, and parent `WM_COMMAND` notification values; include the button rectangle and cursor position at each event. This separates a missed down, release outside the moved button, and double-click notification without replacing user input with `BM_CLICK` or `SendMessage(WM_COMMAND)`. Then test rapid reversal in both directions at several timer phases with 8, 30, and 100 ms press durations, and test an intentional stationary double-click separately. Verify every accepted click changes target, final geometry is correct after settling, hidden sidebar controls stay hidden, and real terminal output continues. Preserve the failed uninterrupted gesture even if a later recovery succeeds.

## 2. Residual toggle outlines in animation frames

**Observed:** I inspected `review-crop-0094-sidebar-collapse-animation-01.png` (derived from attempt 4) and the full `attempt-4/x64-light-desktop-0102-sidebar-expand-animation-00.png`. Both visibly contain more than one chevron/button-outline image in the sidebar header. The collapse crop shows an old partial blue outline and chevron alongside the current one. This is evidence of a captured transient visual defect, not proof that there are two native toggle controls, and it does not measure artifacts between captured frames.

**Relevant source:**

- The same moving toggle is repositioned at `MainWindow.cpp:2428–2431`. Its blue focus border is drawn at `2727–2732`; the owner-draw path clears its **current** rectangle at `2694–2699` before drawing.
- `UiStyle.h:237–275` batches geometry changes with `DeferWindowPos` and invalidates changed children afterward. Its flags do **not** include `SWP_NOREDRAW`; do not attribute the defect to such a flag. It also does not explicitly retain and synchronously repaint the union of each moved child's old/new parent bounds.
- `MainWindow.cpp:1964–1966` performs layout and then asynchronously invalidates the parent. Parent `WM_ERASEBKGND` returns 1 (`1986–1987`); real background restoration is in buffered `WM_PAINT` (`1988–1995`) and `PaintWindow` (`2515–2532`). The top-level window uses `WS_CLIPCHILDREN` (`1049`). Parent buffering does not make separately painted child windows and their geometry updates one atomic visible frame.

**Assessment:** High confidence in the visible residual-image finding; medium confidence in a move/background-repaint timing explanation. Source alone does not prove exactly which paint or copy operation leaves the old pixels in these frames. The normal native move can itself invalidate vacated areas, so the absence of explicit old-rectangle handling is not by itself proof of a bug.

**Candidate remedy / regression:** A stable toggle anchor may eliminate this particular moving-outline artifact along with the input race. If movement remains, inspect update regions and painting order, then explicitly restore the old/new affected parent region and repaint the moved child coherently within each animation tick, using a bounded `RedrawWindow`/paint transaction compatible with Win7. Do not repeatedly repaint the entire application blindly or suppress terminal output. Record the actual desktop during collapse, expansion, and reversal under continuous stdout in both themes, checking that each frame has only one toggle outline and no old chevron pixels. Also check terminal redraws and terminal/PTY dimensions: `UpdateTerminalDimensions` already returns during animation at `4372–4374`, with final layout at animation completion. Preserve that intended once-at-end behavior.

## 3. Connection-list Home → End focus loss

**Source chain (high confidence):**

1. The connection list is a native listbox with `LBS_NOTIFY` (`MainWindow.cpp:2306`). Its overlay subclass passes ordinary keyboard messages to the native control (`1681–1686`). Clicking the scrollbar lane can explicitly put focus on it without selecting a session (`1632–1651`).
2. A keyboard Home that changes selection generates `LBN_SELCHANGE`. `MainWindow.cpp:2085–2090` handles that notification by calling `SwitchSession`, with no keyboard/mouse distinction.
3. `SwitchSession` updates state and selection, then **unconditionally calls `SetFocus(terminal_)` at `3696`**. The next actual End key is therefore routed to the terminal rather than the list, unless some other action restores focus.
4. Terminal key routing explicitly claims Home/End (`1785–1796`, `1201–1207`). In CMD mode, End moves the local editor cursor (`3770–3784`). In connected non-CMD mode it can send the terminal End escape sequence (`3818–3819`, `3840`). Thus an apparently ignored list-navigation key can also become terminal input.

This is a source-backed explanation for interrupted consecutive list keyboard navigation. A mouse row-click focusing the terminal may be intentional and should be reported separately from keyboard selection. The power pseudo-row has its own posted `IdPower` path (`2088–2089`), so it needs its own focus case.

**Candidate remedy:** Preserve list focus when switching a session because of keyboard list navigation. Keep focus transfer to the terminal for explicit connect/activate paths where that is intended. Use an explicit activation/focus policy in the selection handler / `SwitchSession` call rather than removing focus transfer globally; mouse selection and keyboard activation can have distinct documented behavior. If single-click selection should focus the terminal, track input origin in the list subclass and pass that policy through. Keep Enter or an explicit activation gesture available to enter the terminal.

**Meaningful regression:** Create more sessions than fit in the list; establish real list focus through a user action such as the scrollbar lane and confirm it with `GetGUIThreadInfo`. Send actual `SendInput` Home then End **without intervening focus repair**, wait on selection outcomes, and record focus plus selection/top-index after each. Require Home to select the first row and End to select the last while maintaining list keyboard-navigation focus. Follow with Up/Down and PageUp/PageDown, including session and power pseudo-rows. Use a local controlled transport or input recorder to ensure these navigation keys did not leak terminal bytes; for CMD, preserve and inspect the draft/cursor. Run both themes. Recover focus only after preserving the uninterrupted-sequence result.

The current `tests/integration/gui_user_lifecycle.py:230–245` is a meaningful regression foundation: actual Home/End input preserves post-Home focus, records the failure, and performs explicit recovery separately (`248–252`). Conversely, `gui_disconnect_smoke.py:98–99` sends `WM_KEYDOWN` directly to the list HWND, bypassing real focus routing. Its pass cannot validate the consecutive real-user keyboard behavior.

## Conclusion and limits

The review identifies two source-backed interaction risks: a toggle that moves substantially while still accepting input, and unconditional terminal focus after list selection. It also confirms visible duplicate/residual toggle pixels in the supplied animation images. The failed attempt-5 gesture should remain failed, with moving-target timing as a strong hypothesis rather than a proven root cause. No native product fix or new Windows execution was performed by this review.

## Addendum: attempt 7 confirms target drift and consecutive-key failure

This addendum independently reads the complete `attempt-7` JSON evidence from GitHub Actions run **38514898081**, job **115691384612**. It supersedes the earlier light-theme-only evidence limit for the sidebar gesture: the new run exercised both themes in desktop/CMD and SFTP views, still on Windows Server 2022 build 20348 at 96 DPI. It does not establish other DPI values or Win7 behavior. No repository files were edited for this addendum.

### Sidebar: observed movement beneath the pointer in all four cases

Each `sidebar-rapid-reversal` operation succeeds at click index 0 and fails at index 1. The updated harness records button bounds and `WindowFromPoint` immediately before injected down and again before injected up. In all four index-1 observations, the pre-down hit is the toggle and the pre-up hit is the **main window**, while the chosen pointer coordinate stays fixed in the input trace. The caption remains `展开侧栏` after the attempted reversal.

| Evidence JSON under `attempt-7/` | Fixed pointer | Pre-down toggle bounds | Pre-up toggle bounds | Horizontal movement | Pre-down hit → pre-up hit |
| --- | --- | --- | --- | --- | --- |
| `x64-light-desktop-user-actions.json` | `[1120,187]` | `[1098,173,1126,201]` | `[1129,173,1157,201]` | +31 px | toggle 66338 → main 131614 |
| `x64-dark-desktop-user-actions.json` | `[1109,187]` | `[1087,173,1115,201]` | `[1127,173,1155,201]` | +40 px | toggle 66338 → main 131614 |
| `x64-light-sftp-user-actions.json` | `[1408,287]` | `[1386,273,1414,301]` | `[1456,273,1484,301]` | +70 px | toggle 197050 → main 459256 |
| `x64-dark-sftp-user-actions.json` | `[1408,287]` | `[1386,273,1414,301]` | `[1438,273,1466,301]` | +52 px | toggle 197050 → main 459256 |

These samples **confirm target drift out from under the pointer across the press interval**. Each displacement exceeds the 28 px button width, and the fixed pointer lies outside the pre-up rectangle. The logged down/up injection intervals are approximately 8.4 ms; the geometry probes bracket those calls, so this is not a measurement of the exact native message-handling interval. Consecutive click coordinates are separated by 41, 52, 83, and 83 px respectively, versus the recorded 4 px system double-click width.

The evidence is now stronger than the attempt-5 inference: moving geometry and the changed geometric hit target are directly observed in every tested theme/view combination. The corresponding implementation is still `MainWindow.cpp:2428–2431`, which centers the toggle within the animated sidebar width, with the timer at `1959–1966` moving that width. The visible action fails to reverse each time. A stationary toggle remains the leading remedy described above.

**Remaining precision limit:** `WindowFromPoint` reports the window geometrically under the pointer. It does not identify the recipient of a captured mouse-up or prove which native Button messages were dispatched. There is still no UI-thread trace of `WM_LBUTTONDOWN`, `WM_LBUTTONUP`, `WM_LBUTTONDBLCLK`, capture state, or `BN_CLICKED`. Therefore the specific native cancellation/notification sequence remains unproven; do not state that the up message was delivered to the main window solely because `hit_before_up` equals the main HWND. The correct confirmed statement is that the toggle moved away beneath the fixed pointer during the attempted click and the target caption did not change. The prior residual-outline evidence remains a separate confirmed visual finding associated with sidebar animation and issue #15.

### Connection list: real Home → End failure is confirmed

`attempt-7/gui-lifecycle.json` records actual `SendInput` keyboard events and `GetGUIThreadInfo` focus observations, with no intervening focus repair in the failing sequence:

| Action | Actual focus before key | Observed selection afterward | Observed focus afterward |
| --- | --- | --- | --- |
| Home | connection list: control 106, HWND 131552 | 0 | terminal: control 107, HWND 131554 |
| End directly after Home | terminal: control 107, HWND 131554 | **0; expected 35** | terminal: control 107, HWND 131554 |
| Explicit user click on overlay thumb, then End | connection list: control 106, HWND 131552 | 35 | connection list: control 106, HWND 131552 |

The file retains one `behavior_failures` entry for `consecutive Home/End connection-list navigation`, `status: FAIL`, and `exit_code: 2`. This is now a **confirmed actual consecutive-key failure**, not just a source-risk prediction. The source chain `LBN_SELCHANGE` → `SwitchSession` → unconditional `SetFocus(terminal_)` at `MainWindow.cpp:3696` matches the observed focus transfer. The recovery selecting row 35 demonstrates that End works when the list has focus; it does not erase the uninterrupted-sequence failure. This evidence does not show remote bytes being sent, so terminal-input leakage remains a source-backed risk rather than a demonstrated transmission in this CMD run.

This is a distinct newly confirmed keyboard-navigation issue adjacent to the lifecycle work tracked in #9. The same JSON separately confirms **35 real CMD sessions**, `known_children_all_exited: true`, `children_after: {}`, and all-disconnect cleanup in **0.406 seconds**. Those successful lifecycle checks should remain reported as successful while the keyboard failure is tracked separately. The row-click focus observation remains separately classified as potentially intentional.

The run continued after each retained failure and ultimately failed at the aggregate `smoke-ui.ps1:664` check with five entries: four sidebar gesture failures plus the connection-list keyboard failure. Successful completion of later cases is not a pass for either defect.
