# 桌面截图复审及产品交叉复核补充

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

审核对象：`5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，`V1.0.6`。本报告补充 `04-desktop-independent.md`，不改写独立初稿。已阅读 `AGENTS.md`、README、UI-DESIGN-GUIDE 及产品、桌面独立报告，并重新核对下列生产源码；源码未修改。

## 结论与证据边界

**本轮真实桌面代表帧视觉复审部分通过；快速反向侧栏动画发现双主题重复箭头，动态无残影要求未通过。** 截图已从本轮 Windows 产物取回，目录 `evidence/windows-fresh/ui/serialctl-ui` 共 209 张 PNG。先前独立报告“尚未取得/看见截图”的时间性限制在本补充所列文件范围内解除。审核者使用 `view_image(detail=original)` 实际查看原图，不依据文件名或脚本 PASS 代替视觉检查，也没有把 209 张全部声称为逐张审阅。

环境来自 `ui-environment.json`：Windows Server 2022 / NT 10.0.20348、1920×1080、96 DPI（100%）、4 逻辑 CPU、约16 GiB 内存。没有物理 UART/IT6332A；SFTP 为隔离替身。团队在 runner 执行真实窗口和鼠标操作，审核者在本机查看取回的原始截图，未宣称本人在 Windows 桌面手动完成操作。

[source/scripts/smoke-ui.ps1:140–165](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/scripts/smoke-ui.ps1#L140-L165) 明确分开两类采集：`Capture-Desktop` 使用 CopyFromScreen；`Capture-Window` 使用 PrintWindow。前者支持本轮桌面可见画面观察；后者只能支持窗口绘制/布局观察。有限采样帧不能证明连续无闪烁；采样包含截图和 PNG 编码开销，也不是连续60 FPS录像。96 DPI 截图不能证明125/150/200%系统DPI、Win7或不同字体环境通过。

## 实际原图检查登记

以下文件均在 `evidence/windows-fresh/ui/serialctl-ui/`。表中两主题写作 `x64-{light,dark}` 表示对应 light、dark 两张均已原图查看。

| 原图文件 | 采集类型 | 实际观察与边界 |
|---|---|---|
| `x64-{light,dark}-drag-frame-5.png` | CopyFromScreen | 拖动中间帧中，中心输出、两侧卡片与独立滚动条边界清楚；命令Run/Edit未跨入终端；未见被审帧内的大块错色或旧按钮残留。不能证明相邻未采样时刻无残影。 |
| `x64-{light,dark}-resize-frame-6.png` | CopyFromScreen | 较小窗口帧中六项导航、两侧footer和命令加号可见；长状态自然换行，终端仍显示输出。未见被审帧内文字上下裁切或footer重叠。终端在小宽度下显示范围缩小属于空间约束。 |
| `x64-{light,dark}-animation-frame-3.png` | CopyFromScreen | 动画中右栏只剩折叠动作，命令卡片/footer隐藏，符合指南动画时隐藏侧栏动作；中心与侧栏未交叠。不同主题中间宽度不同，不能把采样序号当严格同步动画进度。 |
| `x64-{light,dark}-power-tab-frame-5.png` | CopyFromScreen | 自动化页选中态、三通道卡片、右侧48px rail可见；未知物理读数显示横线、未连接状态可读。底部表单超出当前视区时已有纵向覆盖滚动条；本帧的视区裁切不能直接定性为控件不可访问，也未实测滚到底。 |
| `x64-{light,dark}-sftp-column-frame-5.png` | CopyFromScreen | Name/Size/Modified表头和文件行排齐，路径面包屑、传输区与下方四动作分离；终端受控输出仍可见，长输出以续行显示。该图只支持替身SFTP的布局，不证明真实传输或重启持久化。 |
| `x64-{light,dark}-command.png` | PrintWindow | 单步对话框名称/指令单列，指令加号完整，输入框与footer圆角完整；取消在左、确认在右；未填指令时确认变淡。没有原生客户端深色方角伸出圆角。不能用窗口完整图证明屏幕可见范围。 |
| `x64-{light,dark}-macro-ten.png` | PrintWindow | 十个步骤仍保持紧凑单列，删除按钮与字段齐高，最后字段、间隔和footer在窗口绘制图内完整。**不能解除UI-03高DPI/工作区越界候选**。发现新增UI-V01：到达十步上限后加号与“添加指令”仍以可操作的蓝色显示。 |
| `x64-modal-foreground-before.png`、`x64-modal-owner-after.png` | CopyFromScreen | 前图命令modal位于主窗及Notepad之前；后图modal消失、主窗仍在Notepad前，新增草稿卡及“保存*”可见。与脚本 foreground断言一致；不能仅由截图证明owner链、焦点控件或后台关闭不抢焦点。 |
| `gui-connections-drag-1.png`、`gui-connections-drag-3.png` | CopyFromScreen | 大量连接列表分别显示#13起和#1起，窄滑块位于独立右侧通道，未盖卡片文字；固定“全部断开”footer保持可见。列表最末半行受视区裁切是滚动容器表现。仅深色大量连接帧在本组核验。 |
| `x64-light-drag-frame-0.png`、`x64-dark-drag-frame-11.png` | CopyFromScreen | 浅色较窄右栏把“添加命令”缩为完整加号，深色较宽时图标及文字均可见；没有用裁掉半个标签来适配。与第5帧比较，边界确有变化。 |
| `x64-{light,dark}-resize-frame-11.png` | CopyFromScreen | 与第6帧相比可见区域增大、终端输出序号前进；所审扩大后帧未见命令footer漂出卡片。 |
| `x64-{light,dark}-animation-frame-9.png`、`x64-dark-animation-frame-7.png`、`x64-dark-animation-frame-8.png`、`x64-light-animation-frame-8.png` | CopyFromScreen | **深色第9帧同时出现两个左箭头、浅色第8帧同时出现两个右箭头**；其他三张为单箭头。这是UI-V02的直接帧证据，不能继续以最初第3帧正常概括整个动画。 |
| `x64-dark-api-cmd-visible.png` | PrintWindow | 后续静态主窗显示一个右栏折叠按钮、完整命令列表和footer。它不是与动画同步的真实桌面终态证明，不能用此图否定前述瞬态重复。 |

以上为28张原图；文件名、尺寸和SHA256见[原图审阅清单](../evidence/desktop/visual-inspection-manifest.json)。多个桌面帧背景可见runner/Notepad窗口，并不属于SerialCtl内部白块。不要把这些应用外部窗口误判为产品闪烁。

`gui-lifecycle.json`另提供35个真实CMD会话、`known_children_all_exited=true`、清理0.375秒、拖动前后列表锚点；这些是操作/进程断言，不能由两张列表图独立推导。`x64-dark-sftp-sustained.json`记录paint从446到529，支持列拖期间受控stdout仍触发终端绘制，不等于显示器物理呈现无丢帧。

## 新发现 UI-V01 / P3：达到十步上限后“添加指令”禁用状态仍画为正常蓝色

证据为上述两主题 `macro-ten.png` 和源码交叉：[MainWindow.cpp:6384–6385](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6384-L6385) 在步数达到 MaximumCommandSteps 时调用 `EnableWindow(..., false)`；但是`:2708–2722`的加号/添加动作专用绘制分支始终使用`colors.accent`绘制图标和文字，没有读取`enabled`。正常按钮的`:2732–2735`才有禁用填充和muted文字，因此此问题不能由“系统已禁用”反证视觉正确。

预期：达到上限时同一加号位置使用共享muted token，明确不可再添加，并保持形状/位置。实际：两主题十步PrintWindow图中仍是与可用状态相近的蓝色加号及“添加指令”。影响为用户继续尝试点击、误以为点击无响应；这是禁用外观一致性问题，不是超过十步上限的证据。

修复方向：复用共享enabled/disabled文字与图标token，无需新增颜色或改对话框结构。验收：9→10步后按钮HWND确为disabled且变淡，10→9恢复正常；两主题同时比对真实桌面与PrintWindow，确保二者绘制一致。当前没有补跑这一实际桌面点击序列，因此严格表述为**源码确认+PrintWindow视觉支持**。

## 新发现 UI-V02 / P2：快速反向侧栏动画的真实桌面帧出现重复箭头

**帧级实际观察已确认，持续时间及底层原因待补测。** `x64-dark-animation-frame-9.png` 的右栏顶部在屏幕约`x=969–1037,y=133–161`同时可见两个完整左箭头按钮；`x64-light-animation-frame-8.png` 在约`x=1073–1128,y=133–161`同时可见两个相邻右箭头。两图均为1920×1080 CopyFromScreen，重复位置在应用内部。相邻深色第7/8帧及浅色第9帧各只有一个按钮。本补充最初只看第3帧没有发现此现象，扩展查看后结论相应收紧。

触发证据：[smoke-ui.ps1:383–386](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/scripts/smoke-ui.ps1#L383-L386)反复触发125号侧栏切换，每次等待45ms后采集桌面；实际间隔还包含采集和编码时间，不宜写成精确45ms频率。此阶段终端持续输出。预期任一可见画面只保留一个折叠/展开箭头；实际至少这两张帧呈现重复图标。影响为动画短暂重影/画面不同步，尚无功能误触或长期遗留的证据。

源码关联仅定位调查范围：[MainWindow.cpp:2237](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2237)只创建一个`rightPanelToggleButton_`；`:2337–2340`按动画宽度移动其位置；`:2407`、`:4854`动画期间仍显示它；[UiStyle.h:262–275](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/UiStyle.h#L262-L275)批量移动子窗并使变更子窗失效。该事实排除“设计上有两个箭头”的解释，但**不能仅凭这些源码确定是旧区域擦除、父子窗绘制时序，还是桌面采集恰逢多次绘制的组合帧**。需要给窗口位置和paint/capture打时间戳进一步定位；这不撤销实际采样桌面出现重复图形的观察。

修复与验收方向：集中检查移动控件旧/新边界的背景恢复和父子窗合成时序；保持指南的200ms cubic动画、终端持续输入输出，以及只有终点才重排PTY的约束。用两主题真实桌面连续录像/高频无重编码采样覆盖快速反向，核对一帧只显示一个箭头、旧区域及时恢复；同时做静止终端和持续stdout两组。不能仅以最终PrintWindow正常或脚本坐标断言通过关闭此问题。

## 与产品问题的交叉复核

### P-01 宏状态误导：认可，定位为提交进度与执行完成混淆

重新核对 [MainWindow.cpp:3777–3806](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3777-L3806)：每一步只检查`SendBytesToActive`的bool，最后一步成功即`StopCommandSequence(false)`并显示“执行完成”。`:3617–3624`对本地串口/CMD调用SessionService.Input，只以没有error作为成功；[src/network/SessionService.cpp:108–113](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L108-L113)返回`state=queued, execution=unknown`。`:3814–3826`的Stop只取消后续定时提交、清宏运行字段，不撤回已提交动作。

因此产品报告所述语义缺口有独立源码依据。截图证明宏编辑界面可绘制，**没有证明慢命令尚未结束时UI已宣称完成的真实GUI复现**。建议保留P2，使用“已提交N/N；执行结果见输出”“已停止后续提交”等文案和与之相符的进度含义，不把停止图标变回运行图标当设备已停。

必要验收仍是两步慢CMD、外部完成标记、时间戳同时采集，并在第一步运行时停止后续提交；不能只检查回显或按钮复位。

### UI-01 键盘入口缺失：认可静态缺口，不扩大为所有键盘功能失效

重新核对 [MainWindow.cpp:2180–2186](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2180-L2186) 的CreateChild不会自动加WS_TABSTOP，`:2204–2246`主导航、连接列表、命令列表、全部断开和添加/保存等未传此样式，只有`:2218`终端显式拥有。`:1064–1065`终端索取Tab/all keys；`:1724–1736`对终端不走IsDialogMessage导航；`:3740`把F6作为终端转义序列。`:1468–1557`命令卡Run/Edit是鼠标命中区，未见其对应键盘动作。

因此35会话的Home/End断言仅证明**已经对准列表后**能操作，不证明仅靠键盘能从终端进入列表或访问断开。对话框的Tabstop/电源子控件已有独立路径，不能说整个程序“完全无键盘支持”。保留UI-01/P2，并补真实焦点轨迹与设备TX断言；需保留终端Tab语义，新增明确的退出/切换窗格途径。

### UI-02/UI-03及UI-04：范围不变，更新证据状态

UI-02高DPI持久宽度缺口不由100%截图证伪；UI-03十步高度候选仍缺真实工作区坐标/DPI验证。UI-04“未人工看过帧”限制在本报告28张原图范围内解除，但字体度量、取消/后台modal、顺序变动及重启列宽的脚本断言缺口仍存在；并已发现UI-V02说明动态帧人工审阅有实际新增价值。[MainWindow.cpp:6382](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6382)才是当前SHA的ResizeDialogClientDlu调用，独立报告`:6389`指向布局函数结束，引用修正不改变候选实质。

报告不新增生产修复、不改独立初稿。后续应优先补真实键盘焦点、慢宏语义、十步系统DPI/工作区、连续重绘录像和Win7现场矩阵，再把有限截图通过扩大为发布验收。
