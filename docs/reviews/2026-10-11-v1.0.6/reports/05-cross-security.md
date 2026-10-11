# 安全交叉复审：终端副作用、CMD 粘贴与电源断开

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

复审对象：`source/`，SHA `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，V1.0.6。先读 `source/AGENTS.md` 与独立安全稿，再读架构/产品独立稿并回查生产源码及证据。本文件单独保留交叉意见，不覆盖任何独立稿。源码 `git status --short` 为空；本阶段未修改生产源码、未操作远端、未新增 Windows 运行。

结论：**接受 A-01 自动回复所有权缺陷和 P-02 粘贴非原子状态变更；接受 P-03 的异步错误传播缺口，收窄其物理安全结论为待专项验证。** 三项均不能被 fresh Windows 14/14 CTest 或 35 CMD 清理成功排除。

## 逐项裁定

| 原意见 | 安全复审裁定 | 可确认层级 | 不能宣称的结果 |
|---|---|---|---|
| A-01 多终端/历史重复自动响应，P1 | 接受；P1 作为设备输入完整性修复优先级合理 | 两个生产 TerminalModel 实跑分别生成回复；本次重新核对完整生产发送路由 | 尚无两台/两实例真实 Windows GUI 至 UART 的重复 TX 抓包；不能称为任意命令重复执行或所有 AI 客户端受影响 |
| P-02 CMD 多行粘贴前缀已接受、超长尾行丢失，P2 | 接受；收窄“无法恢复”字样 | 原样粘贴分支与生产编辑器的受控运行确认先发送、后失败、活动草稿清空 | 未证明 cmd.exe 实际执行；不能称剪贴板被清空、全部文本不可恢复或权限提升 |
| P-03 全部断开掉电失败提示不闭合，P1 候选 | 接受源码级缺口；保留 P1 候选与故障注入门槛 | OFF 失败被 Disconnect 捕获存入 state.error，disconnect action 仍可 completed；主窗仅等待 connected=false | 不能称设备实际仍加电已复现，不能称状态伪造 OFF，也不能称断开会关闭所有手动通道 |

## A-01：接受完整调用链，保留运行层界限

1. [source/src/terminal/TerminalModel.cpp:592–605](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/terminal/TerminalModel.cpp#L592-L605) 对 DSR、DA、窗口尺寸查询生成 `response`。`evidence/architecture/model-run.log:11–16` 保存两个生产模型共 55 字节响应及历史重新 Feed 后再次响应；两个尺寸分别为 80×24、100×30。`scripts/architecture-terminal-response.cpp:14–26` 明确只执行模型。
2. [source/src/app/MainWindow.cpp:3933–3955](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3933-L3955) 每次 Feed 后处理 response：本地 mode=1 经 SessionService；远端 mode=3 落入一般 Send 分支。CMD mode=4 有明确禁止回复处理，因此本意见不扩展到 CMD。
3. 远端使用 SharedSerialConnection 的构造路径在 [MainWindow.cpp:3352–3363](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3352-L3363)；其 [source/src/connections/SharedSerialConnection.cpp:172–182](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L172-L182) 将回复转换为网络 `input`，主机 [MainWindow.cpp:6621–6623](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6621-L6623) 交给同一 SessionService。没有仅供观察的应答过滤。
4. [SharedSerialConnection.cpp:138–154](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L138-L154) 初次订阅请求 after=0；`189–196` 将 output 的 data 直接交回 GUI，没有携带历史/实时标记。因此，存在历史查询再次变成设备输入的生产路径；具体调度和设备 TX 仍须 Windows 跨层验证。

现有 fresh `multi_serial` 测试通过见 `evidence/windows-fresh/ui/serialctl-ui/ctest-x64.txt:197–208`，但 [source/tests/integration/MultiSerialTest.cpp:31–40](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/MultiSerialTest.cpp#L31-L40) 的“desktop”仅实例化 SharedSerialConnection，输出回调只是累加字节，不调用 MainWindow 或 TerminalModel。这不是 A-01 的反证。纯 Python 客户端不自动解析终端查询，也不受这一具体生成机制影响。

建议统一报告保留“生产模型实跑＋生产路由确认，Windows 双窗口/实体 UART 未特测”的标签。修复应明确本地拥有终端的自动应答所有权，并禁止历史回放产生新设备输入；不能直接禁止 SSH/Telnet 正常终端回复。验证需对比零/一/两个远端时同一 query 的准确 TX，再验证重连回放新增 TX=0。

## P-02：接受部分提交，纠正恢复能力表述

[source/src/app/MainWindow.cpp:4249–4262](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L4249-L4262) 先组合草稿与剪贴板，用最后一个换行分出提交前缀，再 SendBytesToActive；只有成功后才 SubmittedBatch、Insert 尾行。[source/src/terminal/CmdLineEditor.h:16–21](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/terminal/CmdLineEditor.h#L16-L21) 限制 8191 字符；`83–100` 显示 SubmittedBatch 记录完整行历史后清除活动草稿和光标。因此 8192 字符尾行失败发生在前缀已经接受之后。

`evidence/product/paste-tail-probe.cpp:15–17` 明确使用发送接受记录器、简化 ASCII 编码和绘制替身；`43–57` 是具体输入及断言。`evidence/product/paste-tail-probe.log:3–10` 证明接受批次=1、原活动草稿14字符、最终活动草稿0字符、尾行8192字符并报告错误。该 ASCII 场景不受 Linux wchar_t 与 Windows UTF-16 长度差异影响，但它没有验证 Unicode 编码、真实剪贴板 API 或 cmd.exe 执行。

收窄两点：

- “原草稿无法恢复”应表述为“原活动草稿及光标未自动保留/回滚”。已提交组合前缀进入历史，原粘贴内容仍在剪贴板；不是证明所有文本从此不可恢复。原草稿和追加文本的边界不会由当前失败处理恢复。
- 前缀在此证据中为“已接受”，不是“已执行成功”。安全影响是用户重试整个粘贴可能重复提交先前前缀；不涉及绕过用户权限。

fresh 模型通过见 `ctest-x64.txt:140–151`，但 [source/tests/unit/TerminalModelTest.cpp:229–238](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/unit/TerminalModelTest.cpp#L229-L238) 测的是直接调用 SubmittedBatch 后正常尾行/历史恢复，没有通过 MainWindow 粘贴入口覆盖8192尾行。真实 GUI smoke 的 CMD 正常输入通过也不能排除此边界。

建议 P2 保留。全部长度/编码/批次校验应在临时状态完成，失败时不提交前缀，成功后再更新编辑器。专项 Windows 复验用无害外部标记证明“失败前 TX/执行次数=0”，同时核对光标和原草稿；不要自动重发。

## P-03：接受失败传播缺口，反对把断开等同所有通道 OFF

[source/src/power/PowerService.cpp:578–651](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L578-L651) 的 Outputs 有逐通道 readback；失败置 unknown，OFF 失败还尝试无确认补写，最终抛出异常。`653–667` 的 StopTask 只对 owned_ 任务拥有通道调用 OFF；非任务通道不在这个收尾承诺内。

[PowerService.cpp:675–697](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L675-L697) 的 Disconnect 捕获该异常，只保留 state.error，然后关闭 transport、清 ownedChannels，并把所有通道读数和 output 置 null。`789–790,863` 说明 disconnect 返回后 Execute 仍会标 action=completed；`889–892` 的操作记录可能只呈现 completed。故 action 完成至多说明断开流程结束，不能表达掉电确认成功。

主窗 [source/src/app/MainWindow.cpp:3544–3559](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3544-L3559) 已正确处理 **Submit 立即拒绝** 并显示错误；本意见针对接受之后的异步失败，不能笼统称所有失败都静默。已接受时只保存 pending bool，没有保存 action ID。`1742–1751` 在 connected=false 时隐藏电源页且不检查 state.error。`4045–4046` 的非错误处理中提示3.5秒后消失。[source/src/app/PowerPane.cpp:458–474](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/PowerPane.cpp#L458-L474) 的失败通知只跟踪自身 pendingActions，不能补上主窗未登记的 disconnect。

现有 `power_service` 在 fresh Windows 通过，见 `ctest-x64.txt:19–29`；测试源码 [source/tests/integration/PowerServiceTest.cpp:99–125](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/PowerServiceTest.cpp#L99-L125) 覆盖 retained ownership、正常 Shutdown 后 unknown、连接读回失败、加电失败后的补救，**没有覆盖任务 OFF 读回失败时的 disconnect action 及 MainWindow 最终提示**。`evidence/windows-fresh/workflow-logs/0_build.txt:434` 的35 CMD/0.375秒清理也没有实际电源路径。

建议保留 P1 候选，标题明确为“全部断开未持续展示任务掉电确认失败”。确认 state.error 保留是有价值的正确行为；[PowerPane.cpp:1027,1046–1048](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/PowerPane.cpp#L1027) 在电源页可显示该错误，但主窗已隐藏此页。不能宣称错误从全部状态或日志中消失；“记录可能仅 completed”指内存 action/records，[PowerService.cpp:683](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L683) 已先停止文件 logger，不应推断断开后的 completed 必然写入文件。先用 Windows 受控 SCPI 后端制造已拥有通道 OFF/readback 故障，验证 action、state.error、unknown、主窗提示和日志；再 IT6332A 拔线/超时实测物理输出。修复要将 transportDisconnected 与 taskOutputsConfirmed 分开，持久呈现未确认通道，不应扩大到关闭未拥有通道。

## 与安全独立结论及 fresh Windows 证据的关系

安全独立 SEC-01 是 Submit 满128条队列时 OFF/stop/disconnect 无法接受；P-03 是已接受 disconnect 之后的异步结果传播。两者触发点不同，应分别保留，不合并成一项笼统“电源退出失败”。本次未发现需要撤回 SEC-01/SEC-02 的反证。

已取得并检查 fresh run [38097145874](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38097145874) 详细 `evidence/windows-fresh/ui/serialctl-ui/ctest-x64.txt`：14个测试均 Passed，含生产 CMD、网关、共享串口、多COM和模型测试。它支持 Windows Server 2022 上这些受控测试实际运行成功；不会自动提升上面三项为“已在 Windows GUI 复现”或“已被Windows测试排除”。本交叉阶段只复核已有运行证据与源码，未重新执行模型/探针，未新增 Windows GUI、物理 UART、电源、Win7 或8小时实跑。
