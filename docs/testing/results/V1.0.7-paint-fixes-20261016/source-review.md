# 独立源码复审：按钮首次显示白块与主题漏刷新

2026-10-16。审核者：独立 Win32 绘制/布局复审代理。只读生产源码；没有修改生产实现、提交或远端。基线 `b8fd3a7fd8a465e98da0c24d87687d9a6bc094a2`，生产实现对应 `906275761ea9729a87e2090b9458ebbd96ba0b19`。已读 AGENTS、README、架构/UI 指南/构建约束、两轮桌面报告及相关实现，并实际查看既有原图 `x64-dark-desktop-0105-sidebar-expand-animation-03.png`。本文是修复前独立分析，不替代修复后的真实 Windows 操作。

## 已确定事实

1. `src/app/MainWindow.cpp:1385` 的 `ButtonSubclassProc` 当前让 `WM_ERASEBKGND` 进入原生按钮默认处理；mouse move/leave/focus 会请求擦除。主窗口在 `:1998` 返回 1 并不能替代子 HWND 的擦除策略。按钮没有统一 `WM_CTLCOLORBTN` 分支。
2. `:2702` 的 `ODT_BUTTON` 分支在任何按钮类型的提前返回前，先以本主题的 outside brush `FillRect` 整个 `rcItem`，再画形状、文字、图标。包括圆角外部，所有按钮都有完整背景绘制路径。禁用、hover、pressed、focus 仍由现有 draw 状态处理。
3. `LayoutControls` 在 `:2522` 先调用 `ShowSftpPanel`，而几何批量提交在 `:2524`。`ShowSftpPanel :4963` 对一系列控件单独 `ShowWindow`。因此控件存在先可见、后提交最终几何的实现顺序；不是整个侧栏几何与显隐同批。
4. 同一函数用 `GetClientRect(sftpList_)` 决定日期列显隐。这在尚未提交本次布局时读取的是旧宽度。
5. `ApplyTheme :3075–3087` 的手工重绘列表明确遗漏 `disconnectAllButton_`。主窗口有 `WS_CLIPCHILDREN` (`:1049`)，主窗重绘不能被视为所有子控件已重绘的证据。已看到旧截图的白块，源码漏项是明确缺陷；“每次主题切换一定复现白块”仍需实测。
6. 0105 原图的六块浅色区域恰落在命令顶栏和底栏按钮，全部是 BS_OWNERDRAW。此前 `:2442` 固定 toggle 的几何以及 `:1390` 快速点击处理与本问题无冲突，应保留。

## 建议实现与取舍

### 1. 拦截 owner-draw 按钮的默认擦除

在现有 ButtonSubclassProc 前段，仅当 `(GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW` 且消息为 `WM_ERASEBKGND` 时返回 1。由 DrawOwnerItem 在正常 paint 路径完整填充背景。不需要先把按钮涂黑，不需要改变主题 token，不需要替换原生鼠标按下/释放、Space、取消或焦点路径。

**为什么 guard 必须保留：**同一个 subclass 也用于对话框里的原生 Button；将“无需擦除”无条件应用到未来或现有非 owner-draw 按钮，没有完整绘制保证。

**为何仅返回 1 不能自动等于全部修复：**它消除独立系统背景擦除，但不会使所有子控件显示/绘制原子化，也不会解决错误显隐顺序或漏掉 invalidation。实际截图/录像还可能看到旧几何、旧主题或延迟绘制；这些需要下面两项处理及真实回归判断。

现有 DrawOwnerItem 始终填充全矩形，故正常 GDI 成功路径满足抑制背景擦除的前提。该函数不会依赖系统擦除来补圆角外像素。不要为本问题新增全局 `WS_EX_COMPOSITED`、每动画帧 `RDW_ALLCHILDREN|RDW_UPDATENOW` 或反复强制前台；这些扩大影响并可能加重延迟。

### 2. 几何与侧栏显隐统一批量提交

推荐提取只负责 `batch.Show` 的私有 helper：参数为已有 `UiLayoutBatch&` 和本次 SFTP 列表计划宽度。`LayoutControls` 在同一个 batch 内完成所有命令/SFTP 控件 Move 和 Show/Hide，然后 Commit。外部 `ShowSftpPanel` 保持原有允许条件、模式 state 更新，建立局部 batch 调 helper，读取当前已提交列表宽度后 Commit。

统一 helper 必须覆盖 commandHeader、sftpTab、toggle、命令列表/所有命令按钮、path、三个 SFTP 列头、文件/传输列表、transfer disclosure/clear、四个 SFTP action。保留动画/电源/自动收起时只显示 toggle 的规则，以及传输抽屉 expanded 与日期列宽度条件。

较小但不够完整的替代是把 ShowSftpPanel 移到 Commit 后，能去掉旧几何曝光，却仍逐个 ShowWindow，并保留双重可见性来源。建议优先 helper；无需修改通用 UiLayoutBatch 或 PowerPane。动画终点/SetRightPanelCollapsed 已执行 LayoutControls 后的冗余 ShowSftpPanel 可删除，但动画开始必须及时隐藏动作，不能误删那条唯一隐藏路径。

### 3. 主题切换覆盖全部控件

最小修复是将 disconnectAllButton_ 加入现有列表。为避免新增控件再次遗漏，也可主题切换时枚举主窗口子控件统一 InvalidateRect。仅主题改变时执行，不增加动画刷新。PowerPane 自己的 Theme 仍负责内部颜色状态；枚举重绘不能代替该调用。

不建议主题切换过程中额外对每个控件逐个 UpdateWindow；应先更新全部颜色/brush/theme 状态，再统一 invalidation。原本主窗 UpdateWindow 的存在不代表每个子 HWND 都同步更新，测试应观察 idle 后的真实帧。

## 必须观察的回归

- 真正的 Windows 程序：浅/深主题、命令/SFTP（后者可明确标识子进程替身）、持续 CMD 输出；至少 100 次正常和快速反向展开收起，录制连续屏幕，不以 PrintWindow 证明没有闪烁。
- 保留固定按钮边界、每次 released click 反向、同点 double click、列表连续键盘导航与鼠标激活终端焦点的现有断言。
- 首次打开、light→dark→light 的全部断开按钮，按钮正常/hover/pressed/focus/disabled；不能只抓断开后的最终一张图。
- Sidebar splitter 缩到/放大过 300 px 日期列阈值；检查显隐反映本次布局，传输抽屉展开/收起没有叠压；移动/缩放中终端持续实际 stdout。
- MainWindow 共享 subclass 影响的连接/命令/SFTP 对话框 Save/Cancel 按钮及焦点/owner；视觉检查控件边缘和禁用态。
- 源码审查及 Windows Server 2022 通过仍不能证明 Win7、高 DPI、真实物理串口或现场 SFTP 已通过。

## 结论边界

可以确认三个可直接修正的实现问题：原生 owner-draw 背景擦除未屏蔽、侧栏显隐抢在批量几何提交前、主题刷新漏控件。前两项与既有浅块的因果关系在源码上合理，但缺少逐消息 QPC 对照，因此应作为有依据的修复机制，再以真实连续操作消除原复现作为回归证据，不能宣称已排除全部 Win32/DWM 闪烁来源。
