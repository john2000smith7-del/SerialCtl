# 架构交叉复审：安全结论与本轮压力证据

> 仓库归档说明：本报告记录 2026-10-11 的审核，源码固定为 `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。独立初稿中的阶段性限制及过宽表述，以统一报告与交叉复审裁定为准；仅修正导航链接，不把未运行项改为通过。完整 209 张图来自同次 Actions artifact，本仓库保留 17 张关键原图及全部文本证据，见 [证据索引](../evidence/README.md)。

复审对象为只读 `source/`，完整 SHA `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。先读 `source/AGENTS.md` 和独立架构报告，再逐项复核独立安全报告及其生产源码、受控复现接缝和本轮 Windows 原始 CTest。未修改生产源码、远端或原独立报告。本文件记录接受、收窄和不支持的推论，不以专家人数替代证据。

结论：接受 SEC-01 电源满队列安全控制失效和 SEC-02 历史响应 envelope 越界；接受 SEC-03 的同步导出阻塞机制，但修正锁语义且不认定已发生无限挂起；接受 SEC-04 缺少握手总 deadline 的源码事实，不把它单独升级成已实测网络拒绝服务。新 Windows 数据证明本次受控短测下 1/2/8/32 客户端转发和资源回收，不能证明 128 客户端性能、物理串口延迟、UI 绘制延迟或长期稳定。

## 对独立安全结论逐项取舍

