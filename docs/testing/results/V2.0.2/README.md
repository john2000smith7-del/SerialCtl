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

## 正式构建与发布

- 源码 `03e5c734d678e403ba9b6eabc3ec58f047e143ab`，[Windows Server 2022 构建 39744536287](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39744536287) 全部成功：24 项 CTest、MSVC x64 /MT、YY-Thunks、PE 6.01/Win7 导入检查、包白名单、完整 ZIP 重开及仅提取签名运行文件通过。新增 ai_device 在 Windows 原生通过，CMD 真进程及原有测试通过。见 [windows-build-summary.txt](windows-build-summary.txt)。
- 打包程序启动/退出、深浅主题/弹窗、原生主题按钮像素/12px 间距、受控 CMD、侧栏及隔离 SFTP UI、自动更新启动烟测通过；另有 10 项 Python 客户端、24 项签名工具测试通过。测试替身不代表已联调真实 SSH/Gateway、Telnet 或电源。
- [签名发布 39745928827](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39745928827) 重取指定成功构建的 ZIP，核对摘要和元数据签名完全匹配后发布。[v2.0.2](https://github.com/john2000smith7-del/SerialCtl/releases/tag/v2.0.2) 是最新正式版本，标签指向上述源码，资产恰好两个。见 [publish-summary.txt](publish-summary.txt)。

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| SerialCtl-V2.0.2-Win7-x64.zip | 130,245,521 字节 | `8d497d38e69db7b5ca20293b4395b6dcf9f900a17aa27d17a3e90cd056e41a9a` |
| update.json | 4,471 字节 | `3649423eb46d9522217238766334a54c6357ee5ec1af9427917bae34c605bbaf` |

## 发布回读与签名

从正式 Release 重新下载两个文件，核对签名、完整 ZIP SHA-256、322 条目 CRC、全部 SHA256SUMS、VERSION、干净源码 SHA、三份 x64/6.01 PE；serialctl.exe 的公钥字节与源码和签名身份一致。包内无私钥、开发代码、PDB/OBJ 或任意本地配置；update.ini 是用户确认的 `http://172.16.3.188/external/serial/update.json` 模板。见 [published-zip-verification.json](published-zip-verification.json)。验证后同一组下载文件移入 bin，原 V2.0.1 配套两文件保留在工作目录备份。

沿用 V2.0.0 已授权的签名密钥，未重新生成。公钥 SPKI SHA-256 `196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a`；私钥保持仓库外目录 0700/文件 0600，未上传 CI、Git 或发布。公开回执见 [production-signing.json](production-signing.json)。

将实际下载文件交给无测试宏、无外部公钥覆盖的生产验签/提取/暂存代码，在 LLVM-MinGW/Wine 下通过：内置根接受签名，仅提取 24 个运行文件，排除驱动和 update.ini；删除 ZIP 后再次验签并核对暂存文件。改变真实签名一个十六进制字符被拒绝。未运行包内程序或安装更新。保留 Wine 环境服务提示，见 [native-production-trust.txt](native-production-trust.txt)。

## 使用与边界

先在原入口打开 CMD/Telnet/SSH/串口，再从 AI 输入栏选择对应会话。电源先在本机电源页连接 IT6332A 并勾选通道，AI 对话中明确要求 ON/OFF 后直接操作，无需另选终端或二次弹窗；原 action 回读区分 queued、confirmed、unknown/failed。当前只接入本机电源页的既有输出开关，不扩大 WebSocket 的 SSH/Telnet 公开资源范围。

升级后新建 AI 会话加载三个新增电源工具，旧历史仍可恢复；当前检查的 thread/resume 无动态工具替换参数，不能假称已给旧线程补注册。网站 `/external/serial/` 先上传完整 ZIP，再替换 update.json，不解压/改名；旧 INI 仍保留。

未运行：真实 SSH/Telnet/Gateway 联调、实体电源/串口、用户内网站点及正式签名现场在线安装、真实 Win7/10/11、其他 DPI、长时间运行。受控传输、模拟电源回读、Windows Server 2022 CI 和 Wine 都不代替现场验收。
