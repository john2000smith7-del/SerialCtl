# SerialCtl 独立高级架构审核

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

审核者角色：高级软件架构设计师。独立结论形成于交叉复审之前；本文件不把其他审核者意见当作独立发现。本次只读生产源码，未修改被审实现。

## 版本、方法及证据范围

- 审核目录：`source/`，完整 SHA `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，`VERSION` 两项均为 `V1.0.6`；与用户指定交付提交相同。分支/远端状态以统一报告原始 Git 证据为准。
- 已按次序阅读 AGENTS、README、architecture README、UI-DESIGN-GUIDE、build-release，以及 V1.0.6/V1.0.5 验收、MANUAL-REGRESSION、WIN7-HARDWARE-TEST；再追踪源码、生产调用链和相关测试断言。README 中的性能/修复陈述不作为通过证据。
- 实际执行：Linux x86_64、g++ 14.2.0，直接编译本 SHA 的 [TerminalModel.cpp](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/terminal/TerminalModel.cpp)，运行原有 [TerminalModelTest.cpp](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/unit/TerminalModelTest.cpp) 与独立双终端响应复现程序。命令、环境、输出及 ELF SHA-256：[model-run.log](../evidence/architecture/model-run.log)；[可执行脚本](../scripts/run-architecture-model.sh)、[复现源码](../scripts/architecture-terminal-response.cpp)。原有模型测试通过，独立复现得到两份自动响应。
- 这些是生产模型的实际执行，不是 Windows 生产 EXE、Win32 窗口、实体 UART 或 Win7 验收。Windows 构建和桌面操作由主审核流程采集后交叉审查，本独立报告不预先声称它们通过。

## 独立结论

统一入口和单设备发送 FIFO 已落在生产调用链中，不只是文档重命名：本地串口/CMD 与网络 `input` 汇入同一 `SessionService` 及同一 `QueuedConnection`；旧 TCP/REST 业务入口未在生产源码中找到。网络 sender 独立、事件唤醒且有界，其结构能隔离 socket 慢发；不能因此宣称高负载延迟或 Win7 实机已经达标。

仍有一个具体的共享终端职责缺口：本机串口窗口和远程桌面窗口都生成并发送终端自动回复；回放历史也会触发这种回复。生产模型已实际复现两份响应，源码完整路由证明两端均有发送分支，但本报告尚无实体串口或两个 Windows 窗口同时运行的 TX 抓包。建议作为高优先级缺陷核实及修复，避免“只打开远端查看”向设备额外输入。

架构的主要后续工作应是明确响应所有权、将会话数据处理从窗口编排中逐步分离、补充背压/取消边界的观测。不建议另建 REST、微服务、ConPTY、.NET 或改写 UI 框架。

## 源码事实与边界

| 主题 | 源码证据 | 独立判断 |
|---|---|---|
| 单一版本协议 | [src/network/ApiServer.cpp:289–315,394–495](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L289-L315)，[src/network/WebSocket.h](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/WebSocket.h)；生产 `bind/listen` 搜索仅命中 [ApiServer.cpp:70](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L70) | 一个 `/serialctl` + `serialctl.v1` 外部网关；HTTP 仅 Upgrade，未知请求有拒绝。SSH/Telnet 是向外连接，不应误当成残留串口业务入口。握手字节不经业务 handler。RFC 帧正确性须结合网络运行结果，源码入口统一不等于全部帧通过。 |
| 本机/AI 输入单路径 | [MainWindow.cpp:3380–3391,3617–3625,6614–6623](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3380-L3391)；[SessionService.cpp:88–113](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L88-L113)；[QueuedConnection.cpp:36–91](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/QueuedConnection.cpp#L36-L91) | 同一 registered shared_ptr，Entry 的输入锁及 QueuedConnection FIFO 保持整批顺序。CMD 与串口都走该路径。SSH/Telnet/远程 COM 的 GUI 仍走其 outbound connection，符合这些资源不向 AI 开放的范围。 |
| 设备读取 | [SerialConnection.cpp:7–31](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SerialConnection.cpp#L7-L31)；[MainWindow.cpp:3403–3408,3829–3836](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3403-L3408) | 一份本地 transport 读取，先 Publish 后投递 GUI；原始 Bytes 在网络分支未经过终端解码。驱动独占及真实 0–255 回环仍需实体证据。 |
| FIFO 的准确语义 | [QueuedConnection.cpp:36–66,70–91](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/QueuedConnection.cpp#L36-L66)；[SessionService.cpp:99–113](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L99-L113) | 8 MiB 排队字节限制；出队后在途批次另占空间。返回 `state=queued,execution=unknown`。它不是任务互斥，也没有多条命令完成确认。SendObserved 的输入事件产生于入队前，不能当作真实硬件 TX 成功证据。 |
| 输出与慢客户端 | [ApiServer.cpp:211–227,245–284,531–575](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L211-L227) | sender 等待 condition_variable，socket Send 在客户端队列锁之外且不持全局锁；每客户端 4 MiB/4096 条。全局 Publish 仍同步编码、复制和分发，详见 A-02。不能把全局锁直接说成慢网络阻塞。 |
| 历史顺序 | [ApiServer.cpp:458–479,531–598](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L458-L479) | 订阅响应→历史→实时在同一锁下排序；每资源 seq，缺口明确。16 MiB 全局淘汰从 map 第一个有记录的资源开始，非时间 LRU，热资源保留公平性不受保证；这是可改进策略，尚非要求违反。 |
| 历史与实时语义 | [SharedSerialConnection.cpp:138–154,189–207](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L138-L154) | 首次远程选择 replay after=0，历史/实时均当普通 output Bytes 进入模型，丢失事件 seq/回放边界供 UI 判别的能力。形成 A-01。 |
| CMD 边界 | [SessionService.cpp:61–86](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L61-L86)；[CmdConnection.cpp:45–76,100–123,151–175](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/CmdConnection.cpp#L45-L76)；[CmdConsoleBridge.cpp:27–74,77–135](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/CmdConsoleBridge.cpp#L27-L74) | 持久 CMD、隐藏控制台、UTF-16 私有管道、WriteConsoleInputW，不是新增网络协议；长行/未完成行/控制字节前置拒绝。控制台输入队列满时 10 ms 检查是输入背压，不能误报为 stdout 轮询。原生控制台桥接在 Win7 的可用性仍需实机。 |
| 终端/备用屏 | [TerminalModel.cpp:708–737](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/terminal/TerminalModel.cpp#L708-L737)；[MainWindow.cpp:3932–3965](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3932-L3965) | 主屏重排与备用屏网格分开处理，未将备用屏当日志重排。Linux 模型套件通过只能证明确定性模型断言；IME、真实字体/DPI、窗口选择及视觉残影另外验收。 |
| 受限电源 | [ApiServer.cpp:431–441,494–495](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L431-L441)；[MainWindow.cpp:6642–6659](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6642-L6659) | GUI 当前勾选通道在 UI 线程读取后提交异步 action；网络不能替换通道或发任意 SCPI/电压/任务。为避免通道选择竞态保留这一步 UI 调度有依据，但 action 状态查询也被 UI 调度，存在可缩小的耦合。 |
| 退出 | [MainWindow.cpp:2119–2162,3511–3559](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L2119-L2162)；[QueuedConnection.cpp:15–31](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/QueuedConnection.cpp#L15-L31)；[CmdConnection.cpp:78–99](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/CmdConnection.cpp#L78-L99) | 本地停止、SFTP cancel/join、CMD Job Object、网络 cancel 等均有具体收尾；不能只因列表清空判定成功。UI 同步 join/文件操作仍需设备/网络故障注入确认时间上界。 |
| Win7 构建边界 | `CMakeLists.txt:3–5,68–88,104–123`；[scripts/check.ps1:109–110,143–157](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/scripts/check.ps1#L109-L110) | C++17/Win32、x64、/MT、6.01、YY-Thunks 1.2.1 固定对象；生产源码未发现 ConPTY/WinHTTP WebSocket 引入。PE 与固定输入哈希检查不能证明所有 Win7 运行语义。PuTTY/VISA 本轮不建议替换。 |

## A-01：多终端自动响应缺乏唯一所有者，历史回放可产生新设备输入

**优先级/状态：P1；生产模型和源码路由已确认；Windows 两窗口/真实 UART 端到端 TX 尚未运行。** 范围：本地 COM 及一个或多个远程 SerialCtl GUI；纯 Python 客户端不会自行 Feed TerminalModel，因此不能把影响扩大到所有 AI 客户端。

触发：设备输出 DSR/DA/窗口尺寸查询，如 `ESC[5n`、`ESC[6n`、`ESC[c`、`ESC[18t`。本地 COM 和远端 GUI 同时显示它，或新远端订阅回放含查询的历史。

路由证据：

1. [TerminalModel.cpp:592–605](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/terminal/TerminalModel.cpp#L592-L605) 对这些序列返回 `result.response`。
2. [MainWindow.cpp:3933–3955](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3933-L3955) 对 mode=1 通过 SessionService 发送，对 mode=3 落入通用 else，通过远程连接发送，没有禁用 observer 的响应。
3. [SharedSerialConnection.cpp:182](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L182) 将这些 bytes 包装为网络 `input`。主机 [MainWindow.cpp:6622](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L6622) 再汇入同一个真实串口 FIFO。
4. [SharedSerialConnection.cpp:140,195–196](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L140) 初次回放与实时使用同一回调；没有回放标记或序号交给 GUI 阻止历史查询副作用。

已执行复现：`bash scripts/run-architecture-model.sh`。80×24 与 100×30 两个生产 TerminalModel 输入同一 query，各得到 27/28 字节响应，总共 55 字节。尺寸回复分别是 `ESC[8;24;80t`、`ESC[8;30;100t`，同一设备会收到互相冲突的几何状态。新建第三个模型回放同一历史，又产生与远端相同响应。详见 [原始日志](../evidence/architecture/model-run.log)。这没有执行 MainWindow、网络及驱动，报告不冒称已抓到实体 TX。

应有行为：一次新设备查询只有选定的终端所有者回复；新增只读展示者或重播历史不得重新向设备发送自动控制序列。实际实现：每个 GUI 模型均可自动回复，来源到主机后还可能显示为 network 而非 terminal-response，诊断不能可靠区分远端用户主动输入与自动应答。

影响：多份状态回复、不同尺寸/光标状态及旧查询回复可能被设备上层当作额外输入；具体设备会否误执行或表现乱码没有本轮硬件证据，不能声称任意命令已被重复执行。

建议：先规定本地串口 owning terminal 是自动响应唯一来源，远端 GUI 只保留人工输入；在 TerminalModel 与输入适配器之间设置可测试的响应策略，保持解码/渲染与设备写入分离。若以后需要终端所有权转移，以显式本机操作及协议能力协商设计，不能默认抢占。即使仅禁止远端响应，仍需保证回放路径永远不产生新副作用。

回归：Windows 两实例 + 受控串口后端，先无远端对每类 query 记录准确一次 TX；再连 1、2 个不同尺寸的远端，比较 TX 完全相同；查询完成后关闭/重连远端，追加 TX 必须为零；最后实体 UART 抓 RX/TX 并核对。应增加的是包含 MainWindow 响应路由的跨层测试，现有 MultiSerialTest 只使用 SharedSerialConnection callback，未覆盖这个 GUI 分支。

## A-02：网关全局锁内的工作仍影响设备回调，性能上界尚未证明

**优先级/状态：P2 优化候选；锁及同步调用事实已确认，性能回归未确认。**

`MainWindow::PostData:3833–3836` 同步调用 `ApiServer::Publish` 后才投递本地 UI。[ApiServer.cpp:535–575](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L535-L575) 持全局 mutex 做 Base64、JSON 构造、逐客户端 payload 复制、历史淘汰（含重复 dump）。`subscribe:461–478` 又在同一 mutex 下复制/序列化最多约 1 MiB 历史。不同 COM 的数据内容隔离，但此处 CPU/锁等待并不隔离。输入 observer 在 QueuedConnection 锁内再调用 Publish（[QueuedConnection.cpp:59–62](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/QueuedConnection.cpp#L59-L62)、[MainWindow.cpp:1772–1775](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L1772-L1775)），也把类似工作加入输入接受路径。

反证与取舍：socket Send 在 [ApiServer.cpp:260](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L260)，此时已释放 c->mutex，且没有持 server mutex；因而“一个慢 socket 必然阻塞所有设备读取”的结论不成立。已有每客户端有界队列设计应保留。

补测：2 个 COM 同时发布；32 客户端订阅 COM-A 并持续 history replay，COM-B 低频标记。测各自 Publish 入口/返回、队列入队/实际 recv、UI 首次 paint，记录锁等待和序列化时间、CPU/working set/thread/handle。若两路 P99 不满足预定预算再优化。

实施：先缓存一次序列化 immutable payload 并共享队列引用；保存 event 字节数避免淘汰时重复 dump；再考虑每资源序列/历史锁及订阅快照。必须保留“订阅确认先于回放且回放/实时不丢不乱”的原子边界，不能直接粗暴把 Queue 移到锁外。代价中等，兼容成本可保持 wire v1 不变。

## A-03：窗口线程仍决定本地会话的数据保留和健康

**优先级/状态：P2 架构限制已确认；本轮未复现 UI 溢出或掉线。**

原始输出先网络发布，但终端解析、文本日志及终端自动响应都在 `MainWindow::AppendData:3900–3973`。GUI 有每会话 4 MiB、总 4096 item、最多64个累积 id 的缓冲（`3838–3860`）。溢出后 `MessageData:2039–2042` 直接 Stop 该真实 transport。安全地显式停止优于静默丢失，但它意味着“GUI 忙不影响服务”只能在缓冲可承受时间内成立；本地显示仍不是可独立退化的订阅者。

另一个具体阻塞点是 `MainWindow::SaveCurrentLog:4069` 同步调用 `SessionLogger::SaveCopy:99–195`。后者等待排队写盘、持 logger mutex 完整复制全部分卷，随后 FlushFileBuffers/MoveFileEx，未提供取消。保存到慢盘/网络路径或大日志时，UI 不泵消息；连带自动终端响应、图形操作及 UI 调度的电源 API 可能延迟，积累到上述阈值会停止连接。这是源码机制推断，当前没有故障注入的耗时数值，不能宣称必然挂死。

建议：近期把日志导出改成有进度可取消后台工作，使用已落盘长度/分卷快照保证导出一致性，禁止直接将 SaveCopy 丢到并发线程而不处理 Stop/rotation 生命周期。随后将 SessionRuntime 提取为设备/解码/日志/终端响应所有者；MainWindow 只订阅展示快照，必要时明确显示“本地显示存在缺口”。是否允许 GUI 丢显示帧必须先确定产品要求，不可让用户误以为终端/日志完整。

验收：大日志+限速可取消目标、持续输出、拖窗、网络订阅和电源状态查询并行；证据至少包括端到端字节数、导出前后 SHA、响应延迟、取消耗时、终端缺口提示及实际 transport 状态。

## 架构实施路线与兼容代价

| 顺序 | 工作 | 用户收益 | 代价、依赖和风险 | 验收 |
|---|---|---|---|---|
| 立即 | A-01 单一自动响应所有者，历史回放禁止副作用 | 增加查看者不再向设备注入额外响应；共享行为可预测 | 小到中；需复核 SSH/Telnet 仍保持正常终端回复；不改 Win7 依赖和 UI 布局 | 双 GUI/历史回放 TX 精确一致，真实串口复验 |
| 立即 | 统一错误、accepted/queued/execution unknown 与任务完成的呈现 | 操作者知道何时只能查询、何时可安全重试 | 小；沿用现有 wire 字段和电源 action，不凭空给串口添加执行完成语义 | 断线/超时无自动重发，GUI 不报告未证明完成 |
| 近期 | 后台可取消日志导出，补 Publish/队列/历史/GUI 缺口观测 | 大日志保存期间窗口与共享不中断，能解释丢链原因 | 中；需独立生命周期及一致性快照；指标不得同步落盘污染热路径 | 大日志及慢盘受控试验，分层延迟与原始字节校验 |
| 近期 | 独立 SessionRuntime，去除 MainWindow mode 数字分支散布 | 降低 UI 改动影响设备/网络的风险，跨层集成测试更容易 | 中到大，逐会话迁移；保留单设备 transport、FIFO、Win7 API、现有主题/交互 | 原有 CTest+双 GUI+断开/退出+真实硬件全部回归 |
| 按测量需要 | A-02 共享序列化 buffer/缓存 byte size；必要时每资源锁 | 降低多客户端复制成本与跨 COM 延迟干扰 | 中；保序与生命周期是主要风险；wire v1 可兼容 | 多资源 replay 干扰试验，不只单 COM 短测 |
| 后续 | 历史淘汰公平策略与内存总预算配置；更精确 capability/类型化 DTO | 多资源同时使用时容量可预测；新增内部操作不误开放 AI | 中；能力仍按白名单，不开放 SSH/SFTP/SCPI/建连等 | 资源数×订阅数矩阵、伪造参数前置拒绝 |
| 后续（需产品决策） | LAN 明确启用/作用域与身份限制 | 降低“开了本机 CMD 即向可达网络提供 shell 输入”暴露 | 不能声称现有无认证系统已授权隔离；需兼容桌面/Python/Win7 和部署策略，保留既定 AI 权限，不把增加 token 当加密 | 不可信网段无法接入；合法客户端故障清楚；权限仍前置验证 |

## 不应过度重构的部分

SessionService 作为小型注册表+输入门禁、QueuedConnection 作为 FIFO、ApiServer 作为 wire adapter 的方向与现有限定规模相符，有具体调用链支持。现在的问题不是需要更多业务入口，而是若干职责仍停留在 MainWindow，以及响应所有权没有划清。保留本机 GUI 直接调用内部服务，不应为了表面统一让 GUI 通过 loopback WebSocket 回连。

CMD 桥接已经具有实际 Win7 平台理由。完整命令提交而非全屏 PTY 是产品边界，不应以增加 ConPTY 来“修好”而违反最低平台；Ctrl+C 等能力须单独设计 Win7 可实现的语义和取消证明。固定 PuTTY/YY-Thunks/NI-VISA 链路本轮不应顺便升级。

## 尚未运行与交叉复审输入

本审核者未运行 Win7、实体串口、IT6332A、真实 SFTP、现场 LAN 或8小时负载，未以 Linux 模型代替 Win32 窗口。Windows 新 run 的原始日志、EXE provenance、GUI截图/中间帧及统计须在交叉复审中接入统一报告。

向交叉复审提交的明确问题：A-01 请软件审核者复查 MainWindow 的 mode=3 路由及既有测试缺口；桌面审核者复核是否能在真实两窗口演示回放副作用；产品审核者确认增加远端查看者不应改写设备状态。A-02 请按原始测量判定优化优先级，不能由投票或锁的存在宣布性能缺陷。A-03 请区分当前显式停止策略、可恢复用户体验和未测故障概率。
