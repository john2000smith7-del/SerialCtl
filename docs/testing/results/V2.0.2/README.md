# V2.0.2 AI 终端与电源接入验证

AI 页面选择已连接的五种终端，保留 kind/来源/编码和有界接收证据，CMD 走原 SessionService 并使用实际输入代码页、CRLF。新增三个电源工具，先读真实状态和 GUI 选择，只提交现有 PowerService 的输出 ON/OFF，返回 action 并查询回读，不开放参数和自动化。电源不依赖终端选择。

## 本地检查

- LLVM-MinGW x64 交叉编译通过；生产 MSVC、/MT、YY-Thunks 和子系统 6.01 配置保留。SourceOnly 版本/依赖检查通过，Linux git.exe 来源检查由 Windows 构建补足。
- Python 签名发布工具 24 项通过，见 [python-feed-tests.txt](python-feed-tests.txt)。
- 初次 Wine 运行中 ai_protocol、ai_client、power_service、tcp_connection 通过；旧 Xvfb 已退出，ai_pane/ai_device 无法创建窗口；CMD 进程测试 stage=3 失败。保留 [wine-initial-tests.txt](wine-initial-tests.txt)，不记为全部通过。
- 重启显示环境后，ai_pane 与 ai_device 均通过，见 [wine-gui-tests.txt](wine-gui-tests.txt)。后者直接调用 MainWindow 生产回调，五种终端使用受控 IConnection，电源使用真实勾选控件和现有模拟后端，测试目标/日志/编码/换行、断线、ON/OFF 回读和选中通道改变；前者覆盖不选终端的电源工具、先查询、直接调用、去重、channels 越权及停止。

正式 Windows 构建、发布签名和独立回读结果随后记录。未验证真实 SSH/Telnet/Gateway、实体电源/串口、用户更新网站或 Win7/10/11 实机。
