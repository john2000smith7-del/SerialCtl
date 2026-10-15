# V1.0.7 桌面修复后的独立原图视觉复审

复审日期：2026-10-15。复审者：独立桌面视觉子代理 `finish_visual_review`。已阅读 AGENTS.md、README、UI-DESIGN-GUIDE 和上轮桌面报告；没有修改生产代码、测试或图片，没有提交或更改远端状态。

本次独立逐张打开并查看下列 **29 张原始桌面 PNG**，覆盖浅/深主题、普通侧栏展开/折叠、同点快速反向、窗口边框按住缩放、内部分隔线拖动以及 SFTP 列宽按住/释放。不是基于 README 或仅基于自动断言判定视觉结果。

程序源码为 `906275761ea9729a87e2090b9458ebbd96ba0b19`，测试提交为 `ea9d0f3`，Windows 真实桌面运行 [38751634512](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38751634512)。原图来自 `/workspace/serialctl-desktop-fixes-20261015/actual/`，实际采集为 Windows Server 2022、1920×1080、96 DPI；不是 Win7 实机。该目录全部文件及程序摘要的核验由主执行者和独立源码审核者负责，本复审只对已查看的原图和下述对应 JSON 帧记录负责。

## 独立结论

**交互修复与视觉通过必须分开。本轮仍存在已确认的深色侧栏按钮白块过渡帧，不能宣布动画无白块、无闪烁，也不应关闭整个 #15。** 已查看的新代表帧中，没有再看到上轮“按钮移动留下双箭头/旧蓝边”的样式；这仅说明有限抽样未见该旧现象，不证明每帧或每个系统均消失。

### 已确认反例：深色展开时六个浅色矩形

连续原图：

1. `x64-dark-desktop-0104-sidebar-expand-animation-02.png`：侧栏内容仍隐藏，只显示固定位置的箭头按钮。
2. **`x64-dark-desktop-0105-sidebar-expand-animation-03.png`**：右侧顶栏出现 2 个浅色矩形，底部出现 4 个浅色矩形，共 6 个。对应区域的正常文字/图标未画出，矩形在深色背景上明显可见。
3. `x64-dark-desktop-0106-sidebar-expand-animation-04.png`：同一区域恢复正常命令/SFTP 页签、添加命令及底部按钮。

`x64-dark-desktop-user-actions.json` 记录三帧相对时间分别为 15.637444、15.689261、15.747320 秒，toggle 矩形均为 `[1162,173,1190,201]`。因此白块现象与按钮位置稳定可以同时成立。帧间隔不能当作白块实际持续时间；没有连续录像或绘制消息跟踪，不能给出精确持续毫秒数。

中间反例图 SHA-256：`48652707d900d56e835755f1f06363a9d3db849947f0a1827a56bb3e7cacf1a9`（与 JSON 对应）。其前/后帧摘要分别为 `4888505299546b5a8d6e9ca94c7b9524224c3333b2614d68812cae092e05e734`、`1651e87590ae11982497be63a163efbf098be1123a2a5249c1b3901bbf00d1bf`。

建议将此作为 #15 的残留视觉缺陷，优先检查动画结束时隐藏子控件恢复可见、背景擦除与 owner-draw 首次绘制的顺序。**这只是根因候选；本复审没有消息级证据，不能断言某个 API 已被证明为根因。** 后续修复保留 200 ms 动画、现有主题/尺寸和固定 toggle，不应以禁用反向点击或反复全窗重绘掩盖问题。验收须保存该过渡阶段的真实屏幕连续帧，不能仅检查最终布局或 PrintWindow。

### 其他抽样结果和边界

- 浅/深普通折叠、同点反向代表帧：toggle 都只看到一个箭头，侧栏卡片在动画中隐藏；未观察到旧移动箭头残留。是否每次点击都翻转、焦点是否正确应以实际输入/状态 JSON 和源码交叉报告为准，单张截图不能证明这些行为。
- 外框按住缩放和释放后的所选帧：三栏保留各自表面边界，未见整块白底覆盖深色主界面或明显控件互相叠压；终端有软换行/缩放过程中裁切，不能仅据此报告输出字节丢失。
- 内部分隔线代表帧：内容随宽度调整。浅色 `0080-sidebar-splitter-2-held-06` 中“添加命令”右端看起来不完整，释放后 `0083` 已变为纯加号。它提示响应式文字/图标切换边界还可补查，但仅这一帧不足以确认持续截字或高 DPI 缺陷；不将它提升为本轮已确认缺陷。
- SFTP 列宽 8 张 held/released 原图：列表为受控替身的单个 `test.txt`；名称变窄时出现省略号，大小/日期仍在各自区域内。所选帧未见跨列文字叠压或白块。表头蓝色区域可能是当前指针 hover，不能单凭 released 帧仍有高亮认定“旧高亮残留”，需移开鼠标再验证。
- 这 29 张是抽样，不代表对全部 657 张 PNG 的逐图检查，也不能证明视觉上全局无闪烁。未检查高 DPI、Win7、物理串口、电源硬件、真实 SFTP 或长时间运行。

## 实际查看清单

以下为本子代理通过 `view_image` 实际打开的原文件，没有合成、裁剪或重画界面。

