# V1.0.7 桌面局部修复与真实操作回归

2026-10-15。**快速反向点击落空和连接列表键盘失焦已修复，并通过本轮 Windows 实际输入回归；视觉审核仍部分通过。** 固定侧栏按钮后，所看代表帧未再出现旧双箭头，但深色侧栏展开时捕获到六个浅色按钮矩形，已登记 [#31](https://github.com/john2000smith7-del/SerialCtl/issues/31)。不能以 Actions 全绿宣称“界面无闪烁”。

本轮承接[此前实际桌面审核](https://github.com/john2000smith7-del/SerialCtl/blob/main/docs/testing/results/V1.0.7-desktop-interaction-20261014/README.md)，只修改侧栏按钮、列表焦点和电源视图切换。此前[四位独立审核与优化路线](https://github.com/john2000smith7-del/SerialCtl/issues/26)仍需逐项处理；本报告不把所有协议、安全、硬件和现场问题改判通过。

## 精确版本、来源和环境

| 项目 | 本轮记录 |
|---|---|
| 分支与工作区 | `main`，远端仅 main；独立目录 `/workspace/serialctl-main`，未覆盖其他工作区 |
| 生产修复 SHA | `906275761ea9729a87e2090b9458ebbd96ba0b19` |
| 父提交 | `676f50ff8da20f7232fe8830f3c1173858032722`；生产基线来自已统一的 main，包含原交付 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0` 及自动更新，并非原交付源码原样复测 |
| 实际桌面测试 SHA | `ea9d0f351505ae93fa8a0d2d4d49cf29c8209d07`；与生产修复 SHA 的生产/依赖/打包输入一致，工作流先验证后运行 |
| VERSION | `PROJECT_VERSION=V1.0.7`、`CPP_VERSION=V1.0.7`；没有发布新版本号、tag 或 GitHub Release |
| 程序来源 | [完整构建 38683760398](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38683760398) 的 [SerialCtl-Win7-portable](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38683760398/artifacts/11949407164) |
| 工具链与检查 | CMake / MSVC 19.44.35229.0，VS 工具路径 14.44.35207，C++17、x64 Release、/MT、PE 子系统 6.01、YY-Thunks；固定 PuTTY/许可证及 NI-VISA 18 输入均通过原检查 |
| 实际运行 | [桌面 run 38751634512](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38751634512)，GitHub `windows-2022` / Windows Server 2022 10.0.20348，Python 3.12.10、PowerShell 5.1 |
| 实际桌面硬件 | AMD EPYC 7763，4 逻辑处理器，17,178,693,632 bytes 内存，1920×1080，96 DPI；构建机 CPU 另见 build 环境文件 |
| 便携包 SHA-256 | `721636c25a44fcfaec6a1471f59cde71dbe7d48bdb9b600b0f573ab774d40f4c` |
| serialctl.exe SHA-256 | `32a0801cbe6ad767519cb0e72dde81bbcf1069ae1bfc76a748d1c289961c8889` |
| plink.exe SHA-256 | `fb15bf12b038abb6174cb073f710eac55a68a1479769f81c0ee02d03a0c5c246` |
| psftp.exe SHA-256 | `065a5ea3086e54fb65ba9f2e05f4b72d984fc82dda978dbd1017c9b45ee4feb8` |

来源依据：[receipt](evidence/desktop/interaction-receipt.json)、[实际环境](evidence/desktop/ui-environment.json)、[EXE 摘要](evidence/desktop/program-executable-sha256.json)、[包摘要](evidence/desktop/program-package-sha256.json)、[完整构建日志](build-38683760398.log)。运行新编译的程序，不是复用修复前的 EXE。

## 改了什么及为何这样改

源码行号固定在上述生产 SHA；[完整 diff](https://github.com/john2000smith7-del/SerialCtl/commit/906275761ea9729a87e2090b9458ebbd96ba0b19)。

| 问题 | 实现与用户收益 | 验证结论 |
|---|---|---|
| 动画中按钮移动，第二次按下/释放落空；同点双击通知不等于两次激活 | `MainWindow.cpp:2442` 将 toggle 固定在最终右侧轨道；`:1390` 仅对该按钮把原生 DBLCLK 交给正常 DOWN/UP 路径，继续保留释放/取消语义；`:1971/:4957` 清理停止动画后的 timer。连续点击无需追随移动按钮。 | 四场景 80/80 次释放均翻转目标状态，最终展开，按下/释放前后边界不变；有限原图未见旧双箭头。#15 的更广视觉验收仍待完成。 |
| 列表 Home 后焦点被送入终端，End 无法继续选择 | `MainWindow.cpp:1687` 仅在原生列表处理键盘消息期间标记来源；`:2102/:3723` 传显式焦点策略。键盘选行保留列表焦点，鼠标与连接完成仍进入终端。 | 两主题连续导航及键盘后的鼠标激活通过，见 #30。 |
| 选择电源伪行后排队的切页可能滞后于后续选择 | `MainWindow.cpp:3672` 提取原工具栏逻辑为 `ShowPowerPage`，列表与工具栏同步共用。没有新增设备操作、AI 权限或强制前台行为。 | 两主题跨普通会话/电源行，选择、焦点、终端及真实电源 HWND 可见性匹配。滞后风险是源码判断，不称为此前实测缺陷。 |

颜色、现有控件尺寸、200 ms 动画及动画终点更新终端尺寸的约束保留；UI 指南补充固定锚点与输入焦点规则。没有新增运行库依赖。

## 实际检查结果

“通过”限于本行所述场景。系统输入为 `mouse_event / SendInput`，读取 HWND/几何/焦点并校验 PID；新连续操作截图为屏幕 BitBlt，旧动态检查为 CopyFromScreen。静态布局仍有单独 PrintWindow，不能据其证明无残影。没有使用网页仿真、重画界面或伪造图。

| 检查 | 结果 | 原始证据与限制 |
|---|---|---|
| 完整构建、16 项原生/集成测试、PE/导入/包复开检查 | 通过 | [CTest 原始日志](evidence/build/ctest-x64.txt)、[构建日志](build-38683760398.log)；不代表 Win7 实机通过 |
| 更新器启动失败路径、Python 客户端及签名 feed 回归 | 通过（各自范围） | [updater GUI](evidence/build/updater-gui.json) 与构建日志；生产签名成功升级、真实内网地址未运行 |
| 浅/深标题拖动、980×620 ↔ 1180×760 三轮、最大化/恢复 | 通过 | [light desktop](evidence/desktop/x64-light-desktop-user-actions.json)、[dark desktop](evidence/desktop/x64-dark-desktop-user-actions.json)；每主题 17 项操作、134 帧 |
| 持续实际 CMD 输出时侧栏分隔线三次往返 | 通过 | 同上；每主题 10 段拖动期间出现新的实际绘制 stdout 标记，不只计 WM_PAINT |
| 普通展开/折叠、交替点位快速反向、同点快速反向 | 通过（操作/几何） | 命令/SFTP × 两主题，共 80 次快速点击，每次 caption 翻转，最终展开；72 个相邻间隔均为 76.344–120.332 ms，均短于 200 ms；160 张侧栏阶段截图记录固定按钮矩形 |
| 侧栏动画视觉 | **部分通过；存在新确认白块** | 所看帧旧双箭头未见，但深色展开原图 0105 出现六个浅色矩形，见 U4/#31；不宣称全程序无闪烁 |
| SFTP 名称列三次、大小列两次实际拖动 | 通过（受控替身） | [light SFTP](evidence/desktop/x64-light-sftp-user-actions.json)、[dark SFTP](evidence/desktop/x64-dark-sftp-user-actions.json)；每主题 10 项操作、87 帧。替换的是隔离副本的 SSH/SFTP 子进程，控件为生产程序；不证明真实服务/文件传输 |
| 35 个 CMD 的滚轮、滑块、通知刷新锚点 | 通过（短时） | [lifecycle](evidence/desktop/gui-lifecycle.json)；初始 top=12 与通知刷新明确属于测试准备，不冒充真实用户输入 |
| 两主题连续 Home/End、上下、PageUp/Down、经过电源行 | 通过 | 22 次导航均保留列表 HWND 106 焦点，选择与 terminal/power HWND 可见性匹配；同一键序列中无焦点修复；键盘后鼠标选另一行均转入终端 107 |
| 两主题真实 CMD 逐键编辑与实际执行 | 通过（ASCII） | [light](evidence/desktop/x64-light-cmd-real-input.json)、[dark](evidence/desktop/x64-dark-cmd-real-input.json)：系统逐键输入、编辑键、文件字节、device stdout、pushd/set 状态；不等于中文 IME/真实多行剪贴板/全屏程序通过 |
| “全部断开”与正常退出 | 通过（本次 CMD 路径） | 35 个真实 CMD，清理 0.391 s，已知子进程均退出且列表/资源清空；关窗 WM_CLOSE 后正常退出。串口/SFTP/电源任务硬件清理未测 |
| 电源页切换、命令编辑保存/取消、owner 与遮挡窗口 | 部分通过 | [点击记录](evidence/desktop/actual-clicks.jsonl)、[完整 transcript](evidence/desktop/desktop-interaction-transcript.txt)；真实按钮操作及原有 owner 断言通过，不覆盖硬件监测刷新或全部抢焦点路径 |
| 125/150/200% DPI、Win7、实体串口/IT6332A、真实 SFTP/内网、8 小时 | **未运行** | [可执行人工验收与证据方法](MANUAL-ACCEPTANCE.md) |

左侧连接列表随窗口布局，没有独立宽度拖拽入口；传输抽屉只有展开/收起，不把这些设计边界报告为拖动故障。性能统计沿用原 JSON 的具体测点；本次没有测 CPU/内存/线程/句柄的长期时序、32 网络客户端负载或网络转发延迟，不作性能达标保证。

## U4 / P2：深色展开时按钮出现浅色矩形（已确认，#31）

连续 CMD 输出，实际点击展开右侧栏。预期按钮首次可见时即显示完整主题内容；实际在一张真实屏幕中出现顶栏两块、底栏四块浅色矩形，文字/图标缺失，下一采样帧恢复。toggle 本身仍单一、位置固定，没有本次点击失败或数据丢失证据。

- [前帧 0104](evidence/desktop/x64-dark-desktop-0104-sidebar-expand-animation-02.png)：控件尚隐藏。
- [反例 0105](evidence/desktop/x64-dark-desktop-0105-sidebar-expand-animation-03.png)：六个浅色矩形；SHA-256 `48652707d900d56e835755f1f06363a9d3db849947f0a1827a56bb3e7cacf1a9`。
- [后帧 0106](evidence/desktop/x64-dark-desktop-0106-sidebar-expand-animation-04.png)：控件正常。相对采样时间分别为 15.637444、15.689261、15.747320 s，不等于已测得故障精确持续时间/发生率。

调查位置：`MainWindow.cpp:1971` 动画终点、`:2512`/`:4963` 子控件重新显示、`UiStyle.h:237` 批量几何/可见性提交、`MainWindow.cpp:1385` 原生按钮路径及 `:2702` owner draw。**默认背景擦除与主题内容绘制先后是候选根因，尚未通过消息级诊断确认；不能归因于本次修改而没有对照证据。**

建议先统一记录 SHOWWINDOW/ERASEBKGND/PAINT/DRAWITEM 与捕获的 QPC，定位首次显示的擦除/绘制顺序；再统一主题背景、批量显示和必要的局部绘制。不要只把白块涂黑，也不要全窗口反复刷新或关闭动画。回归要涵盖两主题、命令/SFTP、持续输出、至少100次反向及连续原始录像，保留快速点击能力。完整修改和验收条件见 [#31](https://github.com/john2000smith7-del/SerialCtl/issues/31)。

## 独立复审与取舍

[独立源码与原始 JSON 复核](cross-source-review.md)核对生产调用链、80 次点击、22 次导航、442 张引用截图的 SHA-256 和程序来源；[视觉复审](independent-visual-review.md)逐张查看原图，并与旧反例比较。主执行者复读原始记录并再次查看 0104/0105/0106 后采纳白块发现：**自动操作 PASS 与视觉缺陷可以同时成立。**

测试复审还发现早版测试只验证终端隐藏，不能证明电源页显示；也未覆盖键盘之后再用鼠标。最终测试 SHA 已补上真实 power HWND/PID、双视图可见性及两主题键盘后鼠标断言，并实际通过。没有把跳过项或失败后的恢复当作通过。

处理状态：#30 按本轮 Win2022/96 DPI 的明确复现范围结案；#15 保留固定锚点的已验证进展，完整视觉验收仍开放；#31 记录新白块；#29 原有“全部断开”浅块未在本轮修复，不因一张最终截图正常而关闭。尚未测试的按下拖出取消、Space 激活、首次16 ms timer tick前反向及真实设备导航输入见人工表。

## 证据目录、下载与复跑

完整桌面 artifact：**674 文件、657 张 PNG、166 条主按钮点击记录**；其中新连续操作脚本 54 项操作、442 帧，均无操作断言失败。仓库保存全部本轮 JSON/文本记录与精选原图，原始大包保留全部截图。没有修改原图，也没有生成录像替代缺失的连续录屏。

- [下载全部原始桌面证据](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38751634512/artifacts/11980499567)：567,301,575 bytes；SHA-256 `84bf3b016918e86516bb2f48456cfe39cbc094c1ea6952f05b906ec604b9f68b`。
- [构建基础证据](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38683760398/artifacts/11949616995)：140,358,285 bytes；SHA-256 `28e6fc59411e65806eae326a4ce4806d7e9c64d2aa01d71d7f7af14224c26d3f`。
- 下载工具有 512 MiB 上限，故使用[仅分卷导出 run 38752671773](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38752671773)。两卷拼接后本地重新计算原 ZIP SHA、长度及 ZIP CRC，全部一致。导出没有重新运行测试或改变内容；[分卷收据](split-receipt.json)、[导出日志](export-38752671773.log)、[独立核验](cross-json-check.json)。
- 所有 Actions artifact 保存期30天，请及时下载。[SHA256SUMS](SHA256SUMS)校验本目录文件；`.gitattributes` 保留证据原始换行字节。索引见 [artifacts.json](artifacts.json)。

复跑说明：[DESKTOP-INTERACTION.md](https://github.com/john2000smith7-del/SerialCtl/blob/main/docs/testing/DESKTOP-INTERACTION.md)。手动启动 Desktop user interaction audit，选择构建 `38683760398`；若生产代码改变必须选择匹配的新完整构建。脚本仍包含已明确标记的字段填充、通知刷新和 WM_CLOSE 准备/检查，它们不冒充真实鼠标动作。

优化顺序：立即处理 #31 的局部显示/擦除，保留当前已验证的点击与焦点行为；近期补 #15 的高频视觉验收、Win7及高 DPI，并按 #9 继续键盘可达性；随后按既有任务清单执行真实设备、内网、慢客户端和8小时现场验证。依赖及代价主要是诊断采样与现场环境，不建议引入新 UI 框架或全面改版。
