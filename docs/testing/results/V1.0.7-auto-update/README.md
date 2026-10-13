# V1.0.7 自动更新实际验证记录

程序与自动化脚本的实际测试提交为 `2dbc8a3e797a04cf63901f5c7ccbb73da46c9923`，分支 `feat/intranet-auto-update`，VERSION 两项均为 V1.0.7。本目录随后作为纯证据文档提交，不改变该提交的生产代码、测试或构建配置。基于未合并的 `fix/unified-websocket-win7` / `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`；main 未改动。

[Windows 流水线 38315844534](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38315844534) 全部成功。环境为 GitHub `windows-2022`、Windows Server 2022 10.0.20348、4 vCPU、约 16 GiB 内存、1920×1080、96 DPI。使用仓库 CMake/MSVC Release 入口，保持 C++17、x64、/MT、PE 6.01、YY-Thunks 1.2.1、固定 PuTTY 0.85 与 NI-VISA 18。保留记录未包含完整 MSVC 补丁号，不自行推断。

| 运行文件 | SHA-256 |
|---|---|
| SerialCtl-V1.0.7-Win7-x64.zip | `72dc0926074b5968f5dd2f7fcd5b5e16e9c26e1d7dd2ca0627e1f072a7826f7d` |
| serialctl.exe | `c6cfdcb844d4c4eccc117b3384264e0ed651f61089a32fac0b2c863ec9b4b183` |

原始 [release transcript](evidence/serialctl-build.txt)、[16 项 CTest 日志](evidence/ctest-x64.txt)、[桌面环境](evidence/ui-environment.json)、[GUI 结果](evidence/updater-gui.json)、[HTTP 请求](evidence/updater-http.log) 与[证据摘要](evidence/SHA256SUMS.txt) 保存在本目录。完整 CI 截图集和便携包在上述流水线的 artifacts；有保存期限，请按需下载。

## 结果及证明范围

| 项目 | 结果 | 证据与边界 |
|---|---|---|
| SourceOnly / Clean Build / Test / PE 与依赖检查 / ZIP 重开检查 | 通过 | release.ps1 完整执行；不等于 Win7 实机通过 |
| CTest | 16/16 通过 | 原有 14 项及新客户端、安装器集成测试 |
| Python AI 协议、签名发布工具回归 | 通过 | 流水线两步骤成功；签名工具包含 16 项用例 |
| 签名、白名单、路径、长度、版本、SHA-256、4 MiB 全字节负载 | 通过 | 客户端生产逻辑 + 临时测试密钥、loopback HTTP；不是真实内网 |
| 并发取消隔离 | 通过 | A 正在等待 body，B 正在下载签名 payload；取消 A 后 B 仍 Ready，暂存与原程序摘要正确 |
| 安装、回滚、锁、多实例、版本不符、未授权 helper、恢复记录 | 通过 | 实际 Win32 文件和进程操作；替换应用是测试窗口程序、临时信任根，不是正式 GUI 的生产签名正向升级 |
| 正式 GUI 更新异常流程 | 6/6 通过 | 缺失/空 INI、A 地址 404、B 地址伪签名、慢头、慢 body；实际鼠标注入、键盘、CopyFromScreen |
| 异常更新期间真实 CMD | 通过 | 404 与慢 body 场景均执行命令并生成正确 marker 文件；命令为 ASCII，不证明中文专项 |
| GUI、INI 与原文件保留 | 通过 | 6 例 exit code 0；原 exe/PuTTY/VERSION 摘要不变，配置前后摘要一致 |
| 完整正式包生成签名更新目录 | 通过 | 管理员侧工具在 Linux 对真实上述 ZIP 验证、生成并复验签名；23 文件、4,181,165 字节；不含私钥、INI 或驱动 |
| 正式 GUI 生产签名成功升级、Win7、真实服务器、设备、掉电、8 小时 | 未运行 | 见[人工验收表](../../AUTO-UPDATE-ACCEPTANCE.md)；不能用上述通过项替代 |

## 资源回归

旧实现每个文件新建 WinHTTP session，在 Server 2022 的 64 次已完成请求和取消循环后各多出 64 个 Event。创建栈位于系统 HTTP/WebIO 路径；这不证明某个具体系统漏洞。改为每种传输固定、初始化后只读的进程级 session，请求及回调仍独立释放。未放宽原断言。

| 最终负载 | 句柄 | 线程 | 取消最大耗时 |
|---|---|---|---|
| 64 次已完成 HTTP 404 | 205 → 205 | 未单列 | 不适用 |
| 64 次取消，每次新 worker | 205 → 205 | 9 → 9 | 26 ms |
| 64 次取消，复用一个 worker | 206 → 208，join 后 205 | 10 → 10（worker 存活时） | 31 ms |

最后一组包含一个显式测试 worker 的句柄；join 后回到同一进程的 205 基线。HTTP State 的创建、销毁和最终关闭回调均平衡。测点为 loopback HTTP 请求已发起后设置取消标志到线程返回；不是串口转发/显示性能，也不是长期稳定性证明。原始数字见 CTest 日志。

正式 GUI 6 例从 Start-Process 至主窗口句柄出现为 74–147 ms，从 WM_CLOSE 至进程退出为 28–58 ms。只代表本次空闲 runner 和这些受控负载，不能泛化为现场启动或更新时延承诺。

## 桌面复审与残留问题

复审者实际查看了 14 张 updater 原图，结论见[桌面复审](desktop-review.md)。本目录保留四张原始截图：

- [404：浅色错误提示](evidence/updater-url-a-404-before.png)
- [404：切到深色](evidence/updater-url-a-404-theme.png)
- [伪签名：深色错误提示](evidence/updater-url-b-invalid-signature-theme.png)
- [慢 body：真实 CMD 操作](evidence/updater-slowbody-cmd.png)

观察到深色主题的“全部断开”按钮仍保留浅色矩形背景。`MainWindow.cpp` 的 ApplyTheme 显式重绘列表遗漏 `disconnectAllButton_`，是需要复现验证的根因候选。本次将它记录为后续界面修复项，不把自动化全绿解释为所有视觉问题已经解决。静态截图不能证明无闪烁；100% DPI 不能代表其他 DPI。

既有 V1.0.6 审核问题仍由 [#26](https://github.com/john2000smith7-del/SerialCtl/issues/26) 跟踪，本功能不声称已修复那些问题。

## 部署

第一次手动部署带更新器的包；以后只修改 exe 旁 `update.ini` 的 `[update] url=`，填写最终的完整 `update.json` 地址后重启。当前只知道 `172.16.2.188`，默认 URL 留空。管理员使用 [new_update_feed.py](../../../../tools/new_update_feed.py) 发布已签名静态文件，私钥离线保存。详见[部署文档](../../../architecture/AUTO-UPDATE.md)。