| # | 原文件 | 查看目的 |
|---|---|---|
| 1 | [x64-dark-desktop-0104-sidebar-expand-animation-02.png](evidence/desktop/x64-dark-desktop-0104-sidebar-expand-animation-02.png) | 深色白块前帧 |
| 2 | [x64-dark-desktop-0105-sidebar-expand-animation-03.png](evidence/desktop/x64-dark-desktop-0105-sidebar-expand-animation-03.png) | 六个浅色矩形反例 |
| 3 | [x64-dark-desktop-0106-sidebar-expand-animation-04.png](evidence/desktop/x64-dark-desktop-0106-sidebar-expand-animation-04.png) | 深色恢复后帧 |
| 4 | [x64-light-desktop-0096-sidebar-collapse-animation-03.png](evidence/desktop/x64-light-desktop-0096-sidebar-collapse-animation-03.png) | 浅色折叠 |
| 5 | [x64-light-desktop-0100-sidebar-collapse-animation-07.png](evidence/desktop/x64-light-desktop-0100-sidebar-collapse-animation-07.png) | 浅色折叠后段 |
| 6 | [x64-light-desktop-0104-sidebar-expand-animation-02.png](evidence/desktop/x64-light-desktop-0104-sidebar-expand-animation-02.png) | 浅色展开 |
| 7 | [x64-light-desktop-0105-sidebar-expand-animation-03.png](evidence/desktop/x64-light-desktop-0105-sidebar-expand-animation-03.png) | 浅色展开内容恢复 |
| 8 | [x64-dark-desktop-0096-sidebar-collapse-animation-03.png](evidence/desktop/x64-dark-desktop-0096-sidebar-collapse-animation-03.png) | 深色折叠 |
| 9 | [x64-dark-desktop-0100-sidebar-collapse-animation-07.png](evidence/desktop/x64-dark-desktop-0100-sidebar-collapse-animation-07.png) | 深色折叠后段 |
| 10 | [x64-light-desktop-0128-sidebar-same-point-reversal-06.png](evidence/desktop/x64-light-desktop-0128-sidebar-same-point-reversal-06.png) | 浅色同点快速反向 |
| 11 | [x64-dark-desktop-0127-sidebar-same-point-reversal-05.png](evidence/desktop/x64-dark-desktop-0127-sidebar-same-point-reversal-05.png) | 深色同点快速反向 |
| 12 | [x64-light-desktop-0010-resize-round-1-minimum-held-01.png](evidence/desktop/x64-light-desktop-0010-resize-round-1-minimum-held-01.png) | 浅色按住外框缩小 |
| 13 | [x64-light-desktop-0036-resize-round-2-minimum-released.png](evidence/desktop/x64-light-desktop-0036-resize-round-2-minimum-released.png) | 浅色最小窗口释放 |
| 14 | [x64-dark-desktop-0035-resize-round-2-minimum-held-08.png](evidence/desktop/x64-dark-desktop-0035-resize-round-2-minimum-held-08.png) | 深色按住外框缩小 |
| 15 | [x64-dark-desktop-0042-resize-round-2-large-held-06.png](evidence/desktop/x64-dark-desktop-0042-resize-round-2-large-held-06.png) | 深色按住外框放大 |
| 16 | [x64-light-desktop-0080-sidebar-splitter-2-held-06.png](evidence/desktop/x64-light-desktop-0080-sidebar-splitter-2-held-06.png) | 浅色内部分隔线按住 |
| 17 | [x64-dark-desktop-0076-sidebar-splitter-2-held-02.png](evidence/desktop/x64-dark-desktop-0076-sidebar-splitter-2-held-02.png) | 深色内部分隔线按住 |
| 18 | [x64-light-sftp-0003-sftp-column-boundary-1-held-03.png](evidence/desktop/x64-light-sftp-0003-sftp-column-boundary-1-held-03.png) | 浅色名称列按住 |
| 19 | [x64-light-sftp-0009-sftp-column-boundary-1-released.png](evidence/desktop/x64-light-sftp-0009-sftp-column-boundary-1-released.png) | 浅色名称列释放 |
| 20 | [x64-dark-sftp-0003-sftp-column-boundary-1-held-03.png](evidence/desktop/x64-dark-sftp-0003-sftp-column-boundary-1-held-03.png) | 深色名称列按住 |
| 21 | [x64-dark-sftp-0009-sftp-column-boundary-1-released.png](evidence/desktop/x64-dark-sftp-0009-sftp-column-boundary-1-released.png) | 深色名称列释放 |
| 22 | [x64-light-sftp-0043-sftp-size-boundary-2-held-07.png](evidence/desktop/x64-light-sftp-0043-sftp-size-boundary-2-held-07.png) | 浅色大小列按住 |
| 23 | [x64-light-sftp-0045-sftp-size-boundary-2-released.png](evidence/desktop/x64-light-sftp-0045-sftp-size-boundary-2-released.png) | 浅色大小列释放 |
| 24 | [x64-dark-sftp-0043-sftp-size-boundary-2-held-07.png](evidence/desktop/x64-dark-sftp-0043-sftp-size-boundary-2-held-07.png) | 深色大小列按住 |
| 25 | [x64-dark-sftp-0045-sftp-size-boundary-2-released.png](evidence/desktop/x64-dark-sftp-0045-sftp-size-boundary-2-released.png) | 深色大小列释放 |
| 26 | [x64-light-desktop-0083-sidebar-splitter-2-released.png](evidence/desktop/x64-light-desktop-0083-sidebar-splitter-2-released.png) | 浅色内部分隔线释放 |
| 27 | [x64-dark-desktop-0083-sidebar-splitter-2-released.png](evidence/desktop/x64-dark-desktop-0083-sidebar-splitter-2-released.png) | 深色内部分隔线释放 |
| 28 | [x64-light-sftp-0083-sidebar-same-point-reversal-08.png](evidence/desktop/x64-light-sftp-0083-sidebar-same-point-reversal-08.png) | 浅色 SFTP 同点快速反向 |
| 29 | [x64-dark-sftp-0083-sidebar-same-point-reversal-08.png](evidence/desktop/x64-dark-sftp-0083-sidebar-same-point-reversal-08.png) | 深色 SFTP 同点快速反向 |
