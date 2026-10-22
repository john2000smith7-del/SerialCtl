# V2.0.2 AI 终端与电源接入验证

AI 页面选择已连接的五种终端，保留 kind/来源/编码和有界接收证据，CMD 走原 SessionService 并使用实际输入代码页、CRLF。新增三个电源工具，先读真实状态和 GUI 选择，只提交现有 PowerService 的输出 ON/OFF，返回 action 并查询回读，不开放参数和自动化。电源不依赖终端选择。

## 本地检查

- LLVM-MinGW x64 交叉编译通过；生产 MSVC、/MT、YY-Thunks 和子系统 6.01 配置保留。SourceOnly 版本/依赖检查通过，Linux git.exe 来源检查由 Windows 构建补足。
- Python 签名发布工具 24 项通过，见 [python-feed-tests.txt](python-feed-tests.txt)。
- 初次 Wine 运行中 ai_protocol、ai_client、power_service、tcp_connection 通过；旧 Xvfb 已退出，ai_pane/ai_device 无法创建窗口；CMD 进程测试 stage=3 失败。保留 [wine-initial-tests.txt](wine-initial-tests.txt)，不记为全部通过。
- 重启显示环境后，ai_pane 与 ai_device 均通过，见 [wine-gui-tests.txt](wine-gui-tests.txt)。后者直接调用 MainWindow 生产回调，五种终端使用受控 IConnection，电源使用真实勾选控件和现有模拟后端，测试目标/日志/编码/换行、断线、ON/OFF 回读和选中通道改变；前者覆盖不选终端的电源工具、先查询、直接调用、去重、channels 越权及停止。

- 恢复 Xvfb 后，原 CMD 真实进程测试也通过，见 [wine-cmd-recheck.txt](wine-cmd-recheck.txt)。初次 GUI/CMD 失败与显示环境退出相关；保留原记录，不抹除先前失败。
- 已检查实际生产 EXE 的[浅色](wine-light.png)、[深色](wine-dark.png)和[最小窗口](wine-minimum.png)截图，界面沿用既有布局。截图来自 Wine/Xvfb 可见客户区，不是设计重绘或真实 Windows 实机。

首次 Windows 构建 [39743744951](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39743744951) 的 24 项测试全部通过（含真实 CMD 和 ai_device），但新增测试程序的精确 DLL 导入清单不匹配，发布门槛失败，未签名或发布该候选。按实际导入补入 COMDLG32/GDI32/OLE32/SHELL32 并去除未使用的 WINHTTP，保留精确集合与 Win7 检查，重跑正式构建。

正式 Windows 构建、发布签名和独立回读结果随后记录。未验证真实 SSH/Telnet/Gateway、实体电源/串口、用户更新网站或 Win7/10/11 实机。
