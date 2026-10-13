# 第四位审核者：高级原生 Win32 桌面交互独立审核

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

审核对象：`fix/unified-websocket-win7`，完整 SHA `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，`VERSION=V1.0.6`。源码只读，未修改生产实现。本报告首次结论在阅读其他三位审核者的结论前形成。

## 独立结论及证据边界

结论为**部分通过，桌面视觉审核尚未完成**。代码可核对到统一字体/颜色、覆盖滚动条、批量布局、主/备用终端区分、条件恢复模态 owner 等具体实现。本轮 Windows Server 2022 的真实 EXE 窗口操作烟测完成，但不能据此宣称 Win7、全部 DPI、真实 SFTP 或所有重绘场景通过。

本审核者独立检查了本轮运行 `38097145874` 的原始日志及其脚本断言：两主题真实 CMD 输出/逐键编辑、35 个真实 CMD 会话与进程回收有行为断言，并非只有“窗口存在”。见 [本轮 Windows 原始日志](../evidence/windows-fresh/workflow-logs/0_build.txt)，日志行 391–392、423–424、434、445。该轮来自本轮触发，不用文档中的旧成功运行替代。

**截至报告写入时，尚未取得或看见截图。** Actions 已上传 `SerialCtl-UI-evidence`（artifact ID `11686766435`），但原始 artifact Blob 下载被当前网络代理以 403 拒绝，见 [下载失败记录](../evidence/windows-fresh/SerialCtl-UI-evidence-download.txt)。我没有在本地 Windows 桌面亲自操作；团队通过 Windows runner 执行了真实窗口消息、真实鼠标拖动和桌面采集。此处仅认定日志和断言所能证明的行为，不认定“无闪烁”“中文图标视觉完整”或“已人工查看中间帧”。截图取回后的复审必须补充。

已按要求依次阅读 AGENTS、README、architecture README、UI-DESIGN-GUIDE、build-release，再阅读 `docs/testing` 的 V1.0.3 至 V1.0.6 验收、MANUAL-REGRESSION、WIN7-HARDWARE-TEST。生产分析限定 `src` 与原生脚本；未把 `legacy/csharp` 当生产实现。

## 问题清单

严重度：P1 阻断核心工作或明显安全风险；P2 重要可用性/数据保持缺口；P3 次要一致性。静态确定表示代码路径已确定，不代表完成真实 Windows 复现。

### UI-01 / P2 / 已确认代码缺口、实际键盘复现待补：主窗口操作控件没有 Tab 导航入口，终端没有焦点退出快捷键

- 位置：[src/app/MainWindow.cpp:2180](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2180) 的 `CreateChild` 仅加入 `WS_CHILD|WS_VISIBLE|style`；`:2204`、`:2214`、`:2215`、`:2240` 起创建导航、全部断开、连接列表、命令/SFTP 操作时未传 `WS_TABSTOP`。主窗口唯一明确的 Tabstop 为 `:2218` 的 terminal。[PowerPane.cpp:97](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/PowerPane.cpp#L97) 的电源子控件则有 Tabstop。
- [MainWindow.cpp:1724](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L1724) 的 PreTranslate 只在非终端目标时调用 IsDialogMessage(Tab)。`:1064` 终端声明需要 Tab/all keys，`:1090` 接收 WM_CHAR，`:3641` 起把 Tab 放入 CMD 草稿，`:3734` 把 Shift+Tab 转为终端序列；F6 同样在 `:3740` 发给远端。源码未发现用于退出终端焦点的快捷键或主导航助记键。
- 触发/复现：启动并点击 CMD 将焦点置终端，仅使用 Tab/Shift+Tab/F6，尝试进入“全部断开”“添加命令”或连接列表，再通过键盘选择会话。记录每次 GetGUIThreadInfo.hwndFocus、目标 HWND 和实际设备输入。普通 Tab 仍应保留给终端用途。
- 预期：用户有明确、不发送到设备的途径离开终端并巡游界面；列表和关键操作可用键盘到达。代码实际：以上候选键都留在终端，主操作没有 Tabstop；因此无法由现有测试推断键盘工作流完整。
- 影响：鼠标不可用、精细键盘作业及辅助操作用户；特别是 CMD 不支持 Ctrl+C 时，需要可靠访问断开动作。
- 修复：为可交互控件补标准 Tabstop/分组，保留终端 Tab 语义，选择并展示一个明确的“切换窗格”组合键；命令卡 Run/Edit 是绘制命中区（`:1467`），还需列表 Enter/F2 或上下文菜单等键盘入口。沿用现有布局，不增 composer。
- 回归：真实键盘从终端到连接列表/断开/命令编辑并返回；断言焦点对象与设备原始 TX，导航键不得误写设备。两主题焦点环必须可见。

### UI-02 / P2 / 已确认静态逻辑与算式复现、高 DPI 窗口实测待补：记忆侧栏宽度使用物理像素，重启前按 96 DPI 范围截断

- 位置：[MainWindow.cpp:1766](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L1766) 先 LoadUiState，再 CreateControls；`:2190` 才 ScaleUiMetrics。`:4800` 因此按未缩放的 260–650 限制持久值；`:1842` 拖动用已缩放上限；`:4817` 原样保存物理宽度。SFTP 名称宽度 `:4803/:4816` 也没有 DPI 单位转换。
- 复现条件：真实 150% 或 200% 系统 DPI、足够宽的屏幕/窗口，在可用空间内将侧栏拖至超过 650 物理像素，退出再启动，保持相同 DPI 和窗口宽度。
- 预期：恢复相同逻辑宽度；实际代码：例如 150% 保存 975 后读取成为 650，200% 保存 1300 后成为 650。可用空间不足另会正常限制宽度，应从本缺陷复现中排除。
- 证据：[只读公式复现脚本](../scripts/desktop_layout_model.py)、[已运行结果](../evidence/desktop/layout-model.json)。这复现了源码数值路径，**没有替代真实高 DPI 运行**。脚本假定窗口有足够可用空间。125% 的 MulDiv 四舍五入应以 Windows 实际值为准，不以 Python 的银行家舍入作为像素级判据。
- 影响：使用宽 SFTP 列或命令面板的高 DPI 用户，每次重启需重拖；跨 DPI 的物理像素持久化也改变逻辑尺寸。
- 修复：配置保存为 96 DPI 逻辑单位，加载后统一缩放/限幅；旧配置加入迁移约定。不要简单把所有尺寸变大。
- 回归：96/120/144/192 DPI 分别保存、重启、跨 DPI 重开，比较逻辑宽度；覆盖双击恢复默认宽度（`:1827` 原样写 360）与 SFTP 列宽。

### UI-03 / P2 候选 / 未运行：十步命令窗口缺少工作区高度约束或滚动，可能在高 DPI/小工作区丢失 footer

- 位置：[MainWindow.cpp:6331](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6331) 的 LayoutCommandDialog；`:6389` 按步数 ResizeDialogClientDlu；`:856` 直接 MapDialogRect 后扩大窗口；`:468` CenterOnOwner 只限制坐标，不缩小窗口。`serialctl.rc.in:45` 为不可调整尺寸的普通模态模板；未见该对话框滚动容器。
- 算式：10 步对应 328 DLU 客户区高度；其像素尺寸依赖实际对话框字体/DPI。固定 328 DLU 不可直接声称等于某个像素高度。
- 复现：真实 1366×768 与 1920×1080，在 150%/200% DPI 打开已有十步命令，以及逐步从 1 加到 10；取 GetWindowRect、监视器 rcWork、最后一行/保存取消屏幕坐标。逐步增加时还需检查仅首次 CenterOnOwner 后是否从原坐标向下越界。
- 预期：十条和保存/取消始终可访问；候选实际：若换算后大于工作区，坐标钳制不能令 footer 可见，键盘 Tab 可能聚焦屏幕外控件。
- 修复：保持指南 238 DLU 单列与现有 footer，以工作区计算最大高，只让步骤区域内部滚动，footer 固定；或提供同等可访问的紧凑布局。不要改成无关的大窗口/多列版。
- 回归：实际系统 DPI 矩阵，逐步加行和打开十步两种路径；截图连同 HWND bounds，不只用 PrintWindow 离屏完整窗口图判断可见性。

### UI-04 / P2 验证缺口 / 已确认脚本事实：若把现有 PASS 当完整视觉与可访问性验收，会产生过度结论

- [scripts/smoke-ui.ps1:74](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/scripts/smoke-ui.ps1#L74) 的 AssertLabels 重新创建固定 -15 px 字体，只查 Static；不验证目标进程实际字体、所有 owner-draw Button 文字、图标边界或高 DPI。现有 96 DPI 的日志仍有价值，不能推为 125/150/200%。
- `:422` 启动 Notepad 后先把模态强制置前，`:431` 仅断言保存后的 foreground 等于主窗；未断言 owner 链、焦点控件、取消后相同条件，以及模态在后台时关闭不应抢前台。
- `:435`–`:439` 发送模拟拖动消息后只断言 commands.txt 未写盘；即使排序没有发生，该断言仍通过。需另外断言 draft 顺序改变且保存后精确顺序。
- `:548` 断言 SFTP 列宽变窄，`:545` 断言同时有 stdout paint；结尾 PASS 中的“persisted column resize”未执行重启后宽度对比，无法证明持久恢复。
- 中间帧使用 CopyFromScreen 是正确的真实桌面采集方法；但程序化切换+截图和几何断言不自动识别白块/残影，采样间隔还包括 PNG 捕获/编码，不能当连续 60 FPS 录屏。无闪烁结论需查看原始连续帧/视频并限定采样范围。
- 修复/回归：将 PASS 拆成“窗口行为断言/字体度量/真实桌面人工审阅”三类；增加真实 SendInput/人工 IME、焦点和 DPI 检查，按工作区检查完整交互区域。不要删掉已有真实行为断言。

## 已核对的实现与本轮测试能证明什么

| 范围 | 证据及可支持结论 | 状态/限制 |
|---|---|---|
| 浅色默认与深色 | 源码 Palette/ApplyTheme 和脚本双主题循环；本轮到达最终 PASS | 部分通过；截图未取回，视觉未审 |
| Win7 原生体系 | manifest system-DPI-aware；C++/Win32/GDI 和运行时字体回退 [MainWindow.cpp:689](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L689)；未增加浏览器/.NET 生产依赖 | 源码符合方向；Win7 未运行。测试用 C# 替身与生产运行时依赖应分开 |
| 导航/加号 | PNG 资源导航 `:2622`，UiPlus [UiStyle.h:301](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/UiStyle.h#L301)，右栏文本阈值缩放 [MainWindow.cpp:2348](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2348) | 源码正向证据；按钮裁字/高 DPI 完整性未验证 |
| 最小窗口/最大化恢复 | smoke-ui 改真实 HWND 为 980×620，AssertPanel 检查边界；ShowWindow 最大化恢复 | 本轮行为断言部分通过；不能证明所有内容与视觉状态 |
| 侧栏拖动/快速反向 | UiLayoutBatch 避免重复位置变更；动画 cubic 200 ms；动画时隐藏侧栏控件，终端重排延后。烟测真实鼠标拖动并确认坐标改变 | 部分通过；帧未查看，无“无残影”结论 |
| SFTP 列宽 | 本轮替换隔离目录的 plink/psftp 为固定输出进程；实际 terminal paint 与鼠标表头 drag 有断言 | 控件路径部分通过；没有真实 SSH/SFTP 传输验证，持久化回读未验证 |
| 电源分页 | PowerPane 在窗口绘制用离屏 bitmap；LayoutBatch 显隐；脚本快速切页并采物理桌面帧 | 部分通过；未接 IT6332A，不能认定参数监测编辑稳定或无旧高亮 |
| 电源刷新保护 | [PowerPane.cpp:443](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/PowerPane.cpp#L443) 相同状态跳过；`:479` 仅特定结果填参数，`:498` 跳过当前聚焦编辑框 | 有具体源码措施；跨字段未提交草稿/异步操作完成时的保护须真后端补验 |
| CMD 行编辑/回到实时 | 本轮 gui_gateway_smoke 顺序 WM_CHAR/WM_KEYDOWN 编辑后等待真实 stdout；Unicode/代码页/历史/剪贴板实际输出断言；后台历史与主动输入路径 | 本轮路径通过；消息级键盘测试不证明物理键盘/IME 合成、候选框与焦点；Ctrl+C/全屏程序有明确边界 |
| CMD 显示延迟 | 本轮 light P50/P95/P99 5.597/6.686/8.451 ms，dark 5.283/6.154/6.619 ms，各100样本 | 回调到 BitBlt 的受控短测，非显示器物理呈现/Win7性能指标 |
| 大量连接/列表 | gui_disconnect_smoke 35 CMD、刷新顶部锚点、Home/End、真实覆盖滑块；本轮日志清理 0.375 s | CMD 范围通过；程序化列表键消息不证明从终端可通过键盘到达列表 |
| 全部断开 | [MainWindow.cpp:3511](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3511) 取消连接/SFTP，Stop transports/logger、join 工作者；电源采用异步收尾状态。脚本断言真实 CMD 子进程及记录过的子进程均退出 | CMD 本轮通过；实体串口释放、真实 SFTP 和硬件电源任务未运行，不能扩大 |
| 模态 owner | UiEndOwnedDialog 仅模态当前在前台时设恢复标记；UiModalOwner 一次恢复 owner 与原焦点 [UiStyle.h:174](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/UiStyle.h#L174) | 代码避免全局持续置顶；本轮保存前台断言通过，Win7/取消/后台关闭/实际焦点待补 |
| 历史/备用屏/时间戳 | 主/备用模型与渲染路径区分，terminal input 回实时 [MainWindow.cpp:3644](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3644)；日志断言属于模型/业务测试 | GUI 的中文宽字、拖选/阅读锚点、连缩放尚需截图与连续操作；不能由 README 定案 |

不是全局置顶缺陷：`WS_EX_TOPMOST` 出现在 owner tooltip 创建上（状态和 SFTP 提示），不等于主窗口始终置顶。`SetForegroundWindow` 也用于用户触发的 context menu；不能仅按字符串命中认定“反复抢前台”。

## 实施顺序与人工补验

1. **立即**补键盘焦点导航与持久尺寸单位，分别解决无法仅键盘完成任务和高 DPI 重启丢宽度。低至中成本，复用现有控件；必须验证导航快捷键不发送到设备。
2. **近期**在实际工作区约束命令编辑高度，保留单列/保存取消/现有按钮角色；增加布局与保存重读断言。风险为 Tab 顺序及动态步骤焦点，回归每次增删行。
3. **发布前**实际 Win7 与 100/125/150/200% 系统 DPI，使用鼠标/键盘/IME 操作，并保留物理桌面连续帧。没有现场系统时不得填“通过”。
4. **后续**将散落在 MainWindow 的布局、焦点导航和字体度量抽成少数可测共用方法，不重写 Win32 UI、不引入新运行时；由用户工作流收益约束范围。

可执行补验流程（对生产实现不作修改）：

- 在干净用户配置副本/独立 Windows VM 中解压审核包，记录 MANIFEST、exe SHA-256、系统版本、屏幕分辨率、系统 DPI、字体。系统设置中改缩放后重新登录，再启动程序；拉伸截图不算 DPI 测试。
- 每个 DPI/主题分别执行 `.\scripts\smoke-ui.ps1 -OutputDirectory <绝对证据目录>`，同时记录脚本现有固定 96 DPI 几何假设，不能将高 DPI 几何断言直接误判为产品失败。保留本轮已有原始脚本。
- 使用系统桌面录像/外部相机或 CopyFromScreen 循环采集；不用 PrintWindow 证明动态无闪烁。录连续调整窗口/右栏、每 50 ms 反向收起、SFTP 名称列拖动、Power 快速分页。采集时间戳与丢帧统计，记录前/中/后真实画面。
- 真实 CMD 保持 `for /L %i in (1,1,10000) do @echo %i` 输出；用物理键盘逐键编辑 `echo 中文`、左右/Home/End/删退、IME、粘贴多行。用原始 stdout/日志核对执行结果；另录回看历史期间后台输出和主动输入回实时。
- 复现 UI-01：Tab/Shift+Tab/所选窗格导航键；用 Spy++/UI Automation 检视 focus。记录出现功能前后的焦点，不只用按钮发 WM_COMMAND。
- 复现 UI-02：足够大工作区将侧栏拖至 650 物理像素以上；记录 settings.ini 和 HWND 宽度，正常退出重启再量。不接受只看保存文件存在。
- 复现 UI-03：1→10 步与直接打开10步，检查最后字段和 footer 的屏幕坐标，键盘也能访问。记录工作区/对话框实际矩形。
- 模态分别做保存与取消；Notepad 覆盖后把模态带前、再关闭；以及模态保持后台时关闭。断言 owner、enabled、foreground、focus、z-order；主窗不得全局置顶或重绘时反复抢前台。
- 真 SSH/SFTP、实体 UART 和 IT6332A 按 `docs/testing/MANUAL-REGRESSION.md` 和 `WIN7-HARDWARE-TEST.md` 执行，缺设备逐条写“未运行”。监测编辑时保留未提交文字并等待异步完成，完成后再核对设备读回值。

本报告的源码数字证据和脚本审查不弥补缺失的截图人工审阅；统一报告应保留此限制。
