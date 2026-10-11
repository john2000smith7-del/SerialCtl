# 证据索引与保留范围

审核源码：`5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`；本轮重新执行的 [Windows Actions run 38097145874](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38097145874)。不是把旧成功构建当作本轮实测。

仓库保留全部审核文本日志、JSON/CSV、复现源码与17张关键未修改PNG；不提交生产EXE、驱动、模型ELF、完整ZIP及209张图。完整原图文件名、SHA-256和本仓库保留标记见 [screenshot-manifest.json](screenshot-manifest.json)。

| 完整产物 | 来源及SHA-256 | 获取时的到期时间（UTC） |
|---|---|---|
| SerialCtl-UI-evidence（209原始PNG、日志、JSON） | [artifact 11686766435](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38097145874/artifacts/11686766435)；`ac204cae833f37fcaf43a38e43181f450752adb558dd0f85091a45fd8d30eef8` | 2027-01-09 00:03:56；平台可能提前清理 |
| SerialCtl-Win7-portable（外层artifact ZIP） | [artifact 11686566640](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38097145874/artifacts/11686566640)；`5c5d10a142a9adc8bee8092c91d3eabb9838082efe0dc274769a40a31283c5bd` | 2026-11-10 00:09:13；平台可能提前清理 |

内层发布ZIP的SHA与外层artifact不同，见 [runtime-hashes.json](windows-fresh/runtime-hashes.json)、[MANIFEST.json](windows-fresh/runtime/MANIFEST.json) 和 [SHA256SUMS.txt](windows-fresh/runtime/SHA256SUMS.txt)。抓取时的完整元数据见 [fresh-artifacts.json](source/fresh-artifacts.json)。长期保管人应在到期前自行下载完整产物。

```bash
gh run download 38097145874 --repo john2000smith7-del/SerialCtl --name SerialCtl-UI-evidence --dir serialctl-ui-full
gh run download 38097145874 --repo john2000smith7-del/SerialCtl --name SerialCtl-Win7-portable --dir serialctl-portable-full
```

核心行为证据：[Windows构建日志](windows-fresh/workflow-logs/0_build.txt)、[14项CTest stdout](windows-fresh/ui/serialctl-ui/ctest-x64.txt)、[桌面环境](windows-fresh/ui/serialctl-ui/ui-environment.json)、[CMD显示测量](windows-fresh/ui/serialctl-ui/x64-light-display.json)、[35 CMD收尾](windows-fresh/ui/serialctl-ui/gui-lifecycle.json)、[压力CSV](architecture/fresh-stress.csv)。

边界反例：[电源/历史生产函数结果](security/production-routines-result.json)、[终端双模型](architecture/model-run.log)、[CMD尾行粘贴](product/paste-tail-probe.log)、[DPI公式](desktop/layout-model.json)。模型/替身替换的层次见各独立报告；它们不代表硬件或Win7实测。

动态视觉：[浅色反例](windows-fresh/ui/serialctl-ui/x64-light-animation-frame-8.png)、[其后一帧](windows-fresh/ui/serialctl-ui/x64-light-animation-frame-9.png)、[深色反例](windows-fresh/ui/serialctl-ui/x64-dark-animation-frame-9.png)、[其前一帧](windows-fresh/ui/serialctl-ui/x64-dark-animation-frame-8.png)。CopyFromScreen反例证明采样画面有重复按钮，不证明唯一绘制根因或持续时间；PrintWindow不得用于证明动态无闪烁。

原始日志仍保留执行机器绝对路径与UTC时间，不改写测量事实。认证头、令牌、临时带签名下载URL不属于交付证据。现场未运行项见 [人工清单](../reports/09-manual-acceptance.md)。
