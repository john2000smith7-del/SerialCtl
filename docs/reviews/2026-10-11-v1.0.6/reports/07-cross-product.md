# 产品交叉复审：以用户任务收窄结论

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

审查源码：`5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，V1.0.6。只读审查 `source/AGENTS.md`、README、架构/UI 指南、testing 文档及生产源码；随后交叉核对独立报告 03、04、01。本报告未修改源码、运行设备或改变远端状态。

## 本轮新增证据及裁定

已直接读取本轮 Windows run `38097145874` 的下载后原始证据，原独立报告的“截图下载受阻”是写作时边界，不能继续表示目前仍无原图。本角色查看了 `x64-light-macro.png` 和 `x64-dark-api-cmd-visible.png`：前者为两步宏编辑框，后者有真实 CMD 连续输出及“不支持全屏终端查询”的提示；这两张图没有宏执行结束、电源掉电或高 DPI 证据，也不证明动态无闪烁。

- [环境 JSON](../evidence/windows-fresh/ui/serialctl-ui/ui-environment.json)：Windows NT 10.0.20348、96 DPI、1920×1080、4 逻辑处理器，明确无实体 UART/IT6332A，SFTP 使用隔离替身。
- [浅色 CMD JSON](../evidence/windows-fresh/ui/serialctl-ui/x64-light-display.json)、[深色 CMD JSON](../evidence/windows-fresh/ui/serialctl-ui/x64-dark-display.json)：各 100 样本；P50/P95/P99 分别 5.597/6.686/8.451 ms、5.283/6.154/6.619 ms。测点是完成唯一 stdout 标记的合并批次首个 PostData 回调至首次实际 BitBlt；不是命令全程耗时、物理显示器时间、LAN 或 Win7 性能。checks 包含逐键编辑、GBK/UTF8 原始 stdout、历史重发、普通多行粘贴尾稿、异步输出保留草稿、历史阅读锚点、主动输入回实时。
- [全部断开 JSON](../evidence/windows-fresh/ui/serialctl-ui/gui-lifecycle.json)：35 个真实 CMD 会话，顶部锚点 12/选择 34，断开 0.375 秒，`children_after={}`、`known_children_all_exited=true`；保存了断开前 PID/创建时间，包含 helper、cmd、conhost 及 powershell 子命令。证明该次 CMD 树确已收尾；不能以空列表单独替代这一证据，也不能外推串口/SFTP/电源。
- [管道编码探针](../evidence/windows-fresh/ui/serialctl-ui/cmd-pipe-encoding.json) 的 CP936/65001 `matches=false` 属 [tests/integration/cmd_pipe_encoding_probe.py](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/cmd_pipe_encoding_probe.py) 对直接管道输入的比较探针；生产桥接的两主题 Chinese raw stdout 断言另有通过。反对把此 JSON 当成“生产 CMD 中文确定损坏”的证据。

## 同意、收窄与反对

| 交叉对象 | 裁定 | 证据与用户影响 | 验收要求 |
|---|---|---|---|
| P-01 宏完成/停止语义 | 同意，P2 源码确定；真实慢任务 GUI 待运行 | [MainWindow.cpp:3777–3826](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3777-L3826) 以 SendBytesToActive 成功推进并显示“执行完成”，[SessionService.cpp:108–113](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L108-L113) 仅 `queued/execution=unknown`。现有两步编辑截图不能证明宏真正完成 | 慢 CMD 两步宏；完成输出前只写“已提交”；Stop 明确只停后续，已提交任务继续的事实可见 |
| P-02 超长 CMD 尾稿 | 同意，保留精确边界，P2 | [MainWindow.cpp:4249–4262](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L4249-L4262) 先发前缀再 Insert；[生产分支探针](../evidence/product/paste-tail-probe.log) 接受 1 批次，14 字符草稿归零，8192 字符尾行失败。fresh 普通多行粘贴通过不覆盖此长度边界 | 实际 GUI 粘贴，8190/8191/8192、CR/LF/CRLF、光标中部；拒绝前 TX=0、草稿与光标保持，不自动重发 |
| P-03 全部断开电源失败反馈 | 同意反馈缺口，P1 候选；收窄为异步断开已接收后的掉电确认失败 | [MainWindow.cpp:3544–3559](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3544-L3559) 只保留 pending，`:1742–1751` 见 connected=false 即隐藏；[PowerService.cpp:653–697](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L653-L697) 保留 error 后清连接。不能声称实机没掉电，不能声称通道被伪写 OFF，实际是 unknown/null | 受控 OFF/OUTP? 失败后，再 IT6332A 复验；保存 action ID、设备读回与主窗持久警告，不以“断开 completed”替代掉电确认 |
| SEC-01 电源满队列优先级 | 同意，为与 P-03 不同的失败阶段 | [PowerService.cpp:509–540](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L509-L540) 先容量拒绝再识别安全操作；已由安全报告原样 Submit 实跑。此时主窗有“请求失败；输出状态未知”提示，不能泛称所有断开失败都静默 | worker 阻塞、128 条排队，OFF/stop/disconnect 仍被安全接收，取消旧 ON；仅有权通道改变，I/O 未确认则 unknown |
| P-04 后台错误 | 同意，P2 状态保存缺口；收窄视觉结论 | `MessageStatus :2046–2055` 非活动会话只日志；`SwitchSession :3567–3605` 无错误恢复。不能据此断言颜色永不刷新或所有断连不可见 | A/B 会话后台 A 失败、切回 A 显示最近原因，B 不串用；32 会话刷新仍保持锚点 |
| P-05 “TCP”卡片文案 | 同意，P3 | [MainWindow.cpp:2772–2774](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2772-L2774) 技术底层是 TCP，但操作入口实际 WebSocket；不是旧协议仍可使用的证据 | 文案 WebSocket/端口可辨，浅深主题不截断 |
| UI-01 键盘离开终端 | 同意缺入口，P2；修正协议范围 | `CreateChild :2180` 和主操作无 WS_TABSTOP；`PreTranslate :1724` 终端不进对话框 Tab 导航。**CMD 的 Shift+Tab/F6 走 :3672–3715 的不支持控制键分支，不发送 VT；SSH 等非 CMD 才到 :3734/:3740 的序列分支**。普通 CMD Tab 可进入草稿。列表收到 Home/End 的自动断言不证明键盘能到达列表 | 物理键盘从终端到列表、断开、命令卡再返回；记录 HWND focus 与设备 TX，导航不得误发设备；保留终端正常 Tab |
| UI-02 高 DPI 宽度持久化 | 同意静态逻辑，P2；高 DPI 实测未运行 | `LoadUiState :4800` 在 `CreateControls/ScaleUiMetrics :2190` 前按 260–650 裁切；`:4817` 保存物理宽度。原始环境仅96 DPI，SFTP拖列检查不是重启检查 | 相同 DPI/相同足够大窗口拖宽>650物理像素，保存重启；记录 settings 和 HWND，96/120/144/192 DPI及跨DPI逻辑宽度核对 |
| UI-03 十步窗越界 | 保留 P2 候选，不升级已复现 | `LayoutCommandDialog :6331–6390` 按步骤扩高，无工作区缩小/滚动；像素高度依字体与DPI。所见两步96 DPI图不能确定十步高DPI footer 越界 | 1366×768、1920×1080与150/200%，1→10和直接打开10步，记录工作区、对话框/footer坐标及可点击/键盘可达性 |
| UI-04 “PASS=全部视觉/可访问性通过” | 反对该扩大解释，接受脚本覆盖缺口 | 原始JSON仍只96 DPI；程序消息输入不是IME；单张宏截图不是动态无残影；列宽变化不是重启恢复 | 真系统DPI、物理键盘/IME、实际桌面连续帧与持久化回读分栏记录 |

## 网络暴露、任意 CMD 与接口越权应分开

同意安全报告的**部署边界事实**：[ApiServer.cpp:68](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L68) INADDR_ANY、[MainWindow.cpp:1778](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L1778) 自动监听；[ApiServer.cpp:292–300](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L292-L300) 无凭据或 Origin 检查；instance 是防误选标识。网络可达方可以向**已有本地 CMD**提交该 Windows 用户可执行的命令，读取已有输出。这是既定“无 token、可信内网、允许 CMD 命令”能力；不能重新包装成鉴权绕过或“API 禁止 SSH/SFTP 所以 CMD 不应运行外部程序”的越权。应用 API 操作集限制不等于 Windows shell 沙箱。

额外接口越权必须证明前置白名单/资源归属/字段检查被绕过、网络创建或断开连接、选择未授权通道等。当前源码 [MainWindow.cpp:6642–6659](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6642-L6659) 把 `power.output` 的 channels 替换为 GUI SelectedChannels，并拒绝其他操作；现有安全测试包含实际写入计数为零的负断言。**本交叉复审未发现可据此成立的新增接口越权。** 满队列可用性、历史响应越限及后台导出阻塞分别是可靠性边界，不合并计成权限漏洞。

Origin 未检查支持“浏览器可尝试连接”的候选，但实际浏览器混合内容/本地网络权限与网络可达性尚未运行，不能写成已证实网页利用。建议在现有可信网络部署中明确 CMD=用户 shell 能力，记录监听网卡/防火墙允许主机；可配置绑定与来源限制列为改进，不无授权新增 token 页面、设备权限或网络操作。

## 产品取舍与发布验收

立即处理 SEC-01（安全动作入队）和 P-02（部分提交/草稿丢失），并安排 P-03 受控故障确认；并行以低成本修正文案 P-01，避免用户把排队理解为完成。近期补 UI-01 焦点入口、UI-02 持久单位、P-04 会话错误恢复和 P-05 提示。UI-03 在实际工作区复现后定实现范围。

不能继续声称“CMD基本编辑未被验证”或“全部断开仅删除列表”：fresh 原始行为证据反对这些泛化。不能声称“全部断开全面通过”“全DPI/Win7/真实设备通过”：同一原始证据明确排除了硬件。具体现场步骤、通过标准和空白记录模板见 [09-manual-acceptance.md](09-manual-acceptance.md)。