| 项目 | 复审决定 | 核对后的证据及限制 |
|---|---|---|
| SEC-01，电源容量阻止 OFF/stop/disconnect | **接受，P1** | [src/power/PowerService.cpp:509–510](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L509-L510) 在锁内先检查队列 `<128`，`526–540` 才识别安全优先操作及移除待执行加电，因而满队列条件下后者必定不可达。[src/app/MainWindow.cpp:3544–3548](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3544-L3548) 全部断开调用同一 Submit，错误会被明确显示。抽取测试保留 Submit 本体，替换不消费的 worker 与 State 接缝，足以证明容量分支，不等于物理电源已无法关断。[PowerService.cpp:982–986](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/power/PowerService.cpp#L982-L986) Shutdown 另行停止并清队列，故反对推论“正常程序退出也必定无法收尾”。 |
| SEC-02，events 外层造成协议上限越界 | **接受，P2** | [src/network/ApiServer.cpp:592–597](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L592-L597) 只判断内部 result；`499–512` 加上 response/version/instance/requestId/ok；`207–225` Queue 只检查队列容量，不检查完整消息最大值。受控结果为内部 131069、外层 131309、大于 MaxMessage 131072；[src/network/WebSocket.cpp:326–327](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/WebSocket.cpp#L326-L327) 与 [tools/serialctl_ws.py:145–146](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tools/serialctl_ws.py#L145-L146) 会拒收。错误属于自身协议长度约束和可用性，不是内存越界或设备权限突破。 |
| SEC-03，同步日志导出 | **接受机制，收窄锁与影响描述，P2** | [src/app/MainWindow.cpp:4067–4073](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L4067-L4073) 在保存对话框返回后同步执行 SaveCopy；[src/logging/SessionLogger.cpp:101–102](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/logging/SessionLogger.cpp#L101-L102) 的 condition_variable wait **等待期间释放 mutex**，不是“从等待排空开始始终持锁”。等待结束后 `113–195` 刷新/复制/改名持该锁；`65–96` 三个 producer 方法取同一锁。没有构成必然死锁的循环等待证据，但慢 I/O 可以让 UI 同步等待并阻塞其他 logger 操作。 |
| SEC-04，握手 1.5 秒 | **接受源码事实，保持未实跑** | [src/network/ApiServer.cpp:177–189](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L177-L189) 先计入连接容量，再设置 socket 超时；[src/network/WebSocket.cpp:146–167](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/WebSocket.cpp#L146-L167) 持续 recv，只有 8 KiB header 大小限制，没有绝对截止时间。小块数据可反复重置单次等待，故“握手总时限 1.5 秒”不成立。完整 Windows 慢滴重现和槽位回收时限未运行；既有允许正常 idle 连接的设计本身也可占满 128，不应描述为新发现的唯一 DoS 途径。 |
| 可达网络对已打开 CMD 的权限 | **接受为既有信任边界** | [src/network/ApiServer.cpp:68,292–300](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L68) 绑定所有 IPv4 接口且未验证 Origin/凭据；[src/app/MainWindow.cpp:3380–3391,6621–6623](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3380-L3391) 公开既有 CMD 输入。符合已声明的无 token 可信网络模型，不能改写为“绕过现有认证”。白名单约束 API 名称与资源，不能将 CMD 变成操作系统沙箱。浏览器利用链没有实跑证据。 |
| 密码命令行及兼容依赖 | **接受事实，不作越界保证** | [src/connections/PlinkConnection.cpp:87–88](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/PlinkConnection.cpp#L87-L88) 以 `-pw` 传密码。包/PE 哈希及 Win7 import 检查不能证明运行时凭据隐私；本轮不建议因此无依据替换已固定 PuTTY、YY-Thunks 或新增运行时。 |

SEC-02 另需收窄“桌面客户端连接中断”的触发范围：原生解帧器确实会拒收超长消息，但普通桌面 [SharedSerialConnection.cpp:138–150](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/connections/SharedSerialConnection.cpp#L138-L150) 首先使用逐 event 流式 subscribe；仅 HISTORY_GAP 后才调用 `events(after=0)`，此时 [ApiServer.cpp:582–590](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L582-L590) 的 gap 分支返回空 events。因此本次证据不能证明普通远端 GUI 首次订阅必然触发越界。可直接确认的是主动请求接近上限历史的协议客户端会收到自身解帧器拒绝的响应；Python 后台接收失败还会通知同连接所有 pending/subscription 并 shutdown socket（[tools/serialctl_ws.py:232–258](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tools/serialctl_ws.py#L232-L258)）。

受控复现证据为 [production-routines-result.json](../evidence/security/production-routines-result.json)、[harness-manifest.json](../evidence/security/harness-manifest.json) 与 [security_source_harness.py](../scripts/security_source_harness.py)。本复审阅读并核对这些文件和抽取边界，未将同一次执行冒充第二份独立实跑。harness 中的历史事件符合 Publish 字段和合法 Base64 结构，但 history 容器直接注入、envelope 按生产 schema 构造；不是完整 Windows ApiServer 网络端到端复现。

SEC-03 后续影响的调用链成立：收到的原始字节先 Publish 再排 UI（[MainWindow.cpp:3829–3836](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3829-L3836)）；UI 接收缓冲超过每资源 4 MiB/总 4096 item 时丢弃新增 item 并标记（`3841–3859`）；恢复 MessageData 后停止该 transport（`2039–2042`）。日志处理发生在 AppendData（`3900–3934`），因此若该溢出实际发生，被丢弃部分不会经过该日志路径。**缺少慢盘/断网盘注入与 RX、日志字节对账，本轮仅确认风险机制，不宣称已经出现丢失、永久挂起或自动退出。** 65 MiB 日志测试通过支持正常分卷/完整复制及源保护，不支持磁盘满、Flush 失败、并发导出不阻塞。

## 本轮 Windows 压力原始证据

使用 run `38097145874` 的 [ctest-x64.txt](../evidence/windows-fresh/ui/serialctl-ui/ctest-x64.txt)，不引用交付前 run。`workflow-logs/0_build.txt:12,17,95,107,137` 记录 Windows Server 2022 / windows-2022 和相同 SHA。CTest 从 Oct 11 00:06 UTC 到 00:07 UTC，14 项均通过；`serial_share_backpressure` 总耗时 **10.99 秒**。该总时长包含慢客户端用例和各档短测，不能当作每档运行时长。

已提取原值及日志 SHA-256 到 [fresh-stress.csv](../evidence/architecture/fresh-stress.csv)，校验客户端数为 1/2/8/32、样本数为客户端数×1000、已核对输出字节数为客户端数×1000×64。内存和 handles/threads 为同一测试进程指标；CSV 不添加虚构硬件吞吐或 UI 指标。

| 客户端 | 样本 | P50 ms | P95 ms | P99 ms | 核对输出字节 | wall ms | CPU ms | working set 字节 | idle/active/reclaimed 线程 | idle/active/reclaimed handle |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | 1000 | 0.0641 | 0.0827 | 0.1106 | 64000 | 69.0557 | 93.75 | 8986624 | 8/13/8 | 119/138/119 |
| 2 | 2000 | 0.0888 | 0.1038 | 0.1220 | 128000 | 97.9975 | 109.375 | 9097216 | 8/15/8 | 119/149/119 |
| 8 | 8000 | 0.1120 | 0.1541 | 0.1673 | 512000 | 153.6480 | 343.75 | 9453568 | 8/27/8 | 119/215/119 |
| 32 | 32000 | 0.3588 | 0.5871 | 0.6674 | 2048000 | 549.2670 | 1031.25 | 11132928 | 8/75/8 | 119/479/119 |

测量起止与约束以 [tests/integration/SerialShareBackpressureTest.cpp](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/SerialShareBackpressureTest.cpp) 为准：

1. **延迟起点/终点**：`117–127` 在构造 64 字节 Bytes 和调用 Publish 前取 QPC；每个客户端按 vector 顺序同步 Receive，核对 kind、Base64 解码和字节相等之后取结束 QPC。同次 Publish 的所有客户端共享起点，后一个观察值包含前面客户端接收/解码/断言时间。它是测试调用方观测的 Publish→接收校验延迟，不能简称网络传播时间或独立服务端处理时间。
2. **样本和百分位**：1000 次 Publish，每次等待全部客户端消费后才发下一次，不是独立恒速负载发生器。`130–133` 排序后取 `floor(q*N)` 的零基下标，不插值；32000 样本来自 1000 次发布×32 接收者，不能当成 32000 个独立发布样本。
3. **wall/CPU**：`114–138` wall 从 active Measure 后取 QPC，到循环结束、排序、百分位输出之后取 QPC；包含验证、排序及控制台输出，不含连接/订阅和清理。CPU 为整个进程 kernel+user 时间增量（`12–30,145–146`），初始读取位于 active Measure 内、在线程快照前；终值位于 end Measure 内、在 wall 结束和 handle/memory 读取之后，边界并不完全重合。多个线程可使 CPU ms 高于 wall ms，不能直接写成机器总体 CPU 百分比。
4. **资源量与回收**：active 在连接/订阅后、发布前；working set 在发布结束后、清理前；peak working set 为进程生命周期累计峰值，各档均为 11554816 字节，不是各档独立峰值。先前慢端测试的 fixture 仍在作用域内，基线不是空进程。reclaimed 在 client clear、server/两个 controlled COM Stop 后记录（`139–150`）。`151` 断言线程数回到 idle 以下或相等；handle 相等是本次观测值，没有相应硬性断言，亦无长时间泄漏结论。延迟、CPU、内存指标也只有打印值，没有通过阈值，不能把 Test Passed 等同于满足某个性能预算。
5. **多 COM 范围**：fixture 确有两个 controlled COM，但这四档只发布 `session-5`（`105–120`），未同时持续压 COM-A 和 COM-B，也未混入反复历史订阅、输入竞争或 UI 工作。循环直接调用 ApiServer::Publish，未测 Windows 串口驱动读回调及真实 UART 波特率；没有 MainWindow 绘制和终端解码耗时。
6. **慢端隔离范围**：同文件 `38–90` 把一个 socket 接收窗设成 4096 并停止读取，向健康端逐批发布 600×16384=9830400 字节；每一批等待健康端收到后再继续，逐字节断言完整，检查 SLOW_CLIENT 和受控 transport 仍连接。支持本次有界队列隔离及字节完整，不是无限速洪泛、任意网络故障或实体驱动存活证明。

[tests/integration/ApiServerTest.cpp:183–208](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/tests/integration/ApiServerTest.cpp#L183-L208) 的 **128** 用例只建立并保留 128 个连接，断言第 129 个明确拒绝、自动发现保留 CLIENT_LIMIT。它没有 128 订阅者的数据延迟、CPU、吞吐或公平性测量。**容量 128 绝不能写入性能通过栏；性能实测上限在本份日志中是 32 个接收者，且限定上述短时串行发布方式。**

## 对独立架构结论的增补

本轮压力使 A-02 的证据从“只有结构合理”增加到“32 接收者短测、loopback、单发布资源在本次观测下低于 1 ms 的 P99”。仍没有事先约定的性能预算与跨资源 replay 干扰试验，不能据此宣称所有负载达标或撤回 [ApiServer.cpp:535–575](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L535-L575) 全局锁内编码/复制的耦合事实。也不应反向用这把锁认定慢 socket 阻塞全局：sender 的 socket Send 在 [ApiServer.cpp:245–284](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/network/ApiServer.cpp#L245-L284) 独立线程和队列锁之外。

A-01 自动响应所有权与这些协议修复相互独立。[MainWindow.cpp:3933–3955](https://github.com/john2000smith7-del/SerialCtl/blob/5f12678e4da327f146edb5ce12eb3b3ea9c8afd0/src/app/MainWindow.cpp#L3933-L3955) 的本地/远端模型响应路由和历史回放副作用不会被纯 GatewayClient 压测覆盖；现有模型复现仍保留为生产模型实跑加源码完整路径，不能升级成两窗口实体 TX 抓包。A-03 与 SEC-03 指向同一日志导出职责问题，应合并修复计划而不是计作两个独立缺陷。

建议顺序保持：先确保满容量安全控制可入队并明确未知硬件状态；修正完整 envelope 消息边界；规定终端自动回复唯一所有者与回放无副作用；再做有生命周期/快照/取消保证的后台日志导出。保持 C++17/Win32、x64、/MT、6.01、YY-Thunks 和既定单设备 FIFO。Win7、真实串口/电源、慢盘故障注入和长时间测试仍列未运行，不因本次 14/14 CTest 或三方复审而降级标准。
