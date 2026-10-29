# SerialCtl 架构

SerialCtl 是原生 C++17/Win32 桌面应用。窗口线程负责 UI、会话状态与终端绘制，连接、网络推送、日志和 SFTP 操作通过工作线程执行。CMD 使用同一 serialctl.exe 的内部隐藏控制台桥接进程和持久 cmd.exe，整个子进程树由 Job Object 收尾；没有新增运行时或外部桥接程序。

## 模块

- `src/app`：程序入口、主窗口、主题、布局、对话框、会话编排、命令宏和 SFTP 交互。
- `src/connections`：统一连接接口、有界发送 FIFO，以及串口、WebSocket 远程串口、Telnet、SSH、CMD 子进程和 PuTTY 主机密钥处理。
- `src/network`：SessionService 内部会话服务、单一 WebSocket v1 网关、RFC 6455 拆帧与桌面客户端。GUI 和网络输入使用同一已有 transport；网络不创建连接。
- `src/terminal`：VT/xterm 状态机、主/备用屏幕、滚动历史、单元格属性与协议响应。
- `src/sftp`：PSFTP 批处理客户端、路径安全、列表解析、排序和传输进度。
- `src/logging`：有界异步会话日志、轮转、错误报告和日志副本保存。导出捕获接收字节截止点，等待对应写入完成时释放生产者锁，复制分卷的固定长度前缀；副本刷新成功后才原子替换目标。会话、电源日志和 CSV 统一使用 MainWindow 的单个后台导出任务，可在设置中取消，源会话断开后保留源日志所有权。退出先停止设备和连接，再取消/回收导出；自动更新安装将导出视为忙碌状态。
- `src/platform`：小型 Win32 编码与错误辅助函数。

## 主要数据流

```text
serial / socket / plink / CMD worker
        ↓ callback
MainWindow::PostData
        ├─ QueueReceived → WM_APP → UI thread → decoder → TerminalModel
        │                                                ├─ terminal paint
        │                                                ├─ logger → disk worker
        │                                                └─ scoped terminal response
        └─ EnqueuePublish → bounded publisher → per-client queue → sender
```

本地串口/CMD 的终端输入、粘贴、命令宏和网络输入直接进入 SessionService，再进入同一 QueuedConnection 有界 FIFO。原始串口字节不经文本解码；CMD 本地 Unicode 草稿按回车提交，网络也只提交完整命令行，代码页按真实控制台状态或本地显式编码选择处理。SSH/Telnet 保留各自的终端键盘规则。拖动期间批量移动控件，结束后提交最终终端网格；通过 Telnet NAWS 或定制 Plink 通道通知远端。

共享串口的终端自动回复仅由打开该COM的本地桌面模型负责。远程共享会话在创建、接收任何输出之前设为
TerminalModel::ResponseRole::Viewer；实时输出和历史回放均只解析显示，不生成DSR、DA或尺寸回复。
该所有权不随Clear、Reset、远程RIS、备用屏切换或Resize改变。SSH/Telnet仍各自拥有协议回复，
CMD保留原有不发送控制序列的提示。此约束只涉及合成回复，不过滤用户/AI主动发送的二进制输入，
不改变共享服务的认证边界或多人写入行为。

## 兼容边界

- 最低系统版本为 Windows 7 SP1。
- 生产构建使用 C++17、Unicode Win32、Winsock、Common Controls 和 GDI owner-draw。
- 生产程序不暴露可供其他工程链接的 Public SDK，因此当前没有 `include/` 目录。
- `legacy/csharp` 是只读历史参考，不属于该架构。

界面规则见 [UI-DESIGN-GUIDE.md](UI-DESIGN-GUIDE.md)，构建与交付规则见 [build-release.md](build-release.md)。

V1.0.6：SessionService 持有现有本地串口/CMD 的同一 QueuedConnection；GUI 直接输入，WebSocket 网关按资源 ID 输入。设备工作线程调用 PostData 时立即交接本地接收与网关发布，不等待 UI 绘制、日志写盘或网络分发。每客户端独立 sender condition_variable 与有界队列，资源历史有序并显式报告缺口。远程 SerialCtl 和 Linux Python 都使用同一 serialctl.v1 入口；旧 SerialShareService 与辅助发现已移除。协议见 WEBSOCKET-PROTOCOL.md。CMD 草稿由本地 Unicode 行编辑器保存，提交后进入同一 FIFO；stdout、输入来源与原始字节记录分离。主屏幕保留逻辑行、重排软折行及锚点，备用屏幕保持 VT 网格。

## V2.0.10 连接交互整改

串口和 SSH 的父进程读写分别使用可取消重叠 I/O，关闭先通知取消、等待完成再释放句柄；SSH 连接状态不再取得发送互斥量。MainWindow 将已注销会话的 transport/logger 交给拥有线程的 BackgroundCleanup 回收，窗口销毁前 Drain。设备枚举任务保留所有权，无 detach；电源 VISA/RS232 扫描分别调度，避免慢 VISA 阻塞串口列表。本次 V2.0.10 交付关闭 PowerService 功能开关，不执行电源枚举和连接，生产操作在排队前统一拒绝；独立测试宏仅允许内存仿真回归。

PostData 先投递本地接收，再将 WebSocket 发布交给独立、可停止且持有线程所有权的工作线程。输入也在发送工作线程唤醒前完成发布交接，保留输入/输出事件顺序；UI、串口读线程和发送锁不再执行 JSON/Base64 或等待网关全局锁。发布 FIFO 上限 8MiB/4096项，溢出明确关闭订阅并标记历史缺口，本地收发和日志继续；不默默丢弃后伪称完整。可见窗口用 steady_clock 的2ms预算处理每批至多32块、立即提交终端绘制，积压在下一次正常 WM_PAINT 机会继续，以保留输入优先级且不增加10ms接收定时等待。活动会话获得优先机会，每第四次仍处理全局队首，保持同一会话内的字节顺序。连接完成时的历史回放也进入同一批处理路径，放在已排队的更新数据之前。

CMD 私有控制台桥接进程为 stdout 批次附带输入/输出代码页元数据，父进程缓存并保持手动覆盖；元数据不进入外部 WebSocket 或日志字节。终端历史行和历史投影是不可变共享快照，输入预览缓存仅由 UI 线程使用，const 读取不构成多线程安全承诺。

TerminalViewport 负责终端原生绘制、GDI 缓冲和指针/选区，TerminalViewportController 负责网格、滚动和预览状态。CommandMacroController 拥有宏模型、文件持久化和运行计划；AiDeviceBridge 接收显式目标/上下文并拥有观察和设备策略。SftpController 持有传输队列、取消和两个后台线程；UI 只处理布局、对话框和可注销通知。

会话注册变更与输出发布共用有序的后台交接，避免窗口线程等待网关全局锁；队列溢出保留最新会话快照并明确标记历史缺口。关闭先发取消并保留对象与 HWND，后台完成连接、日志、网络、SFTP、电源及枚举回收后才销毁窗口。全部断开和连接完成也不会等待未结束的启动线程。

远程自动发现一次解析后并行探测既有端口。数字 IP 直接解析；域名通过同一 serialctl.exe 的受限私有解析进程处理，父进程控制超时、取消并回收子进程，以保留 Windows 7 支持；不会引入外部分发的解析程序。地址候选与启动协议阶段使用共同的单调时限。

真实硬件、完整 DPI 和性能矩阵仍需现场验证，逐项状态见 ../audit/V2.0.10-ARCH-FINAL-ACCEPTANCE.md。
