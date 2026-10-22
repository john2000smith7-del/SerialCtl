# V2.0.0 双文件签名更新发布验证

## 正式发布

- 源码：`8041ff06f0837063c55f2a2eeb3475a06ee1c59f`，标签 `v2.0.0` 指向该提交。
- [Windows 构建 39731157878](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39731157878) 全部成功：23 项 CTest、MSVC x64、原 /MT 和 YY-Thunks 配置、PE 子系统 6.01/Win7 导入检查、包白名单和 ZIP 重开验证通过。
- Windows 原生提取实际完整 V2.0.0 ZIP 通过：临时测试密钥签名，验签后只提取 24 个运行文件，不提取驱动或 update.ini，不执行包内程序。打包程序启动/退出、深浅主题和弹窗布局、受控 CMD、侧栏、隔离 SFTP、自动更新启动烟测均通过；10 项 Python 客户端测试、24 项签名发布工具测试通过。见 [windows-build-summary.txt](windows-build-summary.txt)。
- [发布流程 39732471738](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39732471738) 成功：重取指定 main 构建的精确 ZIP，核对摘要，并用源码内置公钥验证公开签名元数据与该 ZIP 的描述完全一致后发布。见 [publish-summary.txt](publish-summary.txt)。
- [正式版本](https://github.com/john2000smith7-del/SerialCtl/releases/tag/v2.0.0) 设为最新，正式资产恰好两个：完整 ZIP 与内嵌签名 update.json，没有外置 .sig、校验侧文件或 files 树。

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| SerialCtl-V2.0.0-Win7-x64.zip | 130,237,881 字节 | `133667c90c50b4da8f63614cb725ed96aaf11b7f93d03fb5ce8eda7eeb9c2aa8` |
| update.json | 4,471 字节 | `7d22a0ecd3b36d1df3daa4d1d71dd12f4ebbc25a0ed2993a17311cbc51b4c876` |

## 独立回读与真实签名

从正式 Release 重新下载两个资产，通过内嵌数字签名与完整 ZIP/运行文件描述验证；重开 ZIP 检查全部 322 条目的 CRC、每个 SHA256SUMS 记录、VERSION、干净源码 SHA、三份运行程序的 x64/6.01 PE，以及 serialctl.exe 内新公钥字节与源码一致。包内无私钥、开发源文件、PDB/OBJ 或本地配置；update.ini 是空网址模板。见 [published-zip-verification.json](published-zip-verification.json)。

补充将实际发布文件交给未定义测试宏、无外部公钥覆盖的生产验签/提取/暂存源码，LLVM-MinGW/Wine 下通过：以编译内置新公钥接受真实签名，提取 24 个运行文件、删除临时 ZIP 后再次验签和核对暂存文件。改变真实签名一个十六进制字符会被拒绝；不执行包内程序。见 [native-production-trust.txt](native-production-trust.txt)。这不是 Win7 实机或用户内网站点的在线安装验收。

新签名身份经用户明确授权建立，公钥 SPKI SHA-256：`196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a`。真实私钥在源码及发布目录之外，以目录 0700、文件 0600 保存；后续版本复用，未提交 Git、未上传 CI、未包含在 ZIP、JSON 或网站中。CI 只接收公开签名 JSON；公开签名回执见 [production-signing.json](production-signing.json)。密钥位置见 AGENTS.md，无外部保险库，替换工作区前需私密备份。

## 先前候选补充与现场边界

新密钥之前的候选 [39722974470](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39722974470) 验证过双文件协议和同套 Windows 检查，但不是上述正式 ZIP。本地 Python 24 项回归、Wine 更新客户端全套及实际 V1.10.0 ZIP 提取通过，验证已有 PowerShell 包的路径和压缩格式；见 [python-feed-tests.txt](python-feed-tests.txt)、[wine-real-archive.txt](wine-real-archive.txt)。旧 Wine 安装测试在旧协议第一个真实子进程案例未通过，未将其记为成功；后续正式 Windows 同套安装测试通过，保留 [wine-update-tests.txt](wine-update-tests.txt)。

首次从 V1.10.0 或更早版本迁移，必须手动部署 V2.0.0 一次，保留原 update.ini。服务器先放完整 ZIP，再替换 update.json。后续启动自动检查、原网址配置、空闲安装及配置/日志/驱动保护继续保留；旧密钥签发的 feed 不被新信任根接受。

未运行：真实 Win7/10/11 实机、用户内网更新站点、正式签名的现场在线安装、断电/磁盘满、实体串口/电源、真实 SSH/Gateway 和长时间运行。Windows Server 2022 CI、Wine、受控 HTTP 和文件替身不代替这些验收。
