# SerialCtl V1.0.0 发布说明

V1.0.0 是当前原生 SerialCtl 实现的首个正式版本基线。

## 主要功能

- 本地串口、可信内网串口共享、共享串口发现、Telnet 和 SSH。
- 多个 SSH、串口和网络会话同时保持并独立切换。
- 单元格式 VT/xterm 终端，支持 ANSI 色彩、宽字符、主/备用屏幕、光标编辑、bracketed paste、20,000 行回滚和宽行横向回看。
- 独立毫秒时间戳栏、UTF-8/GBK/GB2312 编码、自动会话日志和日志另存。
- 常用命令的添加、编辑、删除、排序、TXT 导入导出，以及可停止的多步骤宏。
- SSH 主机密钥非交互校验和实时终端尺寸同步。
- SFTP 目录面包屑、排序、多选、上传、下载、新建、重命名、删除、权限修改及可取消传输队列。
- 对空格、中文、反斜杠、通配符和符号链接等远端名称进行精确、安全的 SFTP 操作。
- 统一的深色/浅色 Win32 界面、紧凑对话框、覆盖式滚动条及可调节 SFTP 侧栏。

## 兼容性

- Windows 7 SP1、Windows 10、Windows 11。
- 同一便携包内提供 x86 和 x64，两种架构彼此隔离。
- 原生 C++17/Win32，静态 MSVC 运行库，不依赖 .NET Framework 或 Visual C++ Redistributable。
- SSH/SFTP 基于定制 PuTTY 0.85；Windows 7 兼容由目标宏、PE 子系统 6.01 和 YY-Thunks 共同保证。

## 使用提示

- 完整解压压缩包后，进入与系统匹配的 `windows\x86\native` 或 `windows\x64\native` 目录运行 `serialctl.exe`。
- 不要单独复制 `serialctl.exe`；SSH 需要同目录的 `plink.exe`，SFTP 需要同目录的 `psftp.exe`。
- 共享串口没有认证或加密，只应在可信内网使用。
- 实际 Windows 7 驱动、串口硬件和现场 SSH/SFTP 兼容性仍须按测试清单验收。
