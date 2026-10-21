# V1.0.9 Windows 发布验证

源码提交：`b5d6bec0b25260963638ba845a93558f30267a9c`。

- [Windows 正式构建](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39593945347)：成功。
- Windows Server 2022 / MSVC x64 / Release / MT / YY-Thunks；全量 22 项 CTest 通过，包含 3 项 AI 测试。
- PE 架构、版本、子系统 6.01、Win7 导入及运行库检查通过。
- 显式包清单、ZIP 重开、所有文件哈希核对通过；打包程序启动、深浅主题/布局、受控 CMD、SFTP 界面、更新启动和 Python 协议回归通过。
- [校验归档后发布](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39595683158)：成功。
- [V1.0.9 发布页](https://github.com/john2000smith7-del/SerialCtl/releases/tag/v1.0.9)，归档 `SerialCtl-V1.0.9-Win7-x64.zip`，130218137 字节。
- 归档 SHA-256：`a09e8e0565b3c724b207e283c6027d6ea680913b3b0d8bf468dac00ff065bbc0`。发布资产的 GitHub SHA-256 摘要与已校验构建包一致；标签指向上述源码提交。

日志摘录见 `windows-release-checks.txt`。较早开发记录中的 MSVC、更新客户端及安装器未运行项，已在本次 Windows 流程中补充验证；Wine 的 session_logger 失败没有复现于本次 Windows 全量测试。

仍未进行真实 Windows 7 SP1 / 10 / 11、现场 SSH / 模型 Gateway / Codex 推理、实体串口/电源或长时间硬件验收。AI 客户端进程测试使用离线替身，AI 窗口测试使用合成消息，不能等同于真实网关和设备联调。发布不改变这些验证边界。

本记录是对发布的文档补充，不改变发布标签对应的生产源码或包内容。
