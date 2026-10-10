# SerialCtl UI Design Guide

SerialCtl uses an Apple-inspired desktop design language adapted to native Win32 and Windows 7. It should feel calm, precise, compact, and consistent without copying macOS window controls.

## Foundations

- Base spacing grid: 8 px.
- Allowed spacing: 8, 12, 16, and 24 px.
- Main cards use 12 px outer gaps, 16 px inner padding, and 8 px gaps between peer actions.
- Component-internal optical insets may use 2 or 4 px only for segmented controls and vector icon strokes; page and dialog layout still follows the base grid.
- All buttons and form controls use a 10 px radius.
- Cards and large surfaces use a 12 px radius.
- Standard button height: 36 px.
- Compact dialog fields and icon buttons use a shared 28 px height; dropdowns and text fields must remain visually equal at the active DPI.
- Compact utility button height: 32 px.
- Terminal fills the center card; there is no bottom composer. Paste and per-session CR/LF/CRLF settings belong in its context menu. SSH Enter stays CR.
- Avoid one-off measurements. Add a shared metric before adding a new value.

## Color tokens

Dark theme:

- Window: `#1C1C1E`
- Surface: `#242426`
- Raised surface: `#2C2C2E`
- Terminal: `#141416`
- Border: `#3A3A3C`
- Text: `#F5F5F7`
- Secondary text: `#98989D`
- Accent: `#0A84FF`
- Accent surface: `#1F3750`
- Danger: `#FF453A`

Light theme:

- Window: `#F5F5F7`
- Surface: `#FFFFFF`
- Raised surface: `#F2F2F7`
- Terminal: `#FFFFFF`
- Border: `#D2D2D7`
- Text: `#1D1D1F`
- Secondary text: `#6E6E73`
- Accent: `#007AFF`
- Accent surface: `#E5F1FF`
- Danger: `#FF3B30`

## Typography

- UI text: use one installed Chinese UI face consistently: Microsoft YaHei UI, then Microsoft YaHei (Windows 7), then SimSun. Use Segoe UI only if no Chinese face is installed. Measure label height with the actual selected font; never clip Chinese glyphs to fixed template bounds.
- Terminal text: Consolas.
- Body: 15-16 px.
- Secondary text: 13-14 px.
- Section title: 18 px semibold.
- Use weight and color before increasing font size.

## Components

- Primary button: accent fill, white text.
- Secondary button: raised-surface fill and subtle border.
- Text button: transparent surface and accent text.
- Danger button: secondary surface with danger text; use a filled danger button only for irreversible actions.
- Icon button: square, same radius as other buttons, centered 16-18 px icon.
- Top navigation icons use the approved monochrome vector family with rounded negative-space details. Editable SVG masters produce transparent 4x PNG atlases, embedded as resources and rendered with Win7 GDI+ high-quality scaling. Do not use raw GDI primitive drawing for navigation icons. The power glyph is a plain circular switch symbol. Desktop icon artwork is unchanged. Other utility icons may retain their shared line family. Do not use Unicode glyphs for theme, arrows, plus, refresh, or other interface symbols.
- Dialog actions placed beside a field use a compact text label, the same height as the field, and at least 32 dialog units of width. Do not use a lone oversized glyph when the action can be named clearly.
- Field-adjacent actions must copy the reference field's final runtime pixel bounds after Windows applies its font and DPI metrics; equal resource values alone are not sufficient.
- Disabled controls retain their shape but use muted content and a low-contrast border.
- Hover uses a slightly brighter surface; pressed uses a darker accent or accent surface; keyboard focus uses an accent border.
- Form labels sit above their controls. Related fields may share a balanced two-column row.
- Dropdowns and text fields must share height, radius, padding, border, focus treatment, and background.
- Native text fields use one shared rounded frame behind an inset borderless edit surface. The rectangular edit surface must remain inside the straight center of the rounded frame, so no dark square client corners can escape the field radius.
- Compact dropdowns stay collapsed in the form, but their opened list must expose every fixed built-in option without scrolling; dynamic serial-port lists may scroll when necessary.
- Application-owned command, SFTP, and combo-box lists use the shared 5 px overlay scrollbar. Hide it when all content fits; never show the classic Windows arrow-and-track scrollbar inside the custom interface.
- Overlay scrollbars reserve a 16 px interaction lane at the right edge. Cards and rows stop before this lane, so the scrollbar never covers content or copy actions.
- Overlay scrollbars have no permanent track. Their rounded thumb uses the standard border token so it recedes into the surrounding surface instead of becoming a bright visual divider. The entire lane alongside the thumb may start a drag, and mouse wheel, keyboard, track paging, and direct thumb dragging must work in both palettes.
- When scrolling is required, every scrollbar redraw first restores the complete interaction lane with the owning list surface color, then draws the current thumb. When all content fits, the lane is not painted at all, so full-width card backgrounds and rounded corners remain intact. Content-count changes request a background erase to remove any former thumb.
- Opened combo-box lists use the same rounded region and border tokens as their collapsed fields. System-owned file dialogs keep native Windows controls.
- `RoundRect` receives an ellipse diameter, so drawing code must apply the radius token as `radius * 2`; control regions use the same rule.
- Rounded control regions must be calculated from the full window bounds, not the inset edit client area, otherwise the lower border is clipped.

