# SerialCtl

SerialCtl 是面向 Windows 的原生 C++17 / Win32 串口与远程终端工具。

本轮候选版本：`V1.0.9`。生产程序不依赖 .NET Framework，使用 `/MT` 静态运行库，并以 Windows 7 SP1 为最低系统版本。

## 主要功能

- Windows 7 SP1、Windows 10、Windows 11，仅提供 x64 程序。
- 本地串口终端；串口打开后可通过 WebSocket `7000–7015` 中可用端口在可信内网共享。
- 发现并连接其他 SerialCtl 已打开的串口，同一 WebSocket 会话服务容量上限为 128 个客户端（不等于性能测定），慢客户端会被隔离断开。
- Telnet 和 SSH；SSH 使用随包提供的定制 PuTTY `plink.exe`。
- SSH 会话的 SFTP 文件浏览、排序、多选、上传、下载、取消、重试和文件管理。
- 多会话切换，每个会话独立保存终端、滚动、编码、日志和 SFTP 状态。
- 主屏幕逻辑行重排与 CMD 本地行编辑；VT/xterm 终端，支持 ANSI 16/256/RGB 色彩、备用屏幕、宽字符、bracketed paste 和 20,000 行回滚。
- UTF-8、GBK、GB2312 编码，以及独立时间戳栏、选择、复制、粘贴、滚动和字体缩放。
- 常用命令、最多 10 步的命令宏、提交进度、停止后续提交及 TXT 导入导出；实际执行结果见终端输出。
- 默认浅色主题，支持深色切换；统一黑白顶部栏，桌面图标保持原样。
- 终端拖选完成自动复制、右键直接粘贴；顶部“设置”集中管理原终端菜单功能，明确全局与当前会话作用域。
- IT6332A 电源管理、USB/RS232 连接、三通道独立控制、串并联、跟踪、自动化及监测。
- 本地 CMD 与自动启动的固定范围 AI 接口；支持 Linux 客户端与二进制安全的 WebSocket v1。
- 发布包包含 Win7 NI-VISA 18.0 Runtime 完整离线安装文件。
- 可配置内网自动更新：每次启动后台检查，签名验证后在空闲时更新；保留配置、命令、日志与驱动。

## 使用

### AI 智能串口助手

顶部 AI 页面使用中间原终端区域进行对话，底部内部提供模型选择、多行输入与发送/停止；回复以蓝色强调的独立消息框显示，支持上下滚动查看历史。连接 Linux Codex 服务器后复用其模型配置，无需在 Windows 安装 Codex 或重复填写网关密钥。

关联串口由用户明确选择。可在输入栏选择“连接远程串口…”，填写远程 IP、查询并选择该主机已经开放的串口；不默认选择本机 COM。AI 可以读取日志，并在用户逐次确认后通过现有串口连接提交诊断命令，执行结果以设备回显为准。当前范围不新增多客户端写入仲裁。

实现、凭据与兼容边界见 [AI 架构说明](docs/architecture/AI-ASSISTANT.md)，正式发布前需完成 [AI 验收](docs/testing/AI-ASSISTANT-ACCEPTANCE.md)。本段不代表已通过现场 SSH、模型网关或 Win7 硬件验收。

### 本地串口与共享

1. 点击顶部“串口”。
2. 选择 COM 口、波特率和流控。
3. 点击“连接”。程序启动即提供统一服务，自动选择 `7000–7015` 可用端口；每个已打开串口注册为明确的会话。
4. 根据现场安全策略配置 Windows 防火墙。

共享串口没有认证、加密或写入互斥，只适合操作者可控的可信内网。多个客户端同时写入时，需要自行避免命令交叉。

### 连接其他电脑的串口

1. 点击“远程串口”。
2. 输入来源电脑的 IP，自动发现共享端口。
3. 自动查询，也可手动刷新远端串口列表。
4. 选择串口并连接。

### SSH 与 SFTP

首次连接未知 SSH 主机会显示指纹。请通过管理员等独立渠道核对后确认；确认结果持久保存到当前用户注册表，密钥变化时拒绝连接。

SSH 登录成功后，右侧 `SFTP` 页会自动读取目录。路径面包屑、排序、多选、拖入上传、传输队列和文件操作均使用同一 SSH 主机信息。

如果填写 SSH 密码，密码会通过 Plink 命令行参数传递。在多用户 Windows 环境中，优先使用 PuTTY 已保存的密钥配置或 Pageant。

### 日志与命令

会话日志默认写入：

```text
%LOCALAPPDATA%\SerialCtl\logs\
```

常用命令默认写入 `%LOCALAPPDATA%\SerialCtl\commands.txt`。文件格式示例位于 `docs/package/常用命令示例.txt`。

## 内网自动更新

编辑 `serialctl.exe` 旁的 `update.ini`，在 `[update]` 下填写 `url=完整的 update.json 网址`。默认留空，确定服务器 `172.16.2.188` 的实际发布目录后填写；后续地址变化只改这个文件。程序每次启动读取配置，服务器不可达时继续使用当前版本。正在使用的设备和任务不会为更新而被强制断开。

