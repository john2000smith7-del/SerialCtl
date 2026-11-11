# SerialCtl MCP 功能能力矩阵 (MCP Capability Matrix)

本文档建立 SerialCtl 桌面 GUI 人工操作功能、底层业务服务、MCP 工具接口 (JSON-RPC 2.0)、权限边界（允许/硬禁止）与自动化测试覆盖的完整机器可审计对照矩阵。

## 1. 权限边界与安全规则

1. **免认证 IP 直连 (Streamable HTTP `POST /mcp`, 默认端口 7080)**：
   - 本机用户明确启用后，任何网络可达客户端均可免 Token/密码/配对直接调用 `initialize`、`tools/list`、`tools/call`。
   - 所有调用均记录内存审计日志（时间戳、客户端 IP、工具名、目标会话、结果码，密码与凭据自动脱敏）。
2. **受控电源硬白名单与黑名单 (Power Hard Boundary)**：
   - **允许白名单**：
     - `power.get`：只读连接、测量值、输出状态、GUI 当前人工选定通道、任务状态。
     - `power.output(enabled, reason)`：**仅允许操作 GUI 当前人工勾选的通道**（且本地 Test 已解锁）；客户端不得通过参数修改或覆盖 `channels`。
     - `power.action(id)`：只读查询电源异步任务进度。
     - `power.recover`：仅在用户已配置并绑定恢复通道时执行上下电恢复。
   - **硬性禁止黑名单（服务端统一硬拦截，返回 `POWER_PARAMETER_CHANGE_FORBIDDEN` / `PERMISSION_DENIED`，零设备写入副作用）**：
     - 严禁设置/步进电压 (`voltage`)、电流 (`current`)、过压/过流保护 (`ovp`/`ocp`)、功率限制；
     - 严禁修改工作模式 (`mode`：串联/并联/跟踪)；
     - 严禁通过参数更改通道勾选 (`channels`)；
     - 严禁修改预设、校准、复位 (`reset`)、发送原始 SCPI 指令 (`scpi`/`raw`)。
3. **机密脱敏与防递归**：
   - `ssh_history.list` 严禁读取或回显已保存的 SSH 明文密码。
   - 首次/变更 SSH host key 若未信任则结构化返回指纹并要求显式 `confirmHostKey=true`，不无条件盲信。
   - `ai.send` 包含调用深度限制（`recursionDepth >= 2` 拦截），防止“外部 MCP 调内置 AI → 内置 AI 又调外部 MCP”的无限递归。
4. **GUI 生命周期（#52 已取消）**：
   - SerialCtl 仅手动启动；MCP 默认关闭，用户本机启用后在 GUI 存活期间提供服务。
   - GUI 退出后 HTTP/TFTP 停止，无开机自启或独立常驻后台服务。STDIO 只桥接现有 GUI。
   - 下表为源码接口映射，不是全部业务已经端到端验证的证明。尤其 TestBusinessFlow 使用模拟 executor；验收结果以本轮精确 SHA 的原始证据和逐 Issue 报告为准。

---

## 2. 功能覆盖矩阵表格

