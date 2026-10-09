# SerialCtl 架构

SerialCtl 是单进程原生 Win32 桌面应用。窗口线程负责 UI、会话状态与终端绘制，连接和 SFTP 操作通过工作线程执行，再使用窗口消息把数据交回 UI 线程。

## 模块

- `src/app`：程序入口、主窗口、主题、布局、对话框、会话编排、命令宏和 SFTP 交互。
- `src/connections`：统一连接接口以及串口、共享串口、Telnet、SSH、子进程和 PuTTY 主机密钥处理。
- `src/terminal`：VT/xterm 状态机、主/备用屏幕、滚动历史、单元格属性与协议响应。
- `src/sftp`：PSFTP 批处理客户端、路径安全、列表解析、排序和传输进度。
- `src/logging`：自动会话日志和日志副本保存。
- `src/platform`：小型 Win32 编码与错误辅助函数。

## 主要数据流

```text
serial / socket / plink worker
        ↓ callback
MainWindow::PostData (WM_APP)
        ↓ UI thread
decode → TerminalModel::Feed
        ├─ render terminal
        ├─ write session log
        └─ send required terminal response
```

发送方向来自终端键盘、粘贴或命令宏，经当前会话编码与换行规则转换后交给 `IConnection::Send`。窗口尺寸变化同步到终端模型，并通过 Telnet NAWS 或定制 Plink 通道通知远端。

## 兼容边界

- 最低系统版本为 Windows 7 SP1。
- 生产构建使用 C++17、Unicode Win32、Winsock、Common Controls 和 GDI owner-draw。
- 生产程序不暴露可供其他工程链接的 Public SDK，因此当前没有 `include/` 目录。
- `legacy/csharp` 是只读历史参考，不属于该架构。

界面规则见 [UI-DESIGN-GUIDE.md](UI-DESIGN-GUIDE.md)，构建与交付规则见 [build-release.md](build-release.md)。

V1.1.0：SerialShareConnection 为每个 COM 持有独立 SerialDevice，进程共享 SerialShareService 按 COM 路由 TCP 客户端。每个客户端独立有界发送队列，关闭一个 COM 不影响其他 COM。查询仍使用 V1 LIST，终止标记后扩展元数据；OPEN 使用 V2 二进制帧。IP 发现由后台工作线程并行探测 7000–7015，UI 只接受当前查询代次结果。SFTP OSC7 在终端模型解析，再按会话 follow 状态同步；手动导航暂停 follow。命令编辑为草稿，显式保存采用临时文件替换。
