# SerialCtl UI Design Guide

SerialCtl uses an Apple-inspired desktop design language adapted to native Win32 and Windows 7. It should feel calm, precise, compact, and consistent without copying macOS window controls.

## Foundations

- Base spacing grid: 8 px.
- Allowed spacing: 8, 12, 16, and 24 px.
- Main cards use 12 px outer gaps, 16 px inner padding, and 12 px gaps between peer actions.
- When the fixed navigation would enter the detached theme action's 12 px gap, use 48 px icon-only navigation items with the same approved 24 px icons, accessible names and full-label tooltips. Restore labels when space returns; do not wrap or enlarge the toolbar.
- The power mode field and its action group share a width budget. Below 564 logical px, move the action group to a separate row and shift the channel cards down by 48 px; peer actions keep 12 px gaps.
- AI headers reserve nonoverlapping areas for status and actions. At narrow widths retain a short AI title and readable status; the monitor wrap switch becomes a 32 px icon button with a tooltip before its title area would shrink below 80 px.
- AI empty-state titles wrap to the measured content width without changing font size; place the description below the measured title with at least 8 px clearance, including at the narrowest divider position.
- Oversized application dialogs (including dynamically growing command/step editors) fit inside the monitor work area with 12 px margins. Keep the footer actions/credits fixed, scroll only the content using the shared overlay scrollbar, clip content before the footer, and reveal fields on keyboard focus. Normal-size dialog geometry stays unchanged.
- Component-internal optical insets may use 2 or 4 px only for segmented controls and vector icon strokes; page and dialog layout still follows the base grid.
- All buttons and form controls use a 10 px radius.
- Cards and large surfaces use a 12 px radius.
- Standard button height: 36 px.
- The unavailable power-connection notice uses a compact 360 by 132 logical-pixel client area, 16 px content insets and a single 96 by 36 px primary footer action. Reuse the existing owned-dialog, theme and rounded-button renderers; do not enumerate or open devices when showing this notice.
- Compact dialog fields and icon buttons use a shared 28 px height; dropdowns and text fields must remain visually equal at the active DPI.
- Compact utility button height: 32 px.
- Terminal fills the center card; there is no bottom composer. Mouse right-click pastes once; per-session CR/LF/CRLF settings belong in the top Settings entry. SSH Enter stays CR.
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
- Accent: `#1F4FD8`
- Accent text / focus ink: `#6595FF` on dark neutral surfaces. Keep `#1F4FD8` for filled controls with white content.
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
- Accent: `#1F4FD8`
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
- Unfilled accent text, links, sort chevrons and focused field outlines use the shared theme-aware accent ink; disabled text actions use secondary text, including Add at the ten-step limit.
- Danger button: secondary surface with danger text; use a filled danger button only for irreversible actions.
- Icon button: square, same radius as other buttons, centered 16-18 px icon.
- Top navigation icons use the approved monochrome vector family with rounded negative-space details. Editable SVG masters produce transparent 4x PNG atlases, embedded as resources and rendered with Win7 GDI+ high-quality scaling. Do not use raw GDI primitive drawing for navigation icons. The power glyph is a plain circular switch symbol. Desktop icon artwork is unchanged. Other utility icons may retain their shared line family. Do not use Unicode glyphs for theme, arrows, plus, refresh, or other interface symbols.
- Dialog actions placed beside a field use a compact text label, the same height as the field, and at least 32 dialog units of width. Do not use a lone oversized glyph when the action can be named clearly.
- Field-adjacent actions must copy the reference field's final runtime pixel bounds after Windows applies its font and DPI metrics; equal resource values alone are not sufficient.
- Disabled controls retain their shape but use muted content and a low-contrast border.
- Checkboxes keep native automatic state, keyboard operation and accessibility while sharing the themed painter. Use a 16 px indicator, 2 px indicator radius and 8 px label gap, scaled with DPI. Disabled labels use one muted text pass without the native embossed shadow; paint the complete parent surface, and retain a visible keyboard-focus cue using the shared rounded outline.
- Owner-drawn buttons paint their entire background and content together; do not expose the native system button erase color before the themed draw. Theme changes invalidate every direct child, including footer actions. Compose each owner-drawn item (including list rows and power tabs) off screen before one final clipped blit, so resize cannot present a cleared card before its text and icons. Preserve native item coordinates and DC state; geometry changes must not copy stale rounded-edge pixels.
- Hover uses a neutral surface. Secondary held presses use solid accent and white content; primary held presses use accentInset. Keyboard focus for buttons uses the shared antialiased inset rounded outline; text fields retain their accent focus frame.
- Form labels sit above their controls. Related fields may share a balanced two-column row.
- Dropdowns and text fields must share height, radius, padding, border, focus treatment, and background.
- Native text fields use one shared rounded frame behind an inset borderless edit surface. The rectangular edit surface must remain inside the straight center of the rounded frame, so no dark square client corners can escape the field radius.
- Compact dropdowns stay collapsed in the form, but their opened list must expose every fixed built-in option without scrolling; dynamic serial-port lists may scroll when necessary.
- Application-owned connection, command, SFTP, and combo-box lists use the shared 5 px overlay scrollbar. Hide it when all content fits; never show the classic Windows arrow-and-track scrollbar inside the custom interface.
- Overlay scrollbars reserve a 16 px interaction lane at the right edge. Cards and rows stop before this lane, so the scrollbar never covers content or copy actions.
- Overlay scrollbars have no permanent track. Their rounded thumb uses the standard border token so it recedes into the surrounding surface instead of becoming a bright visual divider. The entire lane alongside the thumb may start a drag, and mouse wheel, keyboard, track paging, and direct thumb dragging must work in both palettes.
- When scrolling is required, every scrollbar redraw first restores the complete interaction lane with the owning list surface color, then draws the current thumb. When all content fits, the lane is not painted at all, so full-width card backgrounds and rounded corners remain intact. Content-count changes request a background erase to remove any former thumb.
- Opened combo-box lists use the same rounded outline, theme, row padding and overlay scrollbar as their collapsed fields. Both main-window and power controls use the shared implementation. System-owned file dialogs keep native Windows controls.
- Collapsed dropdowns use one buffered field renderer for normal paint, print and native selection-field owner drawing. Native keyboard, mouse and selection notifications may draw directly outside WM_PAINT; restore the complete field synchronously after those transitions, including while focus remains in the dropdown. Never leave the collapsed owner-draw callback empty or rely on focus loss/global window refresh to repair its rectangular native background. This applies to AI, Settings, connection dialogs and power pages.
- All application rounded surfaces use the shared antialiased renderer. Align strokes to physical pixels, scale radii and internal insets with DPI, paint the surrounding surface, and keep window-region clipping outside the antialiased border.
- `RoundRect` receives an ellipse diameter, so drawing code must apply the radius token as `radius * 2`; control regions use the same rule.
- Rounded control regions must be calculated from the full window bounds, not the inset edit client area, otherwise the lower border is clipped.

