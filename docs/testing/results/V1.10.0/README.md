# V1.10.0 修复验证

本次包含之前尚未发布的历史索引修复、顶部统一长条外框、公共下拉框修复，以及主题按钮、AI 输入框、消息简化、选定串口直接操作和服务器协议兼容修复。

## 本地补充检查

- LLVM-MinGW x64 交叉构建通过。生产 MSVC、/MT、YY-Thunks、子系统 6.01 设置保持不变。
- Wine 8/Xvfb 下 `ai_protocol`、`ai_client`、`ai_pane`、`ui_buffered_paint`、`ui_combo_paint` 共 5 项通过；最终调整后重跑 3 项 AI 测试通过；补充迟到停止回应回归后，ai_pane 再次通过。原始日志见同目录 wine-*.txt。
- AI 离线进程替身覆盖：无关通知插入响应之前、请求 ID 关联、字符串 web_search、嵌套线程 ID、UTF-8 流式回复、最终 agentMessage、停止。UI 测试覆盖无角色标题、深色输入框外角底色、直接提交一次命令、防重复、目标固定、停止后拒绝写入、缺少 turn/completed 时结束、后续活动撤销提前结束、迟到结束不影响新任务、缺少模型环境变量时解除忙碌并显示原因。所有设备回调均为测试替身，未操作硬件。
- 在实际应用窗口中，通过主题按钮原生鼠标消息路径切换深浅主题并保留按钮焦点；检查统一顶部边框、AI 输入框圆角、模型/串口字段和移除的页脚提示。环境为 Wine，无 Windows 窗口管理器；不代表真实 Windows 桌面鼠标验收。
- SourceOnly 版本/依赖检查通过；Linux 环境中脚本要求的 git.exe provenance 步骤未运行。

截图见 [深色](wine-dark.png) / [浅色](wine-light.png)，应用 UI 来源为 13625d3；后续 ad93078 仅增加停止回应的回合校验与回归，不改变绘制。

截图报告中的 web_search 字符串和 result.thread.id 路径在仓库 V1.0.9 发布源码中已正确，本次增加了锁定这些形状的回归；没有将截图推测当作已捕获的现场原始报文。新增兼容主要处理最终消息结束语义、环境变量错误提示及旧历史索引。

## 发布门槛与现场边界

已完成正式验证与发布：

- 生产源码：`ad9307877c151d21bd02db261764dabed5c8564d`。
- [Windows 构建 39715432912](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39715432912)：成功；23 项 CTest 全通过，MSVC x64 /MT + YY-Thunks，PE/Win7 导入检查、显式包白名单、ZIP 重开/每文件摘要核对通过。
- 打包程序启动/退出、主题/弹窗/布局、受控 CMD 与 SFTP UI 回归通过；更新启动烟测、10 项 Python 客户端测试及 16 项签名 feed 工具测试通过。原始日志摘录见 [windows-build-summary.txt](windows-build-summary.txt)。
- [发布流程 39716747697](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39716747697)：成功；重新下载已验证构建产物并校验 SHA-256 后发布。标签 `v1.10.0` 指向上述源码，并设为最新发布。
- [正式 ZIP](https://github.com/john2000smith7-del/SerialCtl/releases/download/v1.10.0/SerialCtl-V1.10.0-Win7-x64.zip)：130,221,861 字节，SHA-256 `dfb6525b37e94a045b48aeca3a95c22f1b939447abbf3b73b8bb56ea71470f63`。
- 当前 Linux 网络不能直接取回 Actions 存储域的产物/日志；日志经 GitHub 连接器读取，发布流程在 GitHub 内完成产物复核。之后独立下载正式 release ZIP，验证全部 321 个条目的 ZIP CRC、每文件 SHA256SUMS、版本、x64 与 6.01 子系统，均通过；见 [published-zip-verification.json](published-zip-verification.json)。

未运行：用户 Linux Codex/内网 Gateway、其实际 CONTAINER_APIKEY 配置、真实 SSH、实体串口/电源、Windows 7/10/11 实机及长时间稳定性。服务器缺失环境变量仍须管理员在该 SSH 用户的 ~/.profile 或权限 600 的 ~/.config/serialctl/codex-env.sh 中配置后重连，客户端不能提供未知密钥。

对仅发送最终 agentMessage 的网关使用 1 秒静默兼容结束；后续推理/工具活动取消计时。若自定义网关将中间回复标记为最终后延迟更久才继续，仍需服务端修正结束语义。在线更新须由管理员持现有离线私钥签名并部署静态 feed；本次发布 ZIP 不代表已经部署用户的内网更新站点。