## Main window

- Connection methods form one evenly spaced segmented toolbar.
- The connection-method track uses the standard 12 px horizontal inset and 16 px vertical inset. Keep the 72 px toolbar height so the track does not visually merge with the title bar or the content divider.
- Content segmented controls use one raised-surface outer track and an inset selected pill; the main toolbar uses only individual selection/hover pills. Adjacent segments do not create separate competing borders.
- The top navigation uses six items (SSH, serial, Telnet, remote serial, CMD, power), 24 px icon bounds, consistent 40 px text inset and one shared track; no API item. Default theme is light. Selected navigation uses the existing raised-surface pill and normal foreground; it does not use the content accent color.
- The toolbar merges into the existing surface without an enclosing border or separate track. Only selected and hover items have a raised-surface pill.
- The title-bar application icon follows the active theme: dark artwork in dark mode and the matching light artwork in light mode.
- Left connection list, center terminal, and right command collection are separate rounded surfaces; avoid strong full-height divider lines.
- The side columns scale within shared minimum and maximum widths. The terminal receives remaining space; maximizing the window must not leave both side panels visually undersized.
- Do not add a separate title card above the terminal. The left connection list owns session identity and disconnect actions; the center column is reserved for terminal content.
- Shared serial sessions are directly writable by every connected client; the connection heading keeps only the disconnect action.
- Terminal utility settings belong in the terminal right-click menu: local echo, millisecond timestamps, save log, clear, copy, and select all.
- Terminal output starts empty. Connection guidance and status text belong in the status UI and must not be injected into terminal output.
- Terminal timestamps occupy a separate gutter and never become part of terminal cells, selection, command history, or data sent back to the remote session.
- The terminal uses a cell-based VT renderer with a 20,000-line primary-screen scrollback buffer. Mouse wheel, overlay-thumb dragging, Shift+PageUp/PageDown, Shift+Home, and Shift+End navigate history without moving the remote cursor.
- `Ctrl+mouse wheel` changes only the terminal font size, within the shared terminal typography range. A size change immediately recalculates the cell grid and updates the remote PTY.
- Preserved lines wider than the current grid use a bottom overlay scrollbar following the same 16 px interaction-lane and 5 px thumb tokens as the vertical scrollbar. Horizontal wheel and `Shift+mouse wheel` move the same viewport; no permanent track or new color is introduced.
- New output must not force the view to the bottom while the user is reviewing history. Show the compact accent `新输出` action inside the terminal until the user returns to the live view.
- Alternate-screen applications such as `vi`, `vim`, `top`, and `less` use the full terminal surface without the timestamp gutter or primary scrollback. Entering or leaving the alternate screen recalculates and reports the available PTY size; leaving it restores the primary screen and its scroll position.
- Terminal selection is cell-based: drag selects a range, double-click selects a word, `Ctrl+Shift+C` copies, and `Ctrl+Shift+V` pastes. Bracketed paste is honored when requested by the remote application.
- Command cards use the same card radius and vertical rhythm as connection cards.
- Every command card has matching 28 px Run and Edit icon actions separated by 8 px. Run is disabled when the session is not writable; double-click remains a shortcut for Run.
- Multi-command macros keep the standard command-card size. While a macro runs, its Run action becomes Stop and draws a determinate progress ring inside the same 28 px bounds; other structural command actions remain disabled until the macro completes or stops.
- Idle status labels do not reserve permanent rows. Transient status temporarily reserves space at the bottom of the left card. Error status stays until replaced; long status is available in a hover tooltip.
- The right panel uses the same segmented tab style for Commands and SFTP. The SFTP working directory uses the shared rounded field treatment and follows the active SSH shell directory when it can be identified from the prompt. SFTP transfer actions use compact secondary buttons and remain disabled while an operation is running.
- The right panel has a borderless 12 px splitter interaction lane, supports a 260-650 px remembered width, and may collapse to a visible 48 px rail containing one centered 28 px icon action. The 12 px outer margin is additional to the rail width. Resizing must preserve at least 400 px for the center terminal at the minimum supported window size.
- The SFTP browser uses a clickable breadcrumb field followed by a compact 32 px table header. File rows use the standard 36 px control rhythm, keep folders before files for every sort order, and expose Name, Size, and Modified sorting without native ListView chrome.
- SFTP supports extended multi-selection. Destructive remote actions remain in the SFTP context menu, display the exact affected remote paths, and require confirmation. Local-file drag-in queues uploads; remote drag-out is not implied by this rule.
- The SFTP transfer drawer uses a 32 px text-button header and 48 px transfer rows. It is collapsible, never overlays the file list, and shows queued, running, completed, failed, and canceled states. Determinate progress uses the accent token; unknown progress remains visually indeterminate without inventing a percentage.
- SFTP table, transfer, and context-menu actions use existing secondary, text, icon, and danger roles. Long names remain single-line with ellipsis, while the complete remote path is available from the context menu and copy action.

