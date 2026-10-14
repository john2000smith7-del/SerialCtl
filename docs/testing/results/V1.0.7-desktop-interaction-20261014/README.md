# V1.0.7 真实桌面操作审核：点击、拖动、缩放

2026-10-14。**执行完成，审核不通过；不是程序整体不可用。** 两种主题的窗口拖动、三轮缩小/放大、最大化/恢复、侧栏宽度、SFTP 列宽和 35 个 CMD 的断开清理得到实际验证；侧栏动画重影、移动按钮难以反向点击、连接列表连续键盘导航失焦仍存在。本轮只改测试和证据，没有修复生产代码，也没有关闭既有缺陷。

本报告补充此前[四位审核者的综合审核](../../../reviews/2026-10-11-v1.0.6/)，不重新宣称所有协议、硬件或安全项目通过。当前优先处理侧栏按钮和列表焦点，原有 [#2–#16、优化路线 #17–#25](https://github.com/john2000smith7-del/SerialCtl/issues/26) 仍需逐项处理。

## 来源和执行环境

| 项目 | 可核对的事实 |
|---|---|
| 分支 | `main`；远端仅保留 main。工作区为 `/workspace/serialctl-main`，没有覆盖其他工作目录 |
| 运行程序源码 | `0454b51685d4aa6ec1a3dd316f2427b916cfd777` |
| 与交付提交关系 | 该 main 合并了包含 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0` 的修复分支及自动更新；不是对原交付 SHA 原样复测 |
| VERSION | `PROJECT_VERSION=V1.0.7`、`CPP_VERSION=V1.0.7` |
| 完整窗口测试源码 | `317bec2ebe52df114058dacf81801157c10e1c79`（attempt 7）；后续真实 CMD 系统键盘测试见补测记录 |
| 程序来源 | 成功的 [Windows 构建 38392007467](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38392007467) 的 `SerialCtl-Win7-portable` artifact |
| 编译配置 | 仓库 CMake / MSVC、C++17、x64 Release、/MT、PE 6.01、YY-Thunks；本次复用已构建包，没有重新推断编译器补丁号 |
| 操作平台 | GitHub `windows-2022`，Windows Server 2022 10.0.20348，Python 3.12.10、PowerShell 5.1；不是 Win7 |
| 完整 attempt 7 桌面 | AMD EPYC 9V74，4 个逻辑处理器，17,174,360,064 bytes 内存，1920×1080、96 DPI；不同 runner CPU 以各轮环境文件为准 |
| 包 SHA-256 | `03efacb0d9f28b6fee5c9a01b76887b550f99844d80d05af69ef3ee5ab22776d` |
| serialctl.exe SHA-256 | `20f457f28a5d6f6b107931c384873ffd6982dc59b8b21189fade29c4b22d6404` |
| plink.exe SHA-256 | `fb15bf12b038abb6174cb073f710eac55a68a1479769f81c0ee02d03a0c5c246` |
| psftp.exe SHA-256 | `065a5ea3086e54fb65ba9f2e05f4b72d984fc82dda978dbd1017c9b45ee4feb8` |

每次运行先比较程序源码、依赖、VERSION 和打包输入，再下载一致的便携包；原文件摘要在替换 SSH/SFTP 子进程前采集。参见 [receipt](evidence/attempt-7/interaction-receipt.json)、[环境](evidence/attempt-7/ui-environment.json)、[EXE 摘要](evidence/attempt-7/program-executable-sha256.json)。后续测试提交没有改变 `src/`、VERSION、CMake 或依赖。

## 实际操作及边界

真实操作使用 `SetCursorPos + mouse_event` 或 `SendInput`，先检查 HWND、所属进程、可见/启用状态与屏幕命中。窗口标题拖动、边框缩放、双击最大化、内部分隔线和列宽均由系统鼠标输入完成。外框测试临时开启“拖动时显示窗口内容”，退出恢复原设置，JSON 中 `restored: false` 指恢复为原来的关闭值，并非恢复失败。

新脚本截图来自可见桌面的 BitBlt，旧脚本的动态截图使用 CopyFromScreen；不使用网页仿真。原有 PrintWindow 仍用于静态布局，不能据其证明无残影。完整 attempt 7 保存 **577 张 PNG、168 条主按钮点击记录**，其中新连续拖动脚本有 366 张帧和 54 项操作记录（含 4 项失败、4 项后续恢复）。原始截图不作修图。

脚本仍有明确的测试准备：字段填充值、初始列表顶部和通知刷新使用窗口消息；旧快速回归保留 MoveWindow/WM_COMMAND；关闭触发为 WM_CLOSE。它们不冒充真实鼠标/键盘。本轮的 CMD 业务真实运行；SFTP 只把隔离副本的 SSH/SFTP 子进程换成固定文件列表及持续输出替身，实际 Win32 控件不替换。此结果不证明真实 SFTP 协议、上传下载或网络中断恢复。

## 逐项结果

“通过”只表示本行描述的场景和断言，不表示无闪烁或现场性能合格。

| 检查 | 结论 | 证据 / 限制 |
|---|---|---|
| 浅/深主题入口点击、连接对话框取消、共享端口右键菜单 | 通过 | [actual-clicks](evidence/attempt-7/actual-clicks.jsonl)；真实 COM/IP 连接未运行 |
| 两主题实际标题拖动、980×620 ↔ 1180×760 各三轮、最大化/恢复 | 通过 | [light](evidence/attempt-7/x64-light-desktop-user-actions.json)、[dark](evidence/attempt-7/x64-dark-desktop-user-actions.json)；检查真实外框位置和子控件边界 |
| 每主题侧栏宽度拖动三次，持续 CMD stdout | 通过 | 同上；不同 stdout 标记在实际按住拖动时进入终端绘制，不只检查重绘次数 |
| 普通收起/展开后的稳定布局 | 通过 | 内容可见性、最小终端宽度、卡片不重叠断言；不包含动画视觉结论 |
| 侧栏动画视觉 | **失败 / 已确认** | 浅/深主题中间帧有双箭头和旧蓝边，见 U1 |
| 快速反向点击 | **失败 / 已确认操作结果** | 四组目标在按下/释放采样间移出指针，未改变目标状态，见 U2；不宣称收到 BN_CLICKED 后丢事件 |
| 两主题 SFTP 名称列三次、大小列两次实际拖动 | 通过（受控替身） | [light](evidence/attempt-7/x64-light-sftp-user-actions.json)、[dark](evidence/attempt-7/x64-dark-sftp-user-actions.json)；实际列宽变化及相邻控件布局 |
| SFTP 字段在列宽变化中显示 | 部分通过 | 独立复审的 8 张 held/released 帧未见跨列，名称窄时省略；替身只有 test.txt，不覆盖大目录 |
| 电源分页快速切换、序列编辑窗口 | 部分通过 | 实际按钮点击及中间屏幕帧；无设备，未验证监测刷新时编辑、总输出或任务取消 |
| 中文标签、加号图标/带文字按钮 | 部分通过 | 96 DPI 字体边界断言及代表帧目视检查；125/150/200% 未运行 |
| 35 个 CMD、滚轮、实际滑块拖动、刷新锚点 | 通过（短时） | [lifecycle](evidence/attempt-7/gui-lifecycle.json)；初始 top=12 为显式准备，20 次通知刷新保留顶部及选择 |
| 连接列表连续 Home→End | **失败 / 已确认** | Home 后 focus=107（终端），End 后 selection 仍 0，预期 35；恢复焦点后 End 可选 35，见 U3 |
| “全部断开”停止 CMD 和运行中子进程 | 通过（本次负载） | 35 个真实 CMD，资源/列表清空、已知子进程全部退出，0.406 s；不等于物理串口/SFTP/电源任务已经测试 |
| 命令编辑取消/保存、遮挡程序与 owner | 部分通过 | 实际保存/取消按钮、文件摘要和前台 owner 断言；前台条件由脚本建立，不覆盖所有前后台抢焦点路径 |
| 正常进程退出 | 通过（本次路径） | WM_CLOSE 后 10 s 内正常退出且 exit code 0；不是鼠标点标题 X 的专项 |
| 真实系统键盘 CMD 逐键编辑 | 通过（ASCII，见补测记录） | 旧 gateway 的中文、粘贴和历史用窗口消息；不能把它当真实键盘/IME 通过 |
| Win7、其他 DPI、实体串口、IT6332A、真实 SFTP / 内网、8 小时 | **未运行** | 按[人工验收表](MANUAL-ACCEPTANCE.md)补测 |

左侧连接框随主窗口自动布局，当前没有独立宽度拖动入口；不能把所有矩形区域都当作可拖动控件。传输抽屉展开/收起也不等于可任意调整高度。

## 问题及如何修改

### U1 / P2：侧栏动画存在双箭头和旧边缘（已确认，关联 #15）

触发：CMD 持续输出，点侧栏折叠/展开；浅色和深色均复现。预期每一帧只显示一个按钮；实际中间帧保留旧位置箭头/蓝边，稳定结束帧恢复。

- 原始证据：[浅色折叠](evidence/attempt-4/x64-light-desktop-0094-sidebar-collapse-animation-01.png)、[浅色展开](evidence/attempt-4/x64-light-desktop-0102-sidebar-expand-animation-00.png)、[深色展开](evidence/attempt-7/x64-dark-desktop-0102-sidebar-expand-animation-00.png)。后续开启实时外框内容后仍复现，不能归因于系统拖动轮廓。
- 定位：`src/app/MainWindow.cpp:1959` 动画布局/失效，`:2428–2431` 移动 toggle，`:2694–2732` 当前按钮背景/边缘绘制；`src/app/UiStyle.h:237–275` 批量几何更新。**绘制先后是根因候选，尚无修复对照实验。**
- 建议：优先让 toggle 在动画期间保持稳定锚点，减少可见子窗口搬移；若仍移动，确认旧/新父背景脏区并协调子窗口绘制。保留现有尺寸、主题和 200 ms 动画，不引入全局反复重绘。
- 验收：浅/深、普通与反向动画、持续输出，各阶段记录真实屏幕；旧/新按钮区域不得残留箭头，不能只用 PrintWindow 或最终帧关闭问题。

### U2 / P2：移动按钮导致快速反向点击落空（结果已确认；消息级根因仍有边界）

复现：展开态点击收起，约 90–100 ms 后对准此刻按钮再点一次。attempt 7 的命令页/SFTP × 两主题均未翻转第二次目标状态。

| 场景 | 第二次按下前按钮 x | 释放前按钮 x | 固定指针 x |
|---|---|---|---|
| 浅色命令 | 1098–1126 | 1129–1157 | 1120 |
| 深色命令 | 1087–1115 | 1127–1155 | 1109 |
| 浅色 SFTP | 1386–1414 | 1456–1484 | 1408 |
| 深色 SFTP | 1386–1414 | 1438–1466 | 1408 |

四组 `hit_before_down` 为 toggle，`hit_before_up` 为主窗口。实际观察是目标在输入期间移走；鼠标捕获仍可能把消息送到按钮，因此此记录不证明 WM_LBUTTONUP 的实际收件方，更不证明 BN_CLICKED 被业务丢弃。两次点位相隔 41–83 px，大于系统双击宽度 4 px，降低了早轮同点双击合并的歧义。

定位 `MainWindow.cpp:2428–2431`：稳定展开态在右侧，动画中改为变化宽度的中心位置；`:4914–4928` 立即改变目标 caption。建议与 U1 一并固定按钮锚点，保留快速反向能力；不要以动画期间禁用按钮掩盖问题。回归增加 8/30/100 ms 按压和不同动画相位，必要时单独诊断记录实际鼠标/捕获/BN_CLICKED 消息。

### U3 / P2：连接列表键盘选择后焦点被转到终端（已确认，#30）

步骤：创建超过一屏的连接；实际点击滚动条使列表获得焦点；按 Home，再直接按 End。实际 Home 将 selection 置 0 后 focus 变成终端 107；End 无法选到末行 35。单独用鼠标恢复列表焦点后 End 成功。原失败在 JSON 中保留，恢复不抵消失败。

定位 `src/app/MainWindow.cpp:2085–2090` 的 LBN_SELCHANGE 调用 `SwitchSession`，后者 `:3696` 无条件 `SetFocus(terminal_)`。鼠标点击行后进入终端可以是预期行为，但键盘导航因此中断。源码还显示普通终端可将 Home/End 转成控制序列，**本轮没有验证真实设备收到这些字节**。

建议让会话切换显式接收焦点策略：键盘列表选择保持列表焦点，明确激活/连接才转到终端；不要全局删掉 focus。验收用真实 Home/End、上下、PageUp/Down 连续输入，无中途 SetFocus/鼠标修复，覆盖电源伪行和 CMD 草稿；验证导航不会误入设备输入。

### 未重现不等于修复

已有 [#29 深色“全部断开”浅色块](https://github.com/john2000smith7-del/SerialCtl/issues/29) 在本轮[断开后的截图](evidence/attempt-7/x64-all-disconnected.png)未出现。生产代码没有改动，仍保持问题开放，并按原触发路径复测。有限代表帧没有白块，不能宣布全程序“无闪烁”。

## 独立复核和分歧取舍

[视觉复审](independent-visual-review.md)实际查看 66 张原图；[源码交叉复审](source-cross-review.md)核对精确调用链与输入时间。主执行者复读 JSON 并查看关键原图后统一结论：

- attempt 4 的外框黑/橙轮廓为系统设置，不能作为应用残影；attempt 5/7 开启全内容拖动补齐该项。
- 终端 `re` / `al` 分行经局部检查是软换行，不报告数据损坏。
- 早轮“10 次点击最终应展开”受双击合并影响，不认定业务丢动作。新版记录确认按钮移出固定指针，按 U2 的有限结论报告。
- 断言几何通过与视觉重影失败并存，不能以自动测试 PASS 覆盖原始帧。
- 生命周期真实 Home→End 的失败不能被旧 SendMessage 直接发给 list HWND 的通过结果替代。

## 统计和证据解释

attempt 7 的单 loopback 客户端、单 CMD 每主题 100 个 marker：浅色显示 P50/P95/P99 = **4.858 / 5.488 / 5.594 ms**，深色 **3.257 / 3.952 / 4.181 ms**。起点为包含该标记的合并 stdout 批次第一次 PostData QPC，终点为终端首次 BitBlt 画出标记；不含更早批次、串口传输和显示器实际呈现时间。见 [light](evidence/attempt-7/x64-light-display.json)、[dark](evidence/attempt-7/x64-dark-display.json)。这些短时采样不是性能门槛承诺。

35 个 CMD 是已测短时会话数，不是 35 个网络客户端，也不是容量上限。CPU/内存/线程/句柄的压力时序、本轮网络转发延迟、慢磁盘/慢客户端/断网和 8 小时稳定性未测；此前报告若有不同测试，应按各自范围阅读。

完整主轮 [run 38514898081](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38514898081) / [下载 artifact](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38514898081/artifacts/11871580776)：491,772,996 bytes，artifact ZIP SHA-256 `58e0fd1c678c958ab841361158ce988880ecb503e2b04f1f0274c0f51283628c`。工作流在所有剩余用例完成后以 5 个已保留失败退出，并非中途阻塞。仓库保留精选原图、全部本轮 JSON/点击记录和[原始日志尾部](run-38514898081-failure-tail.log)；完整原图在 artifact 中，保存期 30 天，需及时下载。本目录 SHA256SUMS 校验精选文件；完整目录索引见 artifacts.json。

复跑方式和输入方法见 [DESKTOP-INTERACTION.md](../../DESKTOP-INTERACTION.md)。本次未发布新程序或 release，所有工作在 main 上，生产文件保持原样。


## 最终补测：真实系统键盘与完整重复执行

最终测试提交为 `40afd80b6c85a5c17bdd364df59e10a2cfb892bb`，分支 main，生产源码和程序摘要仍与上表一致。[run 38555876437](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38555876437) 完成全部检查并保留同样 5 项行为失败；新增的真实 CMD 键盘检查在两主题均通过。本报告最后的纯文档提交不改变程序或测试。

- [浅色键盘 JSON](evidence/attempt-9/x64-light-cmd-real-input.json)、[深色键盘 JSON](evidence/attempt-9/x64-dark-cmd-real-input.json)，每主题 455 条鼠标/键盘输入记录，含逐字符 SendInput 及 Home/Left/Right/Delete/End/Backspace/Enter。三份实际文件的完整字节与 SHA-256 分别验证修正后的命令、相对目录写入及 set 状态；只读订阅的 device stdout 验证对应内容，input 事件不作为执行成功依据。
- [浅色执行后原图](evidence/attempt-9/x64-light-cmd-real-input-after.png)、[深色执行后原图](evidence/attempt-9/x64-dark-cmd-real-input-after.png)。本测试字符为 ASCII，通过 pushd 改变目录；不证明中文 IME、多行真实剪贴板、所有 cd 语法或全屏应用。
- attempt 8 已生成正确 edited.txt 和独立 stdout，但测试最初要求输出标记前必须有换行；CMD prompt 不带换行导致误判。本次按实际协议匹配完整 marker+CRLF，仍排除 input 事件并独立核对文件。原失败和输出保留在 [attempt 8 JSON](evidence/attempt-8/x64-light-cmd-real-input.json)，不是生产代码修复。独立复核见 [键盘复审](keyboard-review.md)。
- 完整重复执行仍覆盖两主题外框/内栏/列宽、列表和清理；35 个 CMD 的清理为 0.344 s，已知子进程全部退出。Home→End 再次失败；侧栏四组快速点击再次失败。详见 [最终 lifecycle](evidence/attempt-9/gui-lifecycle.json)及[最终日志](run-38555876437-failure-tail.log)。
- 最终 runner 是 AMD EPYC 7763、4逻辑处理器，其他桌面规格仍是 1920×1080、96 DPI；见[最终环境](evidence/attempt-9/ui-environment.json)。每轮环境单独记录，未混用 CPU 型号。
- 最终 artifact 有 601 个文件、584 张 PNG，ZIP 498,376,391 bytes，SHA-256 `992b71fe4a9461b36ca8bbd053249d1351a4201227c09c4c25896649af17bb43`。[下载全部原始证据](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38555876437/artifacts/11890477053)。有限帧及原始样本均保留；没有生成伪造录屏或 UI 图。

每轮运行的 SHA、ZIP 摘要、结果和下载地址在 [artifacts.json](artifacts.json)。attempt 1/2/3/6 是测试脚本定位/等待/变量错误，attempt 4 的轮廓拖动不足以证明实时内容重绘，attempt 5 继续定位移动目标，attempt 7 完整执行，attempt 8 为输出断言误判，attempt 9 完整复测。失败历史没有删除，也没有把它们全部算作软件缺陷。

处理顺序：先按 [#15](https://github.com/john2000smith7-del/SerialCtl/issues/15) 固定动画 toggle 锚点、核对残影；按 [#30](https://github.com/john2000smith7-del/SerialCtl/issues/30) 区分键盘导航与明确激活的焦点策略。两项改动局部、用户收益直接，仍需两主题/真实输入回归。随后做高 DPI、Win7、真实设备和 8 小时验收；不能以本次短时桌面结果替代。更新器与之前审核问题不在本次修复范围。
