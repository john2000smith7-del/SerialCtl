# SerialCtl

SerialCtl 是面向 Windows 的原生 C++17 / Win32 串口与远程终端工具。

当前正式版本：`V1.0.2`。生产程序不依赖 .NET Framework，使用 `/MT` 静态运行库，并以 Windows 7 SP1 为最低系统版本。

## 主要功能

- Windows 7 SP1、Windows 10、Windows 11，提供 x86 和 x64 程序。
- 本地串口终端；串口打开后可通过 TCP `7000` 端口在可信内网共享。
- 发现并连接其他 SerialCtl 已打开的串口，支持最多 32 个客户端同时读写，慢客户端会被隔离断开。
- Telnet 和 SSH；SSH 使用随包提供的定制 PuTTY `plink.exe`。
- SSH 会话的 SFTP 文件浏览、排序、多选、上传、下载、取消、重试和文件管理。
- 多会话切换，每个会话独立保存终端、滚动、编码、日志和 SFTP 状态。
- 单元格式 VT/xterm 终端，支持 ANSI 16/256/RGB 色彩、备用屏幕、宽字符、bracketed paste 和 20,000 行回滚。
- UTF-8、GBK、GB2312 编码，以及独立时间戳栏、选择、复制、粘贴、滚动和字体缩放。
- 常用命令、最多 10 步的命令宏、执行进度、停止及 TXT 导入导出。
- 深色与浅色主题。

## 使用

### 本地串口与共享

1. 点击顶部“串口”。
2. 选择 COM 口、波特率和流控。
3. 点击“连接”。串口打开后同时监听配置的共享端口，默认 `7000`。
4. 根据现场安全策略配置 Windows 防火墙。

共享串口没有认证、加密或写入互斥，只适合操作者可控的可信内网。多个客户端同时写入时，需要自行避免命令交叉。

### 连接其他电脑的串口

1. 点击“共享串口”。
2. 输入来源电脑的 IP 和共享端口。
3. 刷新远端串口列表。
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

常用命令默认写入 `%APPDATA%\SerialCtl\commands.txt`。文件格式示例位于 `docs/package/常用命令示例.txt`。

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

cmake -S . -B .\build\windows\x86\cmake -A Win32
cmake --build .\build\windows\x86\cmake --config Release
```

CMake 会从 `third_party/yy-thunks/yy-thunks.1.2.1.nupkg` 临时解出所需兼容对象，不要求源码树保留展开后的依赖构建目录。

## 发布包

正式本地发布入口：

```powershell
.\scripts\release.ps1
```

发布流程会重新构建并测试 x86、x64，检查 PE 架构、文件版本、子系统版本 6.01 和禁止依赖，生成固定白名单内容，压缩后重新解压并核对 SHA-256。成功后 `bin` 中只保留：

```text
SerialCtl-V1.0.2-Win7.zip
```

压缩包内的 `windows\x86\native` 与 `windows\x64\native` 分别包含匹配架构的 `serialctl.exe`、`plink.exe` 和 `psftp.exe`。复制到目标电脑时必须完整保留同一架构目录中的三个程序。

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

## GitHub 自动构建与下载

推送 main 或手动运行 Actions 的 `Windows Win7 release build` 会构建并验证 x86/x64。正式程序包可在仓库 Releases 下载；Actions 的 `SerialCtl-Win7-portable` 为构建产物（保留 30 天）。CI 使用 Windows Server 2022，不替代真实 Win7 SP1 与硬件验收。
