# AI V1 开发验证记录 · 2026-10-21

基线：`6e443fe7e6e06c807e9cf6b624961bc4687b2a71`。实现位于本地 `feature/ai-assistant-v1` 分支；此记录不是正式发布验收。

## 执行环境与范围

- Linux x64，LLVM-MinGW 20240619 / Clang 18.1.8，MSVCRT 静态交叉构建。
- x64 GUI PE 检查：AMD64、子系统 6.01。生产 CMake 的 MSVC `/MT` 和 YY-Thunks 路径没有改动。
- 交叉构建生成的 Ninja 文件仅在 `build/ai-cross` 内移除了无法与 MinGW ABI 链接的预编译 MSVC YY-Thunks 对象，并为 wmain 入口补充交叉链接选项。该构建只作开发检查，不能作为 Win7 正式包。
- Windows API 和原生控件测试运行于独立 Wine 8.0 / Xvfb 环境。截图式设计稿没有用于自动测试，UI 测试使用实际创建的 Win32 子窗口与合成消息。
- `scripts/check.ps1 -SourceOnly` 使用 PowerShell 7.4.6 执行通过；该 Linux 环境的脚本 Git provenance 检查未运行（没有 Windows git.exe）。实际基线由本机 git 命令读取并记录于上方。

## 结果

| 项目 | 结果 | 证据 |
| --- | --- | --- |
| 新增 ai_protocol / ai_client / ai_pane（含协议适配层端到端进程测试） | 3 项通过 | ai-tests.txt |
| 既有 UI、终端模型/解码、SFTP 模型、主机指纹、响应边界 | 6 项通过；该组历史运行另外包含 2 项新增 AI 测试 | selected-ui-model-tests.txt |
| 既有 API、共享串口、TCP、多串口、终端回复归属、Python API 客户端 | 6 项通过 | network-tests.txt |
| 既有电源服务、CMD 连接、共享串口背压 | 3 项通过 | other-tests-with-baseline-failure.txt |
| 既有 session_logger 只读目标用例 | Wine 中失败，不能记为通过 | other-tests-with-baseline-failure.txt |
| 从未修改的基线重新提取并编译 SessionLogger 及同一测试 | 同样在 readonly target protected 处失败 | unchanged-logger-baseline.txt |
| SourceOnly 版本/源码/依赖哈希 | 通过 | source-check.txt |
| 本环境 Codex CLI 0.163.0：initialize / model/list / thread/start + dynamicTools | 通过；模型列表返回 8 项；没有发起推理或设备操作 | codex-protocol-probe.json |

不同测试组分次运行，去重后共 18 个测试通过。日志测试的基线复现不构成本次通过，也未为 Wine 差异改写生产日志代码。

## 尚未验证

- MSVC 正式构建、Windows PE 导入 allowlist 与 YY-Thunks 链接后的正式检查。
- Windows 7 SP1 / 10 / 11、真实系统 DPI、中文 IME 和完整桌面鼠标/键盘交互验收。
- 用户真实 Linux SSH / Gateway / Codex App Server 的完整多轮推理、工具兼容性与断线恢复。
- 真实远程 IP 发现、实体串口读取/命令/设备回显、电源硬件与长时间性能。
- 更新客户端、更新安装器和 linux_gateway_client 没有纳入本次运行组。

没有创建发布包、替换 bin 内容、推送分支或发布 GitHub Release。
