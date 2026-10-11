# 独立审核 1：高级软件工程与安全

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

审核对象：`fix/unified-websocket-win7`，源码完整 SHA `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，`VERSION=V1.0.6`。这是交付提交本身，未修改生产源码。审核者在本报告初稿形成前未交换其他审核者的结论；本文件保留独立判断，交叉复审另记。

结论：已读生产源码和实际运行受控测试，发现两项可重复的生产逻辑缺陷：**电源队列满时停止/掉电/断开优先级失效**、**历史查询可能生成超过协议上限的响应而被自己的客户端拒收**。另确认同步日志导出阻塞 UI 的实现路径、公开 CMD 网络权限的真实信任边界，并保留慢握手/磁盘故障等未完成运行项。不能因 14/14 CTest 成功认定这些边界通过。

## 方法及证据层级

依次读 `AGENTS.md`、`README.md`、架构 README、UI-DESIGN-GUIDE、build-release、当前/历史验收与人工测试文档，再读 `src/network`、`SessionService`、`QueuedConnection`、CMD bridge、串口、日志、电源提交路径及对应测试。`legacy/csharp` 未作为生产依据。

- 本审核者实际运行：Linux `g++ -std=c++17 -O2 -pthread` 编译**按函数边界原样抽取的生产方法**，执行电源提交队列与历史报文边界；Python 实际调用公共 `Wire.receive()` 核实拒收；现有 Python 核心测试 10/10。抽取源码是受控测试接缝，替代了电源消费者调度、历史数据来源，不能证明 Windows 驱动/GUI/设备行为。
- 本轮团队实际 Windows 运行：run [38097145874](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38097145874)，主代理取得的 [原始作业日志](../evidence/windows-fresh/workflow-logs/0_build.txt) 记录 clean release、14/14 CTest、PE/包检查及真实 GUI 脚本完成。我核对了该日志与测试源码；这不是本审核者自行操作的 Windows 桌面。完整原始 artifact 的可取得情况以统一报告为准。
- 不引用交付前 run `38095158673` 或文档里的旧压力数字作为本轮测量；不把 Windows Server 2022、模型测试、子系统 6.01 当成 Win7 通过。
- [可复跑抽取脚本](../scripts/security_source_harness.py)、[编译实际源码](../evidence/security/production_routines.cpp)、[SHA/范围声明](../evidence/security/harness-manifest.json)、[结果](../evidence/security/production-routines-result.json)、[Python 日志](../evidence/security/python-client-tests.log)。

## 已确认缺陷

### SEC-01 / P1：电源容量校验挡住安全优先操作

**状态：源码确认 + 原样生产 Submit 受控实跑；实体电源影响未运行。**

代码：[src/power/PowerService.cpp:509–510](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L509-L510) 先要求 `queue_.size()<128`；`526–540` 才识别 stop/disconnect/output OFF 的优先级并清除排队加电。[src/app/MainWindow.cpp:3544–3548](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3544-L3548) 的“全部断开”也通过同一 Submit；满队列会报告失败，电源队列继续保留。不是误报已掉电，而是安全收尾动作无法入队。

触发：电源 worker 在设备 I/O 等待中，128 条合法命令已经排队。实际测试使用不消费队列的调度接缝，调用原样 Submit 填充 128 条 output ON；之后分别提交 OFF、stop、disconnect。

实际：三项均返回 `Power command queue full or shutting down`，队列前后仍为 128。预期：队列容量不能阻止安全停止；允许清除/合并待执行加电，再把停止放到保留容量；不应越过当前不可取消的硬件 I/O，也不能谎称输出已关闭。

影响：GUI 总掉电、停止、全部断开及网络总掉电的可用性；设备慢响应/突发操作时尤其重要。程序最终 Shutdown 独立清队列，因此本缺陷不等于证明正常退出永不收尾。

修复：在统一锁内区分停止状态与普通容量；为安全控制保留槽位或先取消可取消的排队加电；合并重复 OFF；保留失败/未知的明确状态。回归：受控 worker 阻塞后填满 128，分别检查 GUI/网络 OFF、stop、disconnect 被接收，旧 ON 不再执行，只操作任务拥有/GUI 当时选择的通道；断线仍不得声称成功。再用 IT6332A 核对物理输出。

### SEC-02 / P2：history 长度检查漏算 response 外层

**状态：源码确认 + 生产历史方法受控实跑 + 生产 Python 解帧实跑。**

代码：[src/network/ApiServer.cpp:592–597](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L592-L597) 只检查 `result.dump().size()`；`499–512` 再加入 type/version/instance/requestId/ok/result；`211–225` Queue 无协议总消息上限检查。客户端 [src/network/WebSocket.cpp:326](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/WebSocket.cpp#L326) 与 [tools/serialctl_ws.py:145–146](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tools/serialctl_ws.py#L145-L146) 拒绝 >131072 字节。

复现：按 Publish 相同 event 字段构造合法 Base64 历史（每事件数据小于 16384 原始字节），令内部 result 为 131069 字节，使用合法的 128 字节 requestId。调用原样 Events，再构造 Request 使用的标准外层；公共 Python Wire 接收其实际声明长度。

实际：Events 未报 `HISTORY_TOO_LARGE`，外层为 **131309** 字节；Python 返回 **Message too large**。预期：整个 JSON 消息不超过 131072；超限返回结构化 HISTORY_TOO_LARGE 并保留连接，提示 subscribe。

影响：接近历史上限的 `events` 查询中断公共/桌面客户端连接，打断其他订阅；不是任意长度输入绕过或设备越权。修复：在完整 envelope 序列化后统一校验；events 预留精确外层开销，避免 requestId 长度改变边界；不要截断 JSON 或丢历史。回归：总长 limit−1/limit/limit+1、requestId 长 1/128、多字节元数据，原生/Python 客户端均应得到合法结果或业务错误，连接保持。

## 源码确认的问题及运行候选

### SEC-03 / P2：同步日志导出可能造成 UI 卡顿及随后接收溢出

**源码确认**：[src/app/MainWindow.cpp:4050–4073](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L4050-L4073) 在 GUI 线程同步 SaveCopy；[src/logging/SessionLogger.cpp:101–195](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/logging/SessionLogger.cpp#L101-L195) 从等待队列排空到 Flush、遍历所有分卷、写目标、最终改名都持 logger mutex。`WriteRaw/WriteText/WriteStatus` 也取该锁，所以注释“Producers never wait for disk”只对正常 Run 写盘路径成立。

**尚未实跑的影响**：大日志写入慢盘或不响应的网络路径时，GUI 无法处理 MessageData；[MainWindow.cpp:3840–3859](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3840-L3859) 每资源 4 MiB/总4096消息接收上限后丢弃新增数据并标溢出；`2039–2042` 恢复处理时停止 transport。日志落盘在 AppendData 的 GUI 消费侧，因此被丢弃部分不会进入会话日志。缺少真实慢存储注入及字节核对，报告不宣称已经复现数据丢失或无限挂起。

复验步骤：至少 65 MiB 日志持续输出，导出至可控限速磁盘/共享，记录 UI heartbeat、RX 累计字节/哈希、溢出状态、export耗时与取消；分别测试磁盘满、权限拒绝、路径掉线。预期后台导出不阻塞消息循环、快照完整、失败不覆盖旧目标、存在取消/进度。建议独立导出 worker，短锁取得已提交分卷快照；不得持写入锁复制全量历史。Windows 7 可用普通线程实现，无须新运行时。

### SEC-04 / P2：1.5 秒是单次 recv 超时，不是握手总时限

**源码确认，实际 Windows 慢滴网络未运行**：[WebSocket.cpp:146–165](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/WebSocket.cpp#L146-L165) 循环 recv，无单调时钟总 deadline；[ApiServer.cpp:189](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L189) 只设 `SO_RCVTIMEO=1500`。每 0.9 秒补一个合法 header 片段可继续等待，最多到 8 KiB；128 个握手也计入容量（`177`）。现有测试覆盖完全空闲与满载，未覆盖持续小量补字节。服务本就无认证且允许 128 个正常闲置连接，因此不要把这个问题夸大成新出现的唯一 DoS 路径。

[无设备写入的探测脚本](../scripts/security_handshake_probe.py) 每次只建立一个连接，检查任意 Origin 和 >1.5 秒握手；本轮未执行。预期总 handshake deadline 到达后返回426/关闭且回收槽位；基于绝对 deadline 计算剩余时间，不依赖每次 recv 重置；Stop 也须及时取消握手。

## 安全暴露边界：事实、风险与建议

[ApiServer.cpp:68](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L68) 为 `INADDR_ANY`；[MainWindow.cpp:1778](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L1778) 启动即监听；[ApiServer.cpp:292–300](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L292-L300) 未验证 Origin 或认证凭据；hello 暴露 instance，实例 ID 是防误选标识而非秘密；[MainWindow.cpp:3380–3391](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3380-L3391) 发布既有本地 CMD；`6621–6623` 可输入完整命令。**任何获网络可达性的客户端可对已打开 CMD 执行该 Windows 用户权限下的命令**，也可读取已有串口/CMD 输出及控制已选电源输出。这是既定无 token、可信内网模型的实际能力，不是假称发现“认证绕过”，也不是 API 白名单保护失败。

Origin 缺失带来浏览器跨站访问候选：能连到服务的网页可尝试 WebSocket；浏览器混合内容/本地网络权限策略各异，本轮未运行浏览器，不宣称网页利用链已被证明。传统防火墙仅限制远端主机时，本机浏览器路径仍需单独评估。

立即收益明确的建议：现场防火墙将7000–7015限制到指定控制主机/接口/隔离实验网；文档明确 CMD 等同用户 shell 权限。近期可拒绝无业务需求的浏览器 Origin、增加可配置绑定地址/来源主机限制及当前监听范围提示。若未来需要非可信网络，须另立鉴权/加密项目，评估 Win7 库与部署成本；本轮不凭空要求新增 token UI 或扩大 AI 操作集。API 禁止 SSH/SFTP 等操作名不意味着 CMD 是操作系统沙箱。

SSH 密码确实以 `-pw` 命令行传递（[src/connections/PlinkConnection.cpp:87–88](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/PlinkConnection.cpp#L87-L88)），与 README 披露一致；多用户主机按现有支持使用密钥/Pageant，不能从包哈希检查推断运行时凭据保密。

## 测试有效性与证据缺口

- [ApiServerTest.cpp:16–41](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/ApiServerTest.cpp#L16-L41) 不只是断言 error：还断言 `powerWrites==0`、串口收到0字节；`44–53` 核对重复 ID 只有4字节；`54–109` 核对合法 FF F4 FF 和旧协议/control 不入设备。这些是有价值的行为断言，替身仍不是UART。
- RFC解析源码支持掩码、RSV、最短长度、分片、控制帧、UTF-8和Close约束；不能只据此宣布完整 RFC 合规。已有测试缺 UTF-8 多类反例、累计分片边界、不同Close码、握手总deadline。`Host` 必需性也未验证，建议补协议互通/负向矩阵。
- [SessionLoggerTest.cpp:13–46](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/SessionLoggerTest.cpp#L13-L46) 实际覆盖65 MiB、分卷、完整副本和拒绝覆盖活动源；未注入磁盘满、WriteFile/Flush失败、慢盘或导出期间并发接收。`docs/testing/V1.0.6-ACCEPTANCE.md:20` “磁盘失败测试通过”超出当前可找到测试的证明范围。标记**未验证**，不能复制到统一报告通过栏。
- ApiServer 请求响应缓存256条/1 MiB、每客户端4 MiB/4096条、输入FIFO8 MiB均有界；events/history边界见SEC-02。批次原子入队只保障完整批次串行，不提供多步任务互斥。去重缓存淘汰后同 ID 可再执行；文档已限制跨重连幂等，客户端断线不自动重发，这一边界应保留。
- [QueuedConnection.cpp:59–64](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/QueuedConnection.cpp#L59-L64) observer 在写队列锁内，[SessionService.cpp:100–113](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/SessionService.cpp#L100-L113) 返回 queued/execution unknown；未把“接受”当设备执行成功。[CmdConnection.cpp:55–64](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/CmdConnection.cpp#L55-L64) 的 Job Object 和私有控制台设计有明确收尾路径；Win7嵌套job失败会拒绝启动。实际 Win7、Ctrl+C/全屏交互、IME仍不能由2022报告替代。

## 重点项目的本专家判定

| 项目 | 判定 | 依据/边界 |
|---|---|---|
| 单一WebSocket/旧入口拒绝 | 部分通过 | 生产入口及测试实效支持；硬件首次字节未运行 |
| 多客户端/多COM/输入批次 | 部分通过 | FIFO单写者及受控Windows测试；任务级互斥不存在且已声明 |
| RFC6455/二进制0–255 | 部分通过 | 核心反例与全字节受控测试；SEC-02响应长度失败、更多负向边界未跑 |
| 事件唤醒/慢端独立队列 | 部分通过 | CV、独立发送线程/队列、Windows测试成功；8小时未运行 |
| CMD提交/状态/退出 | 部分通过 | 有生产CMD2022集成；不能据此认定Win7/IME/全屏交互通过 |
| 终端/滚动/DPI/动态绘制/模态 | 交由桌面独立报告 | 本专家不凭源码宣称无闪烁 |
| 全部断开/电源安全收尾 | 失败（容量边界） | SEC-01；正常负载退出测试成功不覆盖满队列 |
| 固定AI操作集 | 部分通过 | 白名单/字段/资源前置检查有效；公开CMD信任边界如上 |
| 日志增长/磁盘失败 | 部分通过 | 增长分卷有断言；磁盘故障与慢导出未运行 |
| Win7/物理串口/电源/真实SFTP/8小时 | 未运行 | 保留人工验收，不降低标准 |

建议实施次序：先修 SEC-01，再修 SEC-02；补满容量安全控制和完整消息边界回归。随后后台日志导出与磁盘注入测试、绝对握手deadline。部署前明确可信网络边界，执行Win7和实际设备清单。所有变更保持C++17/Win32、x64、/MT、6.01、YY-Thunks及固定PuTTY链路；本次未提供已合并补丁，也不宣称缺陷已修复。