## Main window

- Connection methods form one evenly spaced segmented toolbar.
- The top toolbar has one continuous rounded container enclosing navigation and Settings only, ending 8 px after Settings rather than stretching across the window. The theme action is a separate 36 px icon button at the right 12 px window inset; restore its exterior corners with the window background, and give it its own focus border. The navigation frame starts at y=16 px and ends at y=56 px; its buttons are 36 px tall with a 2 px optical vertical inset. Keep 12 px between the visible toolbar bottom and the three main cards, matching both horizontal card gaps. Scale these shared metrics at the active DPI.
- Top navigation items have no individual outline. All top items use a neutral resting surface, including the currently open AI and power pages. Only an enabled held press uses solid accent and white foreground; release, cancellation and capture loss restore neutral styling. The shared container always keeps its neutral border. Keyboard focus uses the shared inset rounded focus outline, never a persistent blue container or item outline.
- Other selection bars use the shared rounded-rectangle outline, a neutral muted border for the current page, neutral hover and a momentary accent fill for a held press; keyboard focus uses the shared inset rounded focus outline. Apply this to Commands/SFTP, power main pages, automation pages and device-setting groups; underline-only tabs are not used. Keep consistent gaps and avoid overlapping outlines.
- The top navigation retains seven connection/page items (SSH, serial, Telnet, remote serial, CMD, power, AI), plus a separate Settings action. Settings is not a connection type. Use 24 px monochrome icons and the shared label inset; no API item. Default theme is light. Idle navigation uses the shared toolbar surface and normal foreground. Current pages retain neutral navigation buttons; page identity is carried by the visible content and connection list. Primary submit actions and AI user messages retain their existing solid blue role.
- Navigation items share the single toolbar boundary, including in idle and selected states; do not draw per-item borders. The toolbar retains the standard surface, radius and height tokens in both themes.
- The title-bar application icon follows the active theme: dark artwork in dark mode and the matching light artwork in light mode.
- Left connection list, center terminal, and right command collection are separate rounded surfaces; avoid strong full-height divider lines.
- The side columns scale within shared minimum and maximum widths. The terminal receives remaining space; maximizing the window must not leave both side panels visually undersized.
- Resize handlers use the actual client bounds. At the end of a native sizing gesture, commit the final control layout before releasing deferred terminal reflow, so right-edge actions cannot remain at an earlier frame's position while a large terminal history is rewrapped.
- Do not add a separate title card above the terminal. The left connection list owns session identity and disconnect actions; the center column is reserved for terminal content.
- Shared serial sessions are directly writable by every connected client; the connection heading keeps only the disconnect action.
- The top Settings entry contains all terminal utility actions: local echo, millisecond timestamps, raw RX/TX tracing, encoding, Enter policy, save log, save network diagnostics, clear, copy, paste and select all. Clearly distinguish whole-window options from current-session options and instance diagnostics. Missing sessions disable only session-dependent actions.
- Session log, power log and measurement CSV exports run as one background task per application instance. Snapshot the chosen source at a fixed accepted-byte cutoff; ongoing recording, session switching and disconnect must remain usable. Keep Save Log under current-session actions; put Cancel Log Export beside network diagnostics under the instance group, using the existing two-column secondary-button grid. That cancel action remains available after its source session closes. The existing small note shows source and copy progress, with a full-text tooltip; closing Settings or cancelling its option draft does not cancel an export. Copying 100% is not a successful save until the temporary file has been flushed and committed. Never replace device or macro unknown-outcome warnings with export progress.
- Terminal output starts empty. Connection guidance and status text belong in the status UI and must not be injected into terminal output.
- Terminal timestamps occupy a separate gutter and never become part of terminal cells, selection, command history, or data sent back to the remote session.
- The terminal uses a cell-based VT renderer. Primary scrollback retains up to 20,000 logical lines or 1.6 million occupied cells at the default 80-column initial grid (larger initial grids scale the cell budget). New output evicts the oldest content when this budget fills; resizing alone never evicts retained text, and visual soft-wrap rows do not count as new logical lines. Mouse wheel, overlay-thumb dragging, Shift+PageUp/PageDown, Shift+Home, and Shift+End navigate history without moving the remote cursor.
- `Ctrl+mouse wheel` changes only the terminal font size, within the shared terminal typography range. A size change immediately recalculates the cell grid and updates the remote PTY.
- Primary-screen logical lines reflow to the current grid; hard line breaks, cell attributes, wide characters, cursor and reading/selection anchors are preserved. The existing bottom overlay scrollbar remains available for preserved VT grid content where needed.
- New output must not force the view to the bottom while the user is reviewing history. Show the compact accent `新输出` action inside the terminal until the user returns to the live view.
- Alternate-screen applications such as `vi`, `vim`, `top`, and `less` use the full terminal surface without the timestamp gutter or primary scrollback. Entering or leaving the alternate screen recalculates and reports the available PTY size; leaving it restores the primary screen and its scroll position.
- Terminal selection is cell-based: completing a nonempty drag or double-clicking a word copies it once, retaining the highlight and history-reading position. An empty click or scrollbar drag does not replace the clipboard. Mouse right-click pastes once and returns to the live position without appending Enter. Clipboard line breaks retain the session's normal semantics; CMD submits complete lines and retains an incomplete trailing draft. `Ctrl+Shift+C`, selection-aware `Ctrl+C`, `Ctrl+Shift+V`, and `Shift+Insert` remain available. The keyboard context-menu key opens Settings and never pastes. Bracketed paste is honored when requested by the remote application. Text fields, password fields and SFTP retain their own editing/file-operation semantics.
- Commands use one native plain-text RichEdit document, with a 48 logical-pixel gutter and no row cards. A 32 px filename row and 32 px action row sit above the editor, with 8 px vertical gaps; a 48 px compact status footer sits below. Reuse shared surface, font, radius and scrollbar tokens. Never soft-wrap commands; preserve ordinary copy, paste, undo and selection.
- One primary Run action sends the current line or the complete physical lines touched by a selection, in source order. A selection ending at column one excludes that untouched next line. Blank lines, comments and legacy macro delimiters are not sent. Only the caret/selected executable rows show 12 px gutter Run icons. A gutter click runs that row alone and submits on release inside the original hit target.
- Body double-click executes the clicked complete line by default; a checked More option can restore ordinary word-selection behavior. Ctrl+Enter follows the Run button. Text drags never execute.
- Run/Save/Import/Export use 16 px embedded SVG glyphs and equal compact bounds; narrow panes use icon-only actions with accessible captions/tooltips. Filename More owns the double-click option and sending interval. Do not add separate single/multi Run buttons.
- While a snapshot is being submitted, Run becomes Stop and retains its bounds; text/import/save are read-only until submission ends. Report accepted N/N and direct users to terminal output for execution results. Stop prevents future submissions; accepted commands may continue. Session changes/disconnects preserve unknown outcomes and never resend automatically.
- The original commands.txt is retained; the editable draft persists atomically to command-notebook.txt. Invalid import/save does not replace active/persistent content. Dragging one UTF-8 file uses the import path and does not execute. Unsaved replacement/close uses Save/Discard/Cancel.
- A quiet two-line multi-run reminder may occupy free space left of the detached theme action; hide it when that free space is narrower than 180 logical px, and never overlap navigation or the theme button.
- Idle status labels do not reserve permanent rows. Transient status temporarily reserves space at the bottom of the left card. Error status stays until replaced; long status is available in a hover tooltip.
- The right panel uses the same segmented tab style for Commands and SFTP. The SFTP working directory uses the shared rounded field treatment and follows the active SSH shell directory when it can be identified from the prompt. SFTP transfer actions use compact secondary buttons and remain disabled while an operation is running.
- The right panel has a borderless 12 px splitter interaction lane, supports a 260-650 px remembered width, and may collapse to a visible 48 px rail containing one centered 28 px icon action. The 12 px outer margin is additional to the rail width. Resizing must preserve at least 400 px for the center terminal at the minimum supported window size.
- Keep the collapse/expand action anchored at the center of that final rightmost rail in every state, including expanded and reversing animation. The hit target must not move during a press; each released click, including the second click of a double-click, may reverse the animation. Adjacent header actions retain their shared 8 px gap.
- Commit sidebar child visibility with final geometry in the same layout transaction. Newly visible actions must not be exposed at their previous bounds while the animation finishes.
- Connection-list keyboard selection keeps focus in the list for consecutive navigation, including the power row. Mouse session selection and explicit connection activation retain their terminal-focus behavior.
- The SFTP browser uses a clickable breadcrumb field followed by a compact 32 px table header. File rows use the standard 36 px control rhythm, keep folders before files for every sort order, and expose Name, Size, and Modified sorting without native ListView chrome.
- SFTP supports extended multi-selection. Destructive remote actions remain in the SFTP context menu, display the exact affected remote paths, and require confirmation. Local-file drag-in queues uploads; remote drag-out is not implied by this rule.
- The SFTP transfer drawer uses a 32 px text-button header and 48 px transfer rows. It is collapsible, never overlays the file list, and shows queued, running, completed, failed, and canceled states. Determinate progress uses the accent token; unknown progress remains visually indeterminate without inventing a percentage.
- SFTP table, transfer, and context-menu actions use existing secondary, text, icon, and danger roles. Long names remain single-line with ellipsis, while the complete remote path is available from the context menu and copy action.

