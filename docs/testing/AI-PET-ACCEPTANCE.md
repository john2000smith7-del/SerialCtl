# AI companion acceptance — V2.0.8

The companion is a local Win32 child with embedded artwork. It has no device or model API.

1. Open AI. After approximately 4 seconds of usable page time, exactly three forward/backward/forward hops play before the landing pause and single-hand boxing. The 8-second clip returns to the latest actual AI state. Repeated resize/focus does not rearm it.
2. Leave AI before the delay: no later playback. Minimize or open a modal: presentation pauses, a pending opener waits another 4 seconds after resuming. Manual Wave/Boxing/Stop cancels pending opening.
3. Follow accepted AI event states: thinking, native/device tools, reply, success, failure, stop and disconnect. Stale task notifications must not change the state. Failure/stop are never celebrated.
4. Check the real main window, action menu and settings in light/dark themes and 96/120/144/192 DPI. No connection/footer overlap; short windows collapse the dock and retain the header paw. Chat/terminal default ratio remains equal.
5. Toggle visibility/follow/opening, choose 96/120/144, drag within the reserved lane, restore position, save/reopen. Cancel must preserve all preferences. No prompt or device operation is sent by pet actions.

Automated model and native component tests run through CTest. Native screenshots are retained as CI evidence, outside runtime packages. Formal MSVC/Win7-import/package checks and authentic signed old-client upgrade must pass before release. Physical Windows 7, board/power and on-site AI service compatibility are separate checks, not implied by these fixtures.

## V2.0.9 desktop mode

Verify that the default pet is an unowned layered/no-activation tool window, transparent pixels have zero alpha, desktop dragging updates global coordinates, and restored positions fit the current monitor. Minimize the main window, switch pages and continue to receive AI activity: the pet stays visible and does not take keyboard focus. Right-click restores the main window or returns to the dock; the dock reclaims its original reserved lane. Close the application: no pet window/process remains. Check both themes, all three sizes, four DPI scales, desktop settings, topmost off/on and a disconnected monitor. Single-instance integration verifies two launches, a second installation directory and crash-style lock release; internal bridge/updater startup remains independently exercised.
