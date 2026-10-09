# V1.0.1 人工回归清单

本清单是待执行步骤，不是通过证明。

- 深色和浅色：主窗口、SSH/串口/Telnet/共享串口连接对话框、按钮状态和缩放。
- 连接过程中操作已有会话，关闭窗口，检查无死锁；不可达地址不能冻结主窗口。
- SSH 首次确认指纹、拒绝、已保存主机、密钥变化；不可仅从当前连接本身核对身份。
- GBK 输入“中文”分别整包、逐字节、和 ASCII 混合发送，终端与日志均正确。
- 终端异常/超大 ANSI 参数、备用屏幕和粘贴不造成崩溃；UTF-8 和常用宏无退化。
- 共享串口握手后立即输出，首字节不丢失；慢客户端隔离、断开与重连。
- SFTP 上传和单/多文件下载到同名目标，确认与取消覆盖均有效；取消后保留原文件。
- 日志保存、宏停止、会话切换、程序退出。

CI 会启动 x86/x64 包中程序并检查深色/浅色主窗口与 SSH 提示；真实 Win7、硬件和现场服务器仍按 WIN7-HARDWARE-TEST.md 验收。
# V1.0.2 UI regression

- In both themes, inspect complete Chinese glyphs in SSH, serial, Telnet, shared serial, command/macro and SFTP input dialogs; repeat at 100%, 125% and 150% DPI on Windows 7 SP1.
- At the minimum window size and 260 px panel width, verify that tab, add-command and collapse actions do not overlap.
- Collapse the Commands and SFTP panels; only the centered expand button may remain in the 48 px rail. Resize, switch sessions and expand again; no panel control may escape the card.
- CI checks both architectures and themes using real windows, label extents and control bounds. SFTP UI uses a test-only local mock. Real Win7/DPI/hardware acceptance remains not run.

