# V2.0.2 r2 验证记录

产品版本保持 V2.0.2。本次增加可展开、可调宽度的 AI 实时终端侧栏，复用实际终端 HWND/SessionState；侧栏偏好独立于命令/SFTP。AI 复用柔和主题色，模型只读使用服务器默认配置。连接可指定 Linux 项目目录，原生服务器工具提供代码读写、命令输出、退出码及 diff，设备与服务器操作通道保持明确。

初轮 LLVM-MinGW x64 交叉构建与 Wine/Xvfb 的 ai_protocol、ai_client、ai_pane、ai_device、ui_buffered_paint、ui_combo_paint、cmd_connection 七项通过，见 [wine-targeted-tests.txt](wine-targeted-tests.txt)。新增测试使用实际 MainWindow/AI/终端控件，设备传输为受控替身，电源为现有模拟后端。SourceOnly 版本/依赖检查通过；Linux 下 Git 来源检查由正式 Windows 构建补足。签名更新工具 24 项通过。

最终补充原生工具完成事件缺少汇总输出时保留实际流式日志的回归，以及窄窗口欢迎文案换行。正式构建、打包及发布回读证据在发布后补齐；本记录不把初轮通过等同于最终候选已发布。

本地截图来自实际生产 EXE 在 Wine/Xvfb 的可见客户区，仅裁剪窗口范围，无设计重绘或模拟 AI 回复。右侧输出来自真实 CMD 进程。Wine 的窗口装饰不代表 Windows 原生标题栏。

未运行真实 Windows 7/10/11 实机、多 DPI、现场 SSH/Gateway、服务器项目写入及实体串口/电源长时间验收，不宣称这些通过。正式 Windows Server 2022 CI 和离线工具事件不替代现场验收。

同版本修订包使用 v2.0.2-r2 标签，保留原 v2.0.2 及其资产。自动更新仍只接受更高产品版本，已安装 V2.0.2 需手动替换一次。
