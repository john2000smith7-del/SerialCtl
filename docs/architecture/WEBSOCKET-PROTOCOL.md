# SerialCtl WebSocket 应用协议 v1

原生 Win7 SP1 Winsock + CryptoAPI 实现 RFC 6455；无 WinHTTP WebSocket、ConPTY、REST 或其他业务监听。启动即监听 7000–7015 的首个可用端口；全部占用只使网络服务失败，本地设备仍可用。容量上限 128 个同时连接（含握手），不代表 128 客户端负载测试已通过。

握手：`GET /serialctl HTTP/1.1`，RFC 6455 version 13，subprotocol `serialctl.v1`。发现是在同一入口完成握手，读取 hello 后发送 resources。客户端并行探测全部 16 端口；多个实例必须用 instance/port 明确选择。实例标识是每次服务启动的随机 128-bit hex，不跨重启复用。每次握手后重新核对实例；不自动重发输入。

所有业务是 UTF-8 JSON **文本 WebSocket 消息**。字节数据严格 Base64 编解码，串口允许 0–255，不做终端字符串编码或 Telnet 字节过滤。WebSocket binary opcode 被拒绝 1003，数据应用帧是带 resource/seq 的 JSON event。掩码、长度、RSV、UTF-8、分片/续帧、Ping/Pong/Close 由协议层处理；控制帧从不进入设备。旧 SERIALCTL/1、/2、/3、原始 TCP、旧 REST 收到 HTTP 426 升级错误，容量满为 HTTP 503 CLIENT_LIMIT。

hello：`{"type":"hello","version":1,"instance":"...","maxClients":128,"maxInput":65536,"maxMessage":131072,"historyBytes":1048576}`。

请求：`{"type":"request","version":1,"instance":"...","requestId":"unique-id","op":"input","resource":"session-3","params":{"data":"AAH/"}}`。
响应：同一 requestId，type=response，version/instance，ok=true + result；失败 ok=false + error={code,message}。请求 ID 长 1–128 字节；op 最长 32 字节；resource 最长 128 字节。拒绝未知字段、非法类型、未知 op、伪造/越权 resource，再调用设备。每连接缓存请求响应，最多 256 个且请求与响应 JSON 合计不超过 1 MiB（任一上限到达时淘汰最旧记录），重复相同请求不再执行，参数冲突返回 REQUEST_ID_CONFLICT。缓存不是跨重连输入幂等承诺。

| op | resource | params | 意义 |
|---|---|---|---|
| resources | 省略 | {} | 本机现有 local serial/CMD 和 power-1 |
| session.get | session-N | {} | 现有会话状态/编码 |
| input | session-N | {data:Base64} | 1–65536 原始输入字节；queued 与 execution unknown |
| subscribe | session-N | {after:非负整数，可省略默认0} | 重放剩余历史，再持续输出 |
| unsubscribe | session-N | {} | 取消后不再安排新事件；已在队列中的事件可能到达 |
| events | session-N | {after:非负整数} | 有界历史查询，过大返回 HISTORY_TOO_LARGE，改用 subscribe |
| power.get | power-1 | {} | 真实服务状态与本机勾选通道 |
| power.output | power-1 | {enabled:bool} | 仅操作 GUI 当时勾选通道；requestId 用于原有 action 幂等，应使用全局唯一 UUID |
| action.get | 省略 | {id:string} | 电源异步 queued/running/completed/failed/canceled 状态 |

不开放创建/断开连接、参数、SSH/SFTP、通道勾选、电压电流、保护、预设、模式、复位、SCPI、任务。服务无授权 UI/token，仅适合可信内网。GUI 不通过网络回连，直接调用相同 SessionService 和同一个 QueuedConnection。

每个 COM 只有一个本机驱动与内部会话 ID。完整输入批次 FIFO 原子入队，8 MiB 有界写队列；无写租约、无任务级互斥，多写入者的多步任务仍可能互相影响。input accepted 表示接收/排队，不保证执行完成。连接关闭不关闭本机设备、不取消已经入队的写入。超时/断线后执行状态未知，不自动重试可能已经执行的输入。

每连接最多订阅 64 个现有 serial/CMD 会话。每资源独立单调 seq、history 最多 1024 个 event/1 MiB，总缓存 16 MiB；订阅确认先排队，历史与实时事件在同一锁下排序，握手期间输出可通过 after=0 重放。资源事件 `{"type":"event","version":1,"instance":"...","resource":"session-N","seq":1,"kind":"output|input|status|error","source":"device|gui|network:N|connection","data":"Base64","tick":...}`。同一资源的顺序是网关 Publish 获取锁的顺序。不同资源不承诺共同时间线。

after 小于缓存最早序号-1（包括 after=0）、大于当前 seq，返回 HISTORY_GAP；禁止悄悄跳过。重连使用客户端已收到的最后 seq，同一 instance/session，先处理 gap 再决定是否以 session.get 当前状态开始新实时订阅（cursor 通过历史/订阅响应取得）。会话删除发送 resource_gone/TARGET_GONE，并取消订阅；ID 不在同一程序生命周期重复使用。

每客户端独立 4 MiB/4096 消息发送队列，事件到达直接唤醒 sender condition_variable，无 50/100ms 输出轮询。满队列明确终止 1013 SLOW_CLIENT，取消该客户端未发队列；3 秒发送超时保证回收。若 socket 已被堵住，Close 原因可能不能送达，诊断环形记录保留原因。客户端必须核对游标/重放缺口。header 8 KiB，握手接收超时 1.5 秒，业务消息/累计分片 128 KiB；JSON 嵌套最多 32 层；业务读取使用 Win7 重叠 WSARecv 与独立取消事件，取消/退出由事件唤醒并 CancelIoEx，等待 I/O 完成后才释放缓冲与 socket。正常 close=1000，服务器退出1001，协议1002，JSON文本类型1003，非法UTF8为1007，非法Base64为请求错误 INVALID_PARAMETERS（message明确INVALID_BASE64或NONCANONICAL_BASE64），过大1009；诊断保留最近512条阶段、来源、目标、关闭原因，终端右键可手动保存网络诊断JSON，并发日志路径不在设备读取热路径同步写盘。

CMD 是持久 cmd.exe /D /Q /K 与隐藏 Win7 控制台的管道 stdin/stdout，本地 Unicode 编辑器提交完整行，网络同样只接受完整 CR/LF 结束批次，统一为 CRLF，最长 8191 字节/行。管道输入的 Ctrl+C/方向序列/全屏交互在提交前拒绝。服务状态查询返回已读真实控制台代码页；GUI 默认 OEM，chcp 后查询实际 input/output code page，session.get 的 codePageSource 区分 console / oem-fallback / manual。应用已有 console 时不会擅自 detach，需在终端右键手动选择与CMD匹配编码；右键“CMD 实际代码页”恢复自动。手动选择仅是本地文本编码，不改变CMD的chcp或开放网络配置。非完整行、控制字符、超长行不部分执行。允许多行命令，外部命令仍可能读取后续管道输入；交互式应用不提供真实键盘控制。GUI 草稿未提交前不混入 stdout/日志，已提交输入显示并记录来源。