| 功能分类 | UI 控件 / 人工动作 | 内部业务服务 / 方法 | MCP Tool 名称 | 参数与确认要求 | 权限边界 | 自动化测试覆盖 |
|---|---|---|---|---|---|---|
| **应用与服务** | 主窗口标题栏 / 运行状态 | `MainWindow::State`, `GuiInstance` | `app.status` | 无 | 允许（公开只读） | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | 进程健康心跳 | `McpService::Health` | `app.health` | 无 | 允许（公开只读） | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | 激活已运行 GUI（不恢复常驻） | `McpService::LaunchGui`, `GuiInstance` | `app.launch` | `forceFocus` (bool, 可选) | 允许（受 Session 0 隔离保护） | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | 激活主窗口至前台 | `GuiInstance::Activate` | `app.activate` | 无 | 允许 | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | 退出主界面 | `MainWindow::Close` | `app.quit` | `confirm: true` (必须显式确认) | 允许（需确认） | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | MCP 服务配置状态 | `McpService::Status` | `app.service_status` | 无 | 允许（只读） | `McpServerTest::TestResidentLifecycle` |
| **应用与服务** | 配置下次启动的监听地址与端口 | `McpService::Configure` | `app.service_configure` | `port` (int), `bindIp` (IPv4)；autoStart 拒绝 | 允许，需手动重启生效 | `McpServerTest::TestResidentLifecycle` |
| **会话管理** | 左侧会话卡片列表 | `MainWindow::sessions_` | `session.list` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 串口下拉框与网络发现 | `EnumComPorts`, `NetworkConnect` | `session.ports` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 新建串口连接 | `MainWindow::ConnectSerial` | `session.connect_serial` | `port` (str), `baudRate` (int), `parity`, `dataBits`, `stopBits` | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 新建远程共享串口 | `MainWindow::ConnectShared` | `session.connect_shared_serial` | `host` (str), `port` (int), `instance` (str, 可选), `remoteCom` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 新建 SSH 连接 | `MainWindow::ConnectSsh` | `session.connect_ssh` | `host` (str), `port` (int), `username` (str), `password` (str, 可选), `confirmHostKey` (bool) | 允许（未信任密钥需指纹显式确认） | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 新建 Telnet 连接 | `MainWindow::ConnectTelnet` | `session.connect_telnet` | `host` (str), `port` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 新建本地 CMD 会话 | `MainWindow::ConnectCmd` | `session.connect_cmd` | `command` (str, 可选) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 点击左侧卡片切换会话 | `MainWindow::SelectSession` | `session.switch` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 顶部重连按钮 / 卡片重连 | `MainWindow::ReconnectSession` | `session.reconnect` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 顶部断开按钮 / 卡片断开 | `MainWindow::DisconnectSession` | `session.disconnect` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 卡片关闭按钮 (×) | `MainWindow::CloseSession` | `session.close` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **会话管理** | 底部“全部断开”按钮 | `MainWindow::DisconnectAll` | `session.disconnect_all` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **SSH 历史** | SSH 对话框“历史连接”下拉 | `SshConnectionHistory::List` | `ssh_history.list` | 无 | 允许（**严禁回显明文密码**） | `McpServerTest::TestPasswordMasking` |
| **SSH 历史** | 历史连接下拉旁“删除”按钮 | `SshConnectionHistory::Forget` | `ssh_history.delete` | `host` (str), `username` (str), `port` (int) | 允许 | `McpServerTest::TestPasswordMasking` |
| **终端交互** | 终端屏幕主显示区 | `TerminalModel::ScreenText` | `terminal.screen` | `sessionId` (str) | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 接收数据增量读取 | `AiDeviceBridge::Read` | `terminal.read` | `sessionId` (str), `after` (int), `lines` (int) | 允许（只读分页） | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 底部输入栏发送文本 | `AiDeviceBridge::Send` | `terminal.send_text` | `sessionId` (str), `text` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 发送原始十六进制字节 | `IConnection::Send` | `terminal.send_bytes` | `sessionId` (str), `hex` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 键盘功能键 / Ctrl+C / Enter | `TerminalModel::KeySequence` | `terminal.send_key` | `sessionId` (str), `key` (str: Enter, Ctrl+C, Escape, Tab, etc.) | 允许 | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 发送完整命令并等待 | `AiDeviceBridge::Command` | `terminal.command` | `sessionId` (str), `command` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 光标与接收状态观测 | `AiDeviceBridge::Observe` | `terminal.observe` | `sessionId` (str) | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 右键菜单“清空终端” | `TerminalModel::Clear` | `terminal.clear` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **终端交互** | 设置编码/行尾/时间戳 | `MainWindow::SetTerminalSettings` | `terminal.settings` | `sessionId` (str), `codePage` (int), `lineEnding` (int), `echo` (bool), `timestamp` (bool), `rawTrace` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **U-Boot 打断** | 顶部 U-Boot 打断 / 倒计时捕捉 | `BootInterrupt::PreparePreset` | `boot.enter` | `sessionId` (str), `preset` (str, 可选), `timeoutSeconds` (int, 可选) | 允许 | `McpServerTest::TestBusinessFlow` |
| **U-Boot 打断** | 停止 U-Boot 打断循环 | `BootInterrupt::Cancel` | `boot.stop` | `sessionId` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **U-Boot 打断** | 查询打断状态与提示符 | `BootInterrupt::Status` | `boot.status` | `sessionId` (str) | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 右侧“命令”选项卡列表 | `CommandMacroController::Items` | `macro.list` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 笔记本文本框编辑 | `CommandNotebook::SetText` | `macro.set_document` | `text` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 添加宏步骤 | `CommandMacroController::Append` | `macro.add` | `name` (str), `commands` (list of str), `delayMs` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 修改宏步骤 | `CommandMacroController::Update` | `macro.update` | `index` (int), `name` (str), `commands` (list of str), `delayMs` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 删除宏步骤 | `CommandMacroController::Erase` | `macro.delete` | `index` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 运行宏 / 单条执行 | `MainWindow::RunNotebook` | `macro.run` | `index` (int, 可选), `sessionId` (str, 可选) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 红色暂停图标 / 中止运行 | `MainWindow::StopCommandSequence` | `macro.stop` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 查询宏运行进度 | `CommandMacroController::Running` | `macro.status` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 导入命令脚本 | `MainWindow::ImportCommandFile` | `macro.import_file` | `path` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **宏与笔记本** | 导出命令脚本 | `MainWindow::ExportCommandFile` | `macro.export_file` | `path` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 右侧“SFTP”文件列表 | `SftpController::List` | `sftp.list` | `sessionId` (str, 可选), `path` (str, 可选) | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 双击目录 / 切换路径 | `SftpController::Navigate` | `sftp.cd` | `sessionId` (str, 可选), `path` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 新建文件夹 | `SftpController::MakeDirectory` | `sftp.mkdir` | `sessionId` (str, 可选), `remotePath` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 重命名文件/目录 | `SftpController::Rename` | `sftp.rename` | `sessionId` (str, 可选), `oldPath` (str), `newPath` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 右键删除文件/目录 | `SftpController::Delete` | `sftp.delete` | `sessionId` (str, 可选), `remotePath` (str), `confirm: true` (必须显式确认) | 允许（需确认） | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 修改权限 (chmod) | `SftpController::Chmod` | `sftp.chmod` | `sessionId` (str, 可选), `remotePath` (str), `mode` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 上传本地文件到远端 | `SftpController::Upload` | `sftp.upload` | `sessionId` (str, 可选), `localPath` (str), `remotePath` (str), `allowOverwrite` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 下载远端文件到本地 | `SftpController::Download` | `sftp.download` | `sessionId` (str, 可选), `remotePath` (str), `localPath` (str), `allowOverwrite` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 传输任务队列与进度 | `SftpController::Transfers` | `sftp.transfers` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 取消传输任务 | `SftpController::CancelTransfer` | `sftp.cancel_transfer` | `transferId` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 重试失败任务 | `SftpController::RetryTransfer` | `sftp.retry_transfer` | `transferId` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **SFTP 文件** | 清除已完成任务 | `SftpController::ClearTransfers` | `sftp.clear_transfers` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **串口传输 Xfer** | 右侧“Xfer”面板状态 | `TransferController::State` | `xfer.status` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **串口传输 Xfer** | 开始 Raw/X/Y/ZMODEM 传输 | `TransferController::Start` | `xfer.start` | `sessionId` (str), `protocol` ("raw"/"xmodem"/"ymodem"/"zmodem"), `direction` ("send"/"receive"), `filePath` (str), `expectedSize` (int, 可选) | 允许 | `McpServerTest::TestBusinessFlow` |
| **串口传输 Xfer** | 手动结束 Raw 接收 | `TransferController::FinishRaw` | `xfer.finish_raw` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **串口传输 Xfer** | 取消文件传输 | `TransferController::Cancel` | `xfer.cancel` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 右侧“TFTP”服务状态 | `TftpServer::State` | `tftp.status` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 启动 TFTP 服务 | `TftpServer::Start` | `tftp.start` | `bindIp` (str, 可选), `port` (int, 可选) | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 停止 TFTP 服务 | `TftpServer::Stop` | `tftp.stop` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 发布本地文件 | `TftpServer::PublishLocal` | `tftp.publish_local` | `tftpName` (str), `localPath` (str), `allowOverwrite` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 远程流式发布 | `TftpServer::PublishRemote` | `tftp.publish_remote` | `tftpName` (str), `remotePath` (str), `sessionId` (str, 可选), `allowOverwrite` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 取消发布文件 | `TftpServer::Unpublish` | `tftp.unpublish` | `tftpName` (str) | 允许 | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 已发布文件清单 | `TftpServer::ListPublished` | `tftp.list` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 活跃传输与下载进度 | `TftpServer::ListTransfers` | `tftp.transfers` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **TFTP 服务** | 取消指定客户端下载 | `TftpServer::CancelTransfer` | `tftp.cancel_transfer` | `transferId` (int) | 允许 | `McpServerTest::TestBusinessFlow` |
| **日志与诊断** | 会话日志记录状态 | `SessionLogger::Status` | `log.status` | `sessionId` (str, 可选) | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **日志与诊断** | 异步导出日志 | `MainWindow::StartLogExport` | `log.export` | `path` (str), `csv` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **日志与诊断** | 取消日志导出 | `MainWindow::CancelLogExport` | `log.cancel_export` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **日志与诊断** | 网络诊断信息 | `MainWindow::NetworkDiagnostics` | `log.network_diagnostics` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **界面与主题** | 界面布局与当前面板 | `MainWindow::UiStatus` | `ui.status` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **界面与主题** | 浅色/深色主题切换 | `MainWindow::ApplyTheme` | `ui.theme` | `dark` (bool) | 允许 | `McpServerTest::TestBusinessFlow` |
| **界面与主题** | 切换右侧栏选项卡 | `MainWindow::ShowPanel` | `ui.panel` | `name` ("commands"/"sftp"/"xfer"/"tftp") | 允许 | `McpServerTest::TestBusinessFlow` |
| **内置 AI** | AI 对话状态与模型 | `AiPane::Status` | `ai.status` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **内置 AI** | AI 工作空间指示 | `WorkspaceInstructions::Text` | `ai.workspace` | 无 | 允许（只读） | `McpServerTest::TestBusinessFlow` |
| **内置 AI** | 提交 AI 对话提示词 | `AiPane::SendPrompt` | `ai.send` | `prompt` (str), `recursionDepth` (int, 可选) | 允许（**深度 >= 2 拦截**） | `McpServerTest::TestAiRecursionGuard` |
| **内置 AI** | 停止 AI 回复生成 | `AiPane::Stop` | `ai.stop` | 无 | 允许 | `McpServerTest::TestBusinessFlow` |
| **受控电源** | 状态/测量/已选通道查询 | `PowerService::State` | `power.get` | 无 | **白名单允许（只读）** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 通道输出开关 | `PowerService::Submit("output")` | `power.output` | `enabled` (bool), `reason` (str, 可选) | **白名单允许（仅作用于 GUI 已选通道）** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 动作结果查询 | `PowerService::Action` | `power.action` | `id` (str) | **白名单允许（只读）** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 绑定恢复上下电循环 | `PowerService::Submit("recover")` | `power.recover` | 无 | **白名单允许（需预先绑定）** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 修改设定电压 | `PowerService::SetParameters` | *(硬拒绝)* | `voltage` (任何值) | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 修改设定电流 | `PowerService::SetParameters` | *(硬拒绝)* | `current` (任何值) | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 设置过压/过流保护 | `PowerService::Extended` | *(硬拒绝)* | `ovp` / `ocp` | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 修改工作模式 (串/并/跟踪) | `PowerService::ChangeMode` | *(硬拒绝)* | `mode` ("series"/"parallel"/etc.) | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 篡改/覆盖操作通道 | `PowerService::Outputs` | *(硬拒绝)* | `channels` (非 GUI 勾选列表) | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 发送原始 SCPI 调试命令 | `PowerService::Command` | *(硬拒绝)* | `scpi` / `raw` / `line` | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |
| **受控电源** | 预设存储/召回/设备复位 | `PowerService::Extended` | *(硬拒绝)* | `preset` / `reset` / `calibrate` | **硬性禁止：POWER_PARAMETER_CHANGE_FORBIDDEN** | `McpServerTest::TestPowerWhitelistAndRejections` |

---

## 3. 错误码规范

| 错误码常量 | 数值 | 描述 |
|---|---|---|
| `POWER_PARAMETER_CHANGE_FORBIDDEN` | -32001 | 尝试修改受保护的电源参数（电压、电流、保护、模式、通道篡改、原始 SCPI 等） |
| `APP_NOT_RUNNING` | -32002 | SerialCtl GUI 主窗口未运行，请先调用 `app.launch` |
| `LOGIN_REQUIRED` | -32003 | 当前处于 Windows Session 0 或无交互登录会话，禁止在无桌面环境下创建窗口 |
| `CONFIRMATION_REQUIRED` | -32004 | 危险操作（如 `app.quit`, `sftp.delete`）缺少 `confirm: true` 显式确认 |
| `AI_RECURSION_LIMIT_EXCEEDED` | -32005 | 外部 MCP 调内置 AI 超过最大递归深度限制 |
| `HOST_KEY_UNTRUSTED` | -32006 | SSH 主机公钥未受信任，返回指纹并要求显式确认 |
| `PERMISSION_DENIED` | -32007 | 权限不足或未解锁本地密码 Test 保护 |