## Dialogs

- Dialog width remains compact and consistent.
- Connection and command dialogs share a 238-dialog-unit compact width, 12-dialog-unit outer margins, balanced columns, and the same footer geometry.
- Dialog height follows visible content; hidden fields must not leave empty rows.
- Use labels above fields and a two-column grid only for closely related values.
- Footer buttons are right-aligned, equal in size, and separated by 8 px.
- Dialog footer buttons use the 36 px standard height; they must not inherit the taller native resource-template size.
- Buttons sharing a row use the same height and visual role. Keep 8 px between peer actions; field-adjacent actions use the dialog grid gap and align exactly with the field top and bottom.
- Primary action appears at the far right.
- Helper text sits directly above the footer and uses the secondary-text token.
- Do not show generic helper copy in connection dialogs. Reserve the inline message row for validation errors only.
- Connection dialogs and command dialogs must use the same control styling and footer geometry.
- The command dialog retains its 238-DLU width and single-column arrangement at every step count, starting with one full-width instruction field. Added instruction rows use the compact grid with a same-height named Delete action; the shared interval field appears only when more than one instruction exists.

## Review checklist

- No new arbitrary color, radius, height, or spacing value.
- Dark and light themes both checked.
- Main window checked at minimum supported size.
- All changed dialogs checked with every field configuration.
- Hover, pressed, focus, checked, and disabled states checked.
- x64 builds pass Windows 7 compatibility checks.

## Current interaction policy

Command drag, add, edit, delete and import update an unsaved draft. A compact secondary Save button uses the same footer grid as Import/Export/Delete. Closing a dirty draft offers Save/Discard/Cancel. SFTP column boundaries use the resize cursor and persisted widths. Path double-click / Ctrl+L and the context menu open the shared input dialog. Manual browsing pauses terminal follow. IP discovery runs asynchronously after a 500 ms debounce; only already-open COMs are listed.

Power and local CMD share the center card. Power uses three checked channel cards (three columns at 600 px or wider; stacked below this width), one ON/OFF action pair, separate connect/disconnect, concise status and task fields. The command/SFTP panel collapses automatically while power is selected, without changing the saved terminal-panel preference. No decorative helper paragraphs. System DPI scales fonts and metrics, narrow work areas may collapse the side panel. Power content scrolls when needed.

## Power management

- Follow the approved V1.0.5 review geometry, preserved in docs/testing/power-layout.json: 716 px center content at the 1044×760 window reference. Preserve title bar, left card, and 48 px right rail.
- Header buttons: 36 px high, 96 px wide. Mode/actions row starts at 52 px, cards at 104 px, each card 216 px high with 12 px gaps and 16 px inner padding. No per-channel Apply buttons.
- Parallel and series combine CH1＋CH2 into one double-width card; CH3 remains independent. One set of Apply, On and Off actions acts on checked cards. Tracking is fixed 1:1 and retains physical-channel measurement cards.
- Main tabs start at 336 px; details at 384 px. At small widths use stacked cards and the existing overlay scroll lane. All physical readouts are unknown until actual device responses arrive.
- Low frequency actions are grouped under Monitor, Automation, Device Settings, and Logs/Diagnostics. Avoid persistent explanatory text and empty helper rows.
- Sidebar collapse/expand lasts 200 ms with cubic easing. Input and output continue; update terminal cells/PTY once at the end. Hide sidebar actions during animation so their minimum layout cannot overlap the center. Direct splitter dragging has no animation.

- Trend grids use the existing border token at 50% opacity over the surface. Native WM_PRINT rendering must match normal field/combobox painting; disabled edit clients retain the same field fill and muted text.