## AI page

- AI replaces the center content using the existing page mechanism; it is not a Commands/SFTP tab. Its right sidebar starts collapsed and can expand to show the selected live terminal, reusing the real terminal renderer and session state. Use the existing 12 px splitter lane, 260–650 px bounds and 400 px center minimum; remember AI width/collapse separately from terminal Commands/SFTP preferences. Keep the rail toggle anchored during animation. The terminal header uses the existing compact height and panel padding; missing targets show an empty monitor with a concise selection label.
- The AI transcript uses the light window token (#F5F5F7) or dark raised token (#2C2C2E); replies and composer fields use the corresponding standard surface token. Reuse these tokens for the AI connection dialog and every rounded field exterior. No new palette colors are added.
- The center card contains an independently scrollable transcript and a fixed compact multiline composer inside its bottom edge. The read-only server default model and explicitly selected terminal target (serial, remote serial, CMD, Telnet or SSH) belong in that composer; label each connection type and do not select a target by default. Power ON/OFF uses the existing power page connection and checked channels; show its exact scope and action/readback status in the transcript without adding a workspace or second confirmation dialog.
- Offer remote-IP discovery through the existing remote-serial connection dialog. The operator selects the returned port, and the AI turn pins that exact session identity.
- User messages align right on the existing accent surface. Each AI reply uses one complete rounded message frame, including source, code/log blocks and copy action; use the shared accent border and card radius.
- History reading keeps its pixel position when a reply streams. Show a compact return-to-latest action, a 5 px overlay thumb with a reserved 16 px lane, and support wheel, keys and dragging. The composer never scrolls with messages.
- The connection dialog includes one optional Linux absolute project-directory field using the existing single-column compact grid. Do not expose model selection, model refresh or a manual model ID. Native server command/file results appear as tool message frames with actual paths, output, diffs and exit status.
- Reuse the compact connection dialog, standard field/button roles and both palettes. Only verified service state is displayed as connected; examples and test fixtures never populate production conversation content.

## Dialogs

- Dialog width remains compact and consistent.
- Connection and command dialogs share a 238-dialog-unit compact width, 12-dialog-unit outer margins, balanced columns, and the same footer geometry.
- Dialog height follows visible content; hidden fields must not leave empty rows.
- Use labels above fields and a two-column grid only for closely related values.
- Footer buttons are right-aligned, equal in size, and separated by 12 px.
- Dialog footer buttons use the 36 px standard height; they must not inherit the taller native resource-template size.
- Buttons sharing a row use the same height and visual role. Keep 12 px between peer actions; field-adjacent actions use the dialog grid gap and align exactly with the field top and bottom.
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

Command notebook text edits and imports update an unsaved draft. A compact secondary Save button uses the same footer grid as Import/Export/Delete. Closing a dirty draft offers Save/Discard/Cancel. SFTP column boundaries use the resize cursor and persisted widths. Path double-click / Ctrl+L and the context menu open the shared input dialog. Manual browsing pauses terminal follow. IP discovery runs asynchronously after a 500 ms debounce; only already-open COMs are listed.

Power and local CMD share the center card. Power uses three checked channel cards (three columns at 600 px or wider; stacked below this width), one ON/OFF action pair, separate connect/disconnect, concise status and task fields. The command/SFTP panel collapses automatically while power is selected, without changing the saved terminal-panel preference. No decorative helper paragraphs. System DPI scales fonts and metrics, narrow work areas may collapse the side panel. Power content scrolls when needed.

## Power management

- Follow the approved V1.0.5 review geometry, preserved in docs/testing/power-layout.json: 716 px center content at the 1044×760 window reference. Preserve title bar, left card, and 48 px right rail.
- Header buttons: 36 px high, 96 px wide. Mode/actions row starts at 52 px, cards at 104 px, each card 216 px high with 12 px gaps and 16 px inner padding. No per-channel Apply buttons.
- Parallel and series combine CH1＋CH2 into one double-width card; CH3 remains independent. One set of Apply, On and Off actions acts on checked cards. Tracking is fixed 1:1 and retains physical-channel measurement cards.
- Main tabs start at 336 px; details at 384 px. At small widths use stacked cards and the existing overlay scroll lane. All physical readouts are unknown until actual device responses arrive.
- Low frequency actions are grouped under Monitor, Automation, Device Settings, and Logs/Diagnostics. Avoid persistent explanatory text and empty helper rows.
- Sidebar collapse/expand lasts 200 ms with cubic easing. Input and output continue; update terminal cells/PTY once at the end. Hide sidebar actions during animation so their minimum layout cannot overlap the center. Direct splitter dragging has no animation.

- Trend grids use the existing border token at 50% opacity over the surface. Native WM_PRINT rendering must match normal field/combobox painting; disabled edit clients retain the same field fill and muted text.

The left connection card keeps the existing per-session Disconnect action and one compact danger-text “全部断开” footer action. It stops real transports, CMD process trees, SFTP transfers and power-task ownership before removing entries. No remote disconnect permission is implied. CMD drafts render within the terminal; no composer is added.

AI chat bubbles use alignment and accent fill to identify the speaker; omit role/status headings and the Enter/Shift+Enter footer hint. The composer and all its rounded fields restore exterior corners using the parent surface in both palettes. Owner-drawn theme buttons restore their full current-palette rendering after native focus/press transitions; a theme toggle synchronously invalidates the complete visible child tree.


## V2.0.3 approved revision

- Peer rounded cards and peer actions use a shared 12 logical px exterior gap; container insets remain 16 px. Optical glyph insets remain separate. Buttons radius10, cards radius12, standard/compact heights36/28.
- AI workspace starts expanded with equal chat/monitor widths, excludes the connection list, and permits a draggable divider. Persist user ratios under a new setting; migrate the old fixed-width/collapsed default once.
- AI header is compact, without a name or role subtitle. After verified connection readiness, the initial welcome message introduces the engineering assistant's capabilities without disclosing its internal name, jiojio, or initiating device work. History/reconnect do not stack introductions.
- One continuous AI background spans header, transcript and composer. Separate these regions only with thin horizontal lines whose ends stop at the panel padding. The terminal uses the same open-ended separator over its own uniform background.
- AI supports character-range drag selection, copy on release and Ctrl+C; keep whole-message copying. Streaming and scrolling preserve the selected range and reading location. Native edit menus retain their usual semantics.
- Connection cards directly reuse the corresponding ToolbarIcons resource. Sidebar glyphs are20 logical px, never larger than top-nav24. No substitute monitor/DB9/hand-drawn glyphs. Use text plus status dot, with selected and focus states.
- Command cards use a centered 16 px >_ resource glyph in their existing drag lane, name and command preview, and existing run/edit actions. Consistent12px card gaps.
- The AI live monitor hides application timestamp gutters. Its default long-line view joins only actual soft wraps, never changes sent bytes or remote PTY width, and retains explicit CR/LF and alternate-screen semantics. Offer wrap toggle and horizontal scrolling.
- Settings footer shows VERSION and `Application design by jiojio`; utility actions use compact consistent line icons.
- Startup verifies signed newer metadata before showing a mandatory confirmation. Only after OK may ZIP download begin. Closing exits rather than bypassing a known required update. Progress reflects real download/verification/install phases; retries preserve existing data and idle-install protections. Unavailable/untrusted metadata does not invent a newer version. Existing two-file deployment and update.ini URL remain.

## Approved compact resource design (V2.0.3)

- Connection cards are 48 logical px high in 60 px rows, icons 18 px; command cards remain 54 in 66 px rows with a centered 16 px command resource glyph. Use shared 12 px gaps and a compact 190 px left lane.
- AI composer is 96 px: multiline text above a 28 px footer. The target shows a short name; full hardware/source details belong in the themed dropdown and tooltip. Server-default model information belongs in connection settings, not a persistent composer label.
- Selected connection cards, dropdown rows and user messages use solid #1F4FD8 with white foreground; navigation and command rows use neutral resting states; hover without selection uses a neutral raised surface. Final task output stays visible. Completed turns automatically collapse the process, including failed diagnostic steps, whose warning remains in the summary. Failed or stopped turns remain expanded.
- Utility SVG masters and the surface fill/stroke masks generate embedded 4x PNG resources. Surface masks are nine-sliced with fixed DPI-scaled corners; dynamic text, native editing/accessibility and data plots stay live. No runtime SVG framework or source files are required.
- Whole controls compose against their parent surface. Restore the owner surface before painting rounded resources, including focus, dropdown selection, theme switch and scrollbar updates.
- Remote device discovery remains a themed dropdown; it is not a card gallery. Only the application’s own SerialCtl WebSocket service is discovered; there is no transport selector, UDP candidate or UDP fallback. Remove the unused mode row and close the vertical gap using the existing compact grid.

历史会话弹窗使用共享436宽紧凑网格、搜索28高、会话行48高与同色细滚动条；不使用系统原始 TrackPopupMenu 展示会话。远程设备列表仍为统一主题下拉框。压缩与折叠分别处理：折叠保留原文，回合正常完成后自动收起（中间命令失败在摘要标记），回合失败／停止／未知保持展开，最终回答不并入过程；缺少 phase 的中间 AI 消息也归入过程，恢复历史规则相同。

资源底板先完成九宫格合成，再用单次原生 AlphaBlend 提交，遵守局部可见区域与 DC 原点；不得将九片分别缩放直接提交到目标 DC 造成接缝。缓存最多96个底板且通常不超过16MiB，圆角透明处保留父背景。强调色纯色区必须准确等于主题色。

深蓝选中态上的代码块和图标悬停底板共用 accentInset `#193FAF`；白字保持至少4.5:1对比度。不得在用户深蓝消息中继续铺浅灰代码底，也不得让选中命令的白色动作图标落在浅蓝悬停底上。

## V2.0.3 r1 feedback and form corrections

- Shared secondary buttons use #1F4FD8 and white content only during a held enabled press. Hover stays neutral; release, leave/cancel and focus changes cannot latch an action color. Primary submit actions retain their primary idle fill. Native keyboard focus remains visible as an inset cue.
- A selected command keeps a neutral card fill and muted outline. Its 16 px resource-based >_ marker occupies the existing centered drag lane; no extra column or larger card is added. Run/edit feedback stays confined to the action target, independently of row selection or macro progress.
- Power main and automation selectors use 32 px height, measured text width plus 16 px padding per side, and 12 px peer gaps. Automation selectors wrap with a 44 px row pitch; following content moves down with wrapped rows.
- Connection-dialog collapsed dropdowns are normalized to the shared 28 px visible height after their final font, native movement and item population, including editable baud rates. Refresh matches the actual final selector height. Local serial hardware gets a full-width first row beside Refresh; remaining serial settings use the same balanced two-column grid.
- The product version stays V2.0.3; r1 identifies the release revision only. Same-version installed clients require manual replacement; the updater continues to require a higher product version.

## Interaction and edge acceptance (V2.0.4)

- The shared keyboard-focus outline follows the control radius with a 4 px inset, instead of a rectangular dotted XOR focus box. Use light-theme accent / dark-theme `#6595FF`, and white on an accent-filled control; keep disabled states and hidden-focus cues respected. Checkbox labels use the same smooth outline, with 4 px label radius.
- AI history footer actions also use the 10 px button radius. No dialog-specific 8 px action corners.
- Review at actual 96/120/144/192 DPI in both palettes. Retain native-size screenshots plus 6x nearest-neighbor corner crops for normal, hover, pressed, focused, checked and disabled states. Enlargement is evidence inspection, not a replacement for actual DPI testing. Visible partially covered edge pixels are expected antialiasing; hard binary diagonal steps, clipped arcs, nine-slice seams, mismatched stroke widths and stale exterior pixels are failures.
- Rounded surfaces and focus outlines share destination-pixel area coverage at the actual DPI. Integrate the curve within each edge pixel; do not point-sample a reduced bitmap mask or rely on a separate low-resolution focus path. Use at least 32 vertical subpixel samples with continuous horizontal coverage, premultiplied alpha, and one complete-surface AlphaBlend. Preserve clear interiors for outlines. Cache the result and bulk-fill flat spans; do not supersample whole pages or blur text, icons, or straight edges. Quality acceptance uses curve-area accuracy and matched before/after desktop crops, not equality to the old rasterization.
- Command Run/Edit arm on mouse down and activate on a matching release; dragging out, cancellation and capture loss cancel activation. The system double-click interval bounds duplicate Run activations for the same command/session. Stop remains immediate and bypasses that guard. Enter/Space run the selected command once; F2 edits it.
- Ctrl+F6 (Shift reverses) moves between visible panes without transmitting bytes. Terminal Tab/Shift+Tab and unmodified function keys retain remote semantics. All application edit fields support Ctrl+A.
- AI Send keeps its label and becomes disabled while a turn runs. A separate Stop action appears in the utility row, away from Send's hit target. Repeated activation of Send must never interrupt a turn.
- Layout widths are stored as versioned 96-DPI logical values. Legacy pixel values are migrated using their saved DPI, or the current DPI when that historical value is unavailable.
- Per-session transport status retains its reason and time. Background errors mark their own connection card; selecting it acknowledges the mark and restores that session's reason without overwriting another session's state.


## V2.0.6 approved host identity recovery

- SSH and AI connection dialogs add one compact identity-status row and a text-role “重新核验主机” action. Retain the existing fields and footer geometry; the AI row uses space already reserved for connection feedback. Disable the action while connecting/connected.
- The shared native identity dialog is at most 600 logical px wide, fitting the monitor work area at high DPI, with 16 px insets, 12 px gaps and content-dependent height. Unknown hosts show one fingerprint card; changed or manually rechecked hosts show old/current cards. Use shared neutral card surfaces, 12 px radii, selectable read-only 13 px Consolas fingerprints, complete digest text and the normal theme-aware typography.
- A native automatic checkbox starts unchecked. The primary action remains disabled and IDOK is rejected until explicit verification. Both 160 by 36 px footer actions align right with a 12 px gap. Close, Escape and cancellation preserve trust. Reuse the existing dialog viewport on small work areas.
- Updating applies only to the displayed host and port. Clarify that connection recovery never replays AI commands. Password changes keep identity separate. No automatic acceptance, global cache deletion or new trust-management page.

## V2.0.7 approved terminal, power and workspace revision

- Measure Add Command using the actual font, DPI, icon and padding; switch atomically to a plus-only button when the full label cannot fit. Command dragging shows the existing card under the pointer, an insertion placeholder, bounded edge scrolling and cancellation. TXT file drop uses the same unsaved-draft import flow and never runs commands.
- Single-line input text uses measured symmetric vertical insets; multiline inputs stay top aligned. Preserve native selection, IME and accessibility.
- Power opens its existing USB/VISA and RS232 backends. The connection lane stays compact. Remaining space starts at 50:50 power controls/live terminal with an independent persisted ratio/collapse state, 12 px divider and the same monitor controls as AI. Reuse the real selected terminal; do not inject application status into device output. Power header and page selectors stay at the top, actions at the bottom, with open-ended separators and a scrollable body. Compact stacked channel cards retain all readings and settings; automation/settings/logs occupy the main body of their own page rather than following all channel cards. Keep 12 px peer gaps, 16 px insets, 16–18 px resource icons and existing deep-blue emphasis.
- A power recovery binding explicitly names a connected board terminal and the checked physical channels. Opt-in automatic recovery and local U-Boot profiles use the same compact themed fields, never a second confirmation per action. The native task owns the OFF-readback -> minimum 10 s wait -> ON-readback sequence, shows its actual state and supports Stop. No model sleep or unsolicited repeated restart.
- Every AI process starts collapsed, including streaming and restored history; show a concise state/count/duration summary and a separate final answer. Failed/stopped details remain available with a visible warning summary.
- The AI connection dialog defaults a new username to linux, offers an editable themed directory dropdown and refresh/up actions after verified connection, and validates real remote paths. New-conversation workspace selection uses the shared compact modal. Workspace is server/account/project directory; retain old conversation bindings, compact path labels and history scope without reconnecting the transport or adding a model selector. Prefer native App Server filesystem APIs with capability-tested fallback.
- SSH Enter, text paste and named key events share one terminal encoder, including negotiated keyboard mode. TUI mouse capture/scroll and local history remain distinct; the bottom horizontal scrollbar remains available. Preserve main/alternate screen and mode restoration on resize/exit. Names are internal: jiojio is disclosed only when asked, never as a persistent header.

## V2.0.8 approved AI companion

- Keep the existing AI/live-terminal divider and all peer 12px margins. A native child companion docks in the unused lower connection lane, above All Disconnect. Reserve its own area so connection rows and the footer remain usable; at insufficient height collapse the dock and retain the small paw entry in the AI header.
- Default character height96 logical px; offer120 and144, constrained by the available lane. Use a28px status bubble,16px paw/utility resources, the existing surface/border palettes, and the shared buffered drawing. A small24px ellipsis action and right-click open the same themed rounded menu. No new framework, browser, model call, name heading or expert role.
- On an actual entry to the AI page, schedule the approved eight-second forward/backward/forward hopping and single-hand boxing sequence after4000ms. Repeated layout/focus updates do not rearm it. Page exit/hiding cancels pending playback; minimize/modal pauses presentation, and resuming a pending opening starts a fresh four-second delay. User-selected playback takes priority and cancels the pending opening.
- Complete all three hops and a600ms landing pause before punching. Exclude the rejected double-punch pose. After playback restore the latest actual AI activity. Thinking, native tool activity, replies, completion, failure and stop are presentation states only; the companion never invokes a device or sends a prompt.
- Persist visibility, follow-state, opening playback, size and bounded in-dock position in the existing settings.ini. Provide keyboard/context-menu access, stop/restore actions and one compact themed settings dialog. Hidden/unavailable companions stop their paint timer; drawing is limited to their own viewport, and image resources are decoded once.
- Keep artwork provenance and CC BY-NC-SA4.0 notices separate from application code licensing; only the embedded PNG atlas is needed at runtime. Check both themes, four DPI scales, resize, entry cancellation, three-hop timing, manual stop and latest-state restoration.

## V2.0.9 desktop and process revision

- Desktop companion is an unowned, transparent Win32 top-level tool window, using premultiplied alpha and no activation while animating/dragging. Keep the approved artwork,96/120/144 body sizes and4-second AI-entry opening. Its224px logical viewport adds24px vertical travel clearance to the existing dock height. Transparent pixels pass mouse input through; desktop coordinates are constrained to monitor work areas and persisted separately from dock coordinates. Main-window minimization/page changes do not hide the desktop companion or replay the opening. Main application exit closes it. Offer topmost and dock mode in the same themed settings/menu; desktop mode reserves no connection-list lane.
- Command drag uses accent fill with white content for the moving card only. Clip the preview to its rounded silhouette, keep a neutral placeholder, and asynchronously invalidate only changed bounds. Mouse movement and idle edge-scroll ticks must not synchronously erase the complete list.

## V2.0.10 command dragging

- A press on the card body becomes a drag only after the system drag threshold. Run and Edit remain independently activated by a matching press and release. The rounded accent preview follows the pointer in both axes without activating another window; the source card is muted, and the preview shows the destination or cancellation instruction. The list uses an insertion indicator and does not rearrange other cards until release. Cancel with Escape, loss of capture or release outside the list. Edge scrolling is bounded and slow enough to select a target, and moving a preview must not erase and redraw the whole list.
- AI process records use human-readable Chinese action/result summaries. Keep the original structured tool result in an optional read-only technical detail dialog (640 by420 logical client area,16px insets,12px footer gap,36px actions, shared field styling/work-area fitting). Use actual newlines and bounded readable previews, not serialized JSON or terminal cursor-control streams. Keep evidence and execution semantics unchanged.
- Process groups default to folded across live output and history; a user expansion remains open while streaming, and completion folds it. A waiting indicator occupies a valid message rectangle and scrolls with messages. The floating jump-to-latest action belongs inside the transcript above the fixed utility row; it never replaces Analyze/Observe/Stop.
- UI event/terminal reception slices yield to input and painting. Reflow once per display batch, cache unchanged layouts and stable terminal-history projections, and measure long text in bounded chunks. Device enumeration runs after modal first paint on workers that cannot address a stale HWND.

## Approved compact UI polish (design review 2026-10-30)

- Preserve the seven navigation entries and their order; the shared navigation shell ends after AI. Settings is a detached 28px gear-only action immediately left of the28px theme action, with the existing12px gaps. Retain accessible names, tips, compact navigation and the command reminder only when it fits.
- Compact action targets are28px with14px notebook artwork or16px utility artwork and a7px corner radius. Keep standard buttons at their existing sizes. Save uses the existing disk resource; Import/Export use downward/upward tray arrows from the same atlas.
- Notebook actions form a left-aligned compact icon-only group with8px gaps at every panel width. Keep all commands, shortcuts, native focus, tips, Run/Stop semantics and editor geometry. Remove the closed filename-row box; editor and gutter share the notebook surface in both themes.
- Settings/theme and secondary notebook actions have no resting outline. Hover and press use a neutral fill; buttons retain a short neutral keyboard-focus underline instead of an inset blue closed rectangle. White focus cues remain visible on filled primary actions. Editable fields keep their existing single-layer focus boundary. Do not draw a second blue box inside a button.
- The connection-list title uses the approved16px session-sidebar resource (one window outline, left divider and two rows) and a connection-list tooltip/accessibility label. Preserve per-session Disconnect and Disconnect All text actions and all session-card behavior.
- Main panel outlines use the shared low-contrast contour token (#E5E5EA light / #343438 dark); field borders retain their original token. AI/power body geometry remains unchanged; their local controls follow the separately approved current-runtime preview. Power controls remain unavailable.


## Approved V2.0.14 AI/power finish

- The connection title uses option A, a16px session-sidebar glyph. Keep its28px title/tooltip area and original per-connection card icons. No chain-link, extra heading text or button frame is added.
- AI header actions stay in their existing28px cells with16px artwork. Send is a14px paper-plane resource in a28px target, retaining its accessible name, tooltip, shortcut and enabled/disabled state. Analyze/Observe use flat text actions; Stop retains its danger text and independent action.
- The composer keeps one neutral rounded contour while its target dropdown is frameless, with a neutral keyboard cue. Preserve native dropdown selection, popup list, full-name tooltip and source identity; other fields keep their normal boundaries.
- Power main/subcategory tabs use a neutral active fill and no repeated resting outlines; pressed feedback stays deep blue. Channel cards retain their size, scroll extent, contents and low-contrast contours. Disabled fields/actions remain disabled; readouts stay unknown until real responses. Never enable hardware controls as a visual change.
- Live terminal wrap/collapse actions use quiet flat surfaces without changing geometry, input, viewport, scrolling or PTY behavior. Continue using pure flat theme tokens and existing separators; do not reproduce incidental generated-image texture or typography changes.


## V2.0.15 dropdown, path and left rail corrections

- Preserve the native combo popup height and opening side; clamp only to the monitor work area. Do not expand long lists to the whole work area or reset row height on every pointer message. Keep native editable-arrow capture and wait until a selection is committed before rebuilding lists, opening dialogs or changing a terminal.
- Workspace forms use a40px path disclosure plus a12px gap and a28px current-child dropdown/action row. The disclosure shares the native field contour,14px/16px typography and the existing5px neutral overlay thumb. Single click reveals the full value and horizontal viewport; double click or Enter/F2 opens the existing owned edit dialog, Ctrl+C copies the full value. Rendering/scroll offsets never change submitted paths.
- The left rail reuses the48px collapsed-side width,28px fixed toggle and16px session glyph. Its200ms transition follows the existing side animation; header hit target stays stationary. Hide list, disconnect actions and status while collapsed/animating, preserve connections, receive processing and selected sessions. Remember state independently of right-side and live-monitor preferences.
