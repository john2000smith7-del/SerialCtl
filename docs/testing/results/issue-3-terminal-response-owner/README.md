# Issue #3：共享终端自动回复所有权

基线为main `9e70765`，V1.0.8。本项定义打开COM的本地桌面为终端自动回复所有者；远程共享窗口
在连接开始、回放任何历史之前，将模型设为Viewer。Viewer解析和显示输出，但不生成DSR、DA、尺寸回复。
SSH/Telnet默认Owner及CMD现有提示保留；不修改共享手动输入、AI权限、二进制传输或多人并发策略。

## 本地已运行的证据

- 使用基线原样TerminalModel编译历史复现脚本：本地80×24生成27字节，远程100×30生成28字节，
  历史回放再次生成28字节。before-model.log中的PASS表示成功复现旧缺陷。
- 修复模型的after-probe.cpp：Owner仍为27字节；两个不同尺寸Viewer分别为0字节；Resize、Clear、Reset、
  远程RIS后回放仍为0字节。after-model.log保留实际输出。
- 完整现行TerminalModelTest已在Linux g++ C++17编译并返回0，见unit-model.log；新增覆盖两Viewer、
  拆包、重置、备用屏、日志文本、SSH/Telnet独立几何和光标响应。
- 移除三个所有权判断的受控变异构建返回1，8条断言失败，见removed-guards-model.log。
  它用于确认回归断言会捕获原行为，不等同于未修改基线运行现行测试。

复跑本地模型：

```sh
g++ -std=c++17 -O2 -Isrc/terminal -Isrc/app tests/unit/TerminalModelTest.cpp src/terminal/TerminalModel.cpp -o /tmp/terminal-model
/tmp/terminal-model
g++ -std=c++17 -O2 -Isrc/terminal docs/testing/results/issue-3-terminal-response-owner/after-probe.cpp src/terminal/TerminalModel.cpp -o /tmp/terminal-owner
/tmp/terminal-owner
```

## 原生Windows控制场景

新增CTest shared_terminal_response。它使用生产ApiServer、SessionService、QueuedConnection、
SharedSerialConnection和TerminalModel，通过回环WebSocket连接两个观看客户端及两个AI客户端，
以显式RX、记录TX的受控COM替身检查：

1. 无远端时仅本地Owner回复；一个观看客户端回放不新增回复。
2. 两个不同尺寸观看客户端和两个AI同时在线，实时查询仅有80×24所有者的一份准确回复。
3. 调整观看模型尺寸、Clear、Reset、重连后，历史中的两份查询不新增回复。
4. 手动标记跟在该客户端所有可能自动回复之后，经相同socket和设备FIFO落到记录端，
   再严格比较完整TX；不以短暂sleep未观察到额外字节作为通过依据。
5. 两AI及两观看客户端的0–255、FF、CRLF和显式查询字节完整进入设备，证明没有用字节过滤掩盖问题。

Windows结果：**待运行**。标准构建入口为scripts/build.ps1、scripts/test.ps1、scripts/check.ps1，
本次配置共18项CTest。当前会话无法通过api.github.com下载原始Actions日志/产物，不能虚构原始测试日志。

## 未运行范围

真实双Windows桌面操作、实体UART TX抓包、Win7真机、真实SSH/Telnet服务器及长期运行：**未运行**。
上述控制客户端不是Win32窗口或物理串口。固定PuTTY、NI-VISA输入、/MT、PE6.01、YY-Thunks和最低平台未更改。
所有者UI线程阻塞时回复仍可能延迟，这属于后续会话职责优化，不在本项改成后台终端解析器。