HTTP 更新以程序内置 RSA 公钥验证签名，服务器仅需托管静态文件。首次部署 V1.0.7 需正常解压安装；旧版本没有更新器，不能自动跳到本版本。用户步骤见 [AUTO-UPDATE.md](docs/package/AUTO-UPDATE.md)，管理员签名和部署见 [架构说明](docs/architecture/AUTO-UPDATE.md)。

## 工程结构

```text
SerialCtl/
├── CMakeLists.txt             # 原生生产构建入口
├── VERSION                    # 唯一机器可读取版本源
├── VERSION.md                 # 中文版本历史
├── src/                       # 原生生产代码
│   ├── app/
│   ├── connections/
│   ├── logging/
│   ├── platform/
│   ├── sftp/
│   └── terminal/
├── tests/
│   ├── unit/
│   └── integration/
├── docs/                      # 架构、测试与发布文档
├── scripts/                   # 构建、测试、检查、打包与发布入口
├── tools/                     # 独立工程辅助工具
├── third_party/               # 固定版本依赖、补丁与许可证
├── legacy/                    # 不参与生产构建的历史原型
├── build/                     # 可删除的中间状态，不提交
└── bin/                       # 仅保留最新验证通过的发布压缩包
```

目录按真实职责创建，不使用空目录或占位文件表示未来能力。

## 构建与测试

在 Visual Studio C++、Windows SDK 和 CMake 已安装的开发环境中执行：

```powershell
.\scripts\build.ps1 -Architecture All -Configuration Release -Clean
.\scripts\test.ps1 -Architecture All -Configuration Release
.\scripts\check.ps1 -Architecture All -Configuration Release
```

也可以直接使用 CMake：

```powershell
cmake -S . -B .\build\windows\x64\cmake -A x64
cmake --build .\build\windows\x64\cmake --config Release
```

CMake 会从 `third_party/yy-thunks/yy-thunks.1.2.1.nupkg` 临时解出所需兼容对象，不要求源码树保留展开后的依赖构建目录。

## 发布包

正式本地发布入口：

```powershell
.\scripts\release.ps1
```

发布流程会重新构建并测试 x64，检查 PE 架构、文件版本、子系统版本 6.01 和禁止依赖，生成固定白名单内容，压缩后重新解压并核对 SHA-256。成功后 `bin` 中只保留：

```text
SerialCtl-V1.0.9-Win7-x64.zip
```

压缩包根目录直接包含 x64 的 `serialctl.exe`、`plink.exe` 和 `psftp.exe`，解压后保持三者位于同一目录。

## Windows 7 兼容策略

- `WINVER`、`_WIN32_WINNT` 和 `NTDDI_VERSION` 固定为 Windows 7。
- 主程序及辅助程序 PE 子系统版本固定为 `6.01`。
- 使用 `/MT`，发布程序不依赖 Visual C++ Redistributable。
- 使用 YY-Thunks 处理新工具链可能引入的兼容调用。
- SSH/SFTP 使用定制 PuTTY 0.85，补丁和复现说明位于 `third_party/putty/`。

编译与导入检查不能替代真实 Windows 7、USB 转串口硬件和现场 SSH/SFTP 环境验收。人工测试步骤位于 `docs/testing/`。

## 已知边界

- 共享串口只适合可信内网，不提供认证、加密或写入互斥。
- 终端覆盖常用 VT100/xterm 行为，少见图形协议和部分专有扩展未实现。
- 真实 Win7 驱动、长时间大流量、具体 SSH 算法及服务器 SFTP 子系统需要现场验证。
- `legacy/csharp` 是历史 WinForms 原型，不属于生产程序或发布包。

## 审核与后续修改

修复代码、自动更新及审核资料已统一维护在 `main`。后续修复可从[问题与优化总追踪](https://github.com/john2000smith7-del/SerialCtl/issues/26)逐项领取；每项包含修改位置、建议和验收条件。问题已记录不代表已修复。

- [V1.0.6 独立审核、交叉复审与原始证据](docs/reviews/2026-10-11-v1.0.6/README.md)
- [V1.0.7 自动更新实际验证记录](docs/testing/results/V1.0.7-auto-update/README.md)
- [V1.0.7 真实点击、拖动和缩放审核](docs/testing/results/V1.0.7-desktop-interaction-20261014/README.md)
- [2026-10-15 桌面修复回归：点击/焦点通过，侧栏白块仍待处理](docs/testing/results/V1.0.7-desktop-fixes-20261015/README.md)

## GitHub 自动构建与下载

推送 main、feat/*、feature/* 或手动运行 Actions 的 `Windows Win7 release build` 会构建并验证 x64。正式程序包可在仓库 Releases 下载；Actions 的 `SerialCtl-Win7-portable` 为构建产物（保留 30 天）。CI 使用 Windows Server 2022，不替代真实 Win7 SP1 与硬件验收。

V1.0.9 操作说明见 [发布包说明](docs/package/README.md)，Linux AI 接入见 [AI 串口说明](docs/package/AI-SERIAL.md)。发布 ZIP 直接在根目录放置 x64 运行程序。

统一协议和客户端说明见 [WEBSOCKET-PROTOCOL.md](docs/architecture/WEBSOCKET-PROTOCOL.md)，本轮状态见 [V1.0.6-ACCEPTANCE.md](docs/testing/V1.0.6-ACCEPTANCE.md)。
