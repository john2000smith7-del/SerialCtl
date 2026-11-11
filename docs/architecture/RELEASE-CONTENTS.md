# 发布包必要文件范围

2026-11-01 用户确认：保留完整离线驱动及 Linux/Python 接入工具，仅从软件包移除开发验收资料。打包仍使用 scripts/release.ps1 与 package.ps1 的显式白名单，不改变产品功能、根目录布局或自动更新协议。

## 保留内容

| 文件或目录 | 用途与保留原因 |
| --- | --- |
| serialctl.exe | C++17／Win32 x64 主程序；Win7 SP1、/MT、子系统6.01；内置离线语音引擎和模型，保持旧更新客户端的 32 MiB 单文件上限 |
| plink.exe、psftp.exe | 已固定并定制的 SSH／SFTP 运行工具，必须与主程序同目录 |
| update.ini | 用户批准的更新地址模板；已有安装升级时保护原配置 |
| VERSION | 唯一版本来源的随包副本，更新器核对已签名版本 |
| README.md、RELEASE-NOTES.md、AUTO-UPDATE.md | 使用、修改说明、升级部署与故障恢复 |
| AI-SERIAL.md、AI-API.md、WEBSOCKET-PROTOCOL.md、POWER.md、常用命令示例.txt | 已有远程接入、协议、电源功能及命令使用说明 |
| serialctl_client.py、serialctl_api.py、serialctl_ws.py | 既有 Linux/Python 客户端；三个文件保持同目录，Windows GUI 不依赖 Python |
| THIRD-PARTY-NOTICES.md 与 JSON／MINIZ／PUTTY／YY-THUNKS 许可证 | 第三方分发声明与许可证，不以精简名义删除 |
| MANIFEST.json、SHA256SUMS.txt | 构建来源、版本、依赖、Win7 目标和归档逐文件校验；不是额外部署文件 |
| drivers/ni-visa18/ | 完整官方 NI-VISA 18.0 Runtime 离线安装器，包括其自身安装依赖、许可证及x86/x64组件 |

主程序的 /MT 与无 .NET 运行依赖不代表第三方驱动安装器可以删掉其运行库组件。安装器包含多个平台组件，由官方安装逻辑选择；未做相应实机安装矩阵前不能拆删 MSI、CAB、MSU、许可证或语言资源。

## 移出软件包、继续保留在仓库

```text
docs/testing/AUTO-UPDATE-ACCEPTANCE.md
docs/testing/V1.0.6-ACCEPTANCE.md
docs/testing/WIN7-HARDWARE-TEST.md
docs/testing/MANUAL-REGRESSION.md
```

它们是开发、历史验收或现场测试清单，不是应用运行文件。移出包不取消对应测试要求，不删除仓库中的原文，不将历史通过记录当作本次验收。

## 当前数量与收益

以已交付 V2.0.18 ZIP 实际盘点：总322个文件，根目录27个，离线驱动295个；驱动压缩数据约占整个ZIP的97.6%。去除四份验收资料后，新候选包预计318个文件、根目录23个；正式数量以当次白名单与官方驱动清单核对结果为准。

这次精简减少用户看到的开发资料，体积只会小幅变化。取消驱动或接入工具不在本次范围。

## 自动更新和发布红线

- 只调整打包复制白名单；保留客户端和签名工具中历史可更新文件的允许列表。上述四份文档本来不是必需运行项，缺少它们的包仍须经过完整 ZIP 描述、签名和原生提取验证。
- 不移动运行文件或现有客户端工具到新目录，不扩展更新器写入范围。旧安装的历史文档不主动删除；更新继续保护 update.ini、日志、笔记、驱动和用户文件。
- 原私钥、公钥身份、schema、版本比较和恢复逻辑保持；不能修改现有 ZIP 后继续使用旧 update.json。
- 本次是当前 V2.0.18 的打包规则维护，旧 Release、标签及双文件保持原样。CI 同版本输出仅作候选验证；下一次正式交付必须递增版本，从精确 main 重新构建验证、完成真实旧客户端升级、原私钥签名及发布后复核。
- 用户最终仍只接收 SerialCtl-Vx.y.z-Win7-x64.zip 与其匹配的 update.json，不增加 .sig、files 或维护备份作为应用交付资产。

构建、签名及发布操作见 [build-release.md](build-release.md)，开发规则见 [AGENTS.md](../../AGENTS.md)。
