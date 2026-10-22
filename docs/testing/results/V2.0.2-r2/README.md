# V2.0.2 r2 验证记录

产品版本保持 **V2.0.2**，修订包标签为 [v2.0.2-r2](https://github.com/john2000smith7-del/SerialCtl/releases/tag/v2.0.2-r2)。2026-10-23 07:54:55 北京时间发布，日志使用 UTC。原 v2.0.2 标签、资产及验证记录保留。

本次增加可展开、可调宽度的 AI 实时终端侧栏，复用实际终端 HWND/SessionState，独立保存 AI 侧栏偏好。AI 使用柔和主题色，模型仅显示并使用服务器默认配置。连接可指定 Linux 项目目录，开启 SSH 用户权限内的服务器文件和命令工具；显示实际输出、退出码及 diff，与设备终端通道区分。升级后新建 AI 会话应用新配置。

## 正式 Windows 构建

最终源码为 `3db66c7808b5fe5ec57d5adead48a87c4db42f22`。[Windows 构建 39873495558](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39873495558)，job `120153334148`，全部步骤成功：

- 24 项 CTest，包括实际 MainWindow/AI/终端控件、五类受控终端、真实 CMD 进程、电源模拟回读、AI 历史滚动、原生服务器工具输出、缺少汇总输出时保留实际增量、禁用字段深色像素及原有回归。
- MSVC x64、/MT、YY-Thunks、6.01 子系统及精确 DLL/Win7 导入检查。
- 白名单打包、完整 ZIP 重开、SHA-256、原生更新程序只提取批准的运行文件，不提取驱动和配置。
- 打包程序启动/退出，深浅主题、主题按钮原生输入像素、等距顶栏、AI 项目目录弹窗、默认模型只读、AI 侧栏展开/收起及无目标时不显示错误终端、CMD、SFTP、原有电源页面检查。
- 启动更新的空/缺配置、404、错误签名及慢响应界面流程；10 项 Python 客户端及 24 项签名工具测试。

见 [windows-build-summary.txt](windows-build-summary.txt)。Windows 原生构建运行在 Server 2022，不代表已验证实体 Windows 7/10/11。

初轮 [39872928042](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39872928042) 也全部成功，但缺少最后的禁用字段修正，未签名或发布该候选。

## 本地补充与界面

LLVM-MinGW x64 交叉构建与 Wine/Xvfb 初轮七项通过，见 [wine-targeted-tests.txt](wine-targeted-tests.txt)；新增原生流式输出兼容用例的 ai_protocol/ai_pane/ai_device 三项通过，见 [wine-final-stream-tests.txt](wine-final-stream-tests.txt)。最后禁用字段像素回归通过，见 [wine-disabled-fields.txt](wine-disabled-fields.txt)。

补充修改期间一次 clang 前端 SIGBUS 中断，保留 [原日志](cross-compiler-interruption.txt)；清理相关对象、停止源码改写后重新编译成功，见 [最终完整构建](cross-build-final.txt)及[禁用字段修正构建](cross-build-disabled-fields.txt)。正式 MSVC 候选独立构建通过。SourceOnly 版本/依赖检查及 Python 签名工具测试通过；Linux 下来源检查由正式 Windows Git 检查补足。

以下截图来自最终源码的实际 LLVM-MinGW 应用 EXE 在 Wine/Xvfb 的可见窗口，仅裁剪窗口范围，无设计重绘或模拟 AI 回复。右栏输出来自真实 CMD 进程，Wine 窗口装饰不代表 Windows 原生标题栏。

- [浅色双栏](wine-light.png)、[深色双栏](wine-dark.png)、[最小窗口](wine-minimum.png)、[收起侧栏](wine-collapsed.png)。
- [浅色连接弹窗](wine-light-dialog.png)、[深色连接弹窗](wine-dark-dialog.png)。

## 签名发布与独立回读

[发布 39874816535](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39874816535)，job `120157730385`，重取指定成功构建的 ZIP，核对摘要和原信任根签名后发布。使用既有 V2 更新签名身份，未轮换公钥，私钥始终留在仓库外，未上传。见 [签名回执](signing-receipt.json)、[发布摘要](publish-summary.txt)和[两个资产](release-assets.json)。

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| SerialCtl-V2.0.2-Win7-x64.zip | 130,253,809 字节 | `8e7cb88bd22d572f439dfca4a9514b2edf920165135bbde858c869cb44b17ffb` |
| update.json | 4,471 字节 | `7a0ddcd00efc572bfc67ee836a04831bd04604f280ec8555ac63f9cdea1934d4` |

从正式 Release 重新下载两文件，核对内嵌签名、完整 ZIP 摘要、322 条目 CRC、全部 SHA256SUMS、干净源码 SHA、VERSION、x64/6.01 PE，以及 EXE 内公钥与源码、签名身份一致。包内无私钥、任意本地配置、开发代码或测试程序，update.ini 仅包含用户批准的站点模板。见 [独立回读](published-zip-verification.json)。

使用未启用测试信任覆盖、无外部公钥参数的生产更新验签代码，在 Wine 内接受实际发布签名并提取 24 个运行文件；只改动一位签名立即拒绝。见[正例](production-trust-positive.txt)和[反例](production-trust-negative.txt)。Wine 的 winedevice 服务警告与两项预期退出码分别记录，不把这些警告当作实体 Windows 验收。未执行 ZIP 内的程序。

同一组已核验下载文件移入 bin，目录恰好两文件；原 V2.0.2 配套两文件留在工作目录备份。服务器先上传完整 ZIP，再替换 update.json，不解压、不改名，继续使用 `http://172.16.3.188/external/serial/update.json`。已安装 V2.0.2 的客户端需退出后手动替换一次，保留配置和数据；自动更新仍只接受更高产品版本。

## 未运行范围

真实 Windows 7/10/11、多 DPI、现场 SSH/Gateway、服务器项目实际写入和命令执行、实体串口/电源及长时间负载验收未运行。服务器工具协议、受控事件和模拟电源测试不代替这些项目，也不把 queued/accepted 当作设备执行成功。
