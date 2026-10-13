# SerialCtl 架构

SerialCtl 是原生 C++17/Win32 桌面应用。窗口线程负责 UI、会话状态与终端绘制，连接、网络推送、日志和 SFTP 操作通过工作线程执行。CMD 使用同一 serialctl.exe 的内部隐藏控制台桥接进程和持久 cmd.exe，整个子进程树由 Job Object 收尾；没有新增运行时或外部桥接程序。

## 模块

- `src/app`：程序入口、主窗口、主题、布局、对话框、会话编排、命令宏和 SFTP 交互。
- `src/connections`：统一连接接口、有界发送 FIFO，以及串口、WebSocket 远程串口、Telnet、SSH、CMD 子进程和 PuTTY 主机密钥处理。
- `src/network`：SessionService 内部会话服务、单一 WebSocket v1 网关、RFC 6455 拆帧与桌面客户端。GUI 和网络输入使用同一已有 transport；网络不创建连接。
- `src/terminal`：VT/xterm 状态机、主/备用屏幕、滚动历史、单元格属性与协议响应。
- `src/sftp`：PSFTP 批处理客户端、路径安全、列表解析、排序和传输进度。
- `src/logging`：有界异步会话日志、轮转、错误报告和日志副本保存。
- `src/platform`：小型 Win32 编码与错误辅助函数。

## 主要数据流

```text
serial / socket / plink / CMD worker
        ↓ callback
MainWindow::PostData
        ├─ ApiServer::Publish → per-client bounded queue → event-woken sender
        └─ coalesced WM_APP → UI thread → incremental decoder → TerminalModel
                                                   ├─ terminal paint
                                                   ├─ async logger queue → disk worker
                                                   └─ scoped terminal response
```

本地串口/CMD 的终端输入、粘贴、命令宏和网络输入直接进入 SessionService，再进入同一 QueuedConnection 有界 FIFO。原始串口字节不经文本解码；CMD 本地 Unicode 草稿按回车提交，网络也只提交完整命令行，代码页按真实控制台状态或本地显式编码选择处理。SSH/Telnet 保留各自的终端键盘规则。拖动期间批量移动控件，结束后提交最终终端网格；通过 Telnet NAWS 或定制 Plink 通道通知远端。

## 兼容边界

- 最低系统版本为 Windows 7 SP1。
- 生产构建使用 C++17、Unicode Win32、Winsock、Common Controls 和 GDI owner-draw。
- 生产程序不暴露可供其他工程链接的 Public SDK，因此当前没有 `include/` 目录。
- `legacy/csharp` 是只读历史参考，不属于该架构。

界面规则见 [UI-DESIGN-GUIDE.md](UI-DESIGN-GUIDE.md)，构建与交付规则见 [build-release.md](build-release.md)。

V1.0.6：SessionService 持有现有本地串口/CMD 的同一 QueuedConnection；GUI 直接输入，WebSocket 网关按资源 ID 输入。设备工作线程调用 PostData 时立即 Publish，不等待 UI 绘制或日志写盘。每客户端独立 sender condition_variable 与有界队列，资源历史有序并显式报告缺口。远程 SerialCtl 和 Linux Python 都使用同一 serialctl.v1 入口；旧 SerialShareService 与辅助发现已移除。协议见 WEBSOCKET-PROTOCOL.md。CMD 草稿由本地 Unicode 行编辑器保存，提交后进入同一 FIFO；stdout、输入来源与原始字节记录分离。主屏幕保留逻辑行、重排软折行及锚点，备用屏幕保持 VT 网格。
