# V1.10.0 修复验证

本次包含之前尚未发布的历史索引修复、顶部统一长条外框、公共下拉框修复，以及主题按钮、AI 输入框、消息简化、选定串口直接操作和服务器协议兼容修复。

## 本地补充检查

- LLVM-MinGW x64 交叉构建通过。生产 MSVC、/MT、YY-Thunks、子系统 6.01 设置保持不变。
- Wine 8/Xvfb 下 `ai_protocol`、`ai_client`、`ai_pane`、`ui_buffered_paint`、`ui_combo_paint` 共 5 项通过；最终调整后重跑 3 项 AI 测试通过。
- AI 离线进程替身覆盖：无关通知插入响应之前、请求 ID 关联、字符串 web_search、嵌套线程 ID、UTF-8 流式回复、最终 agentMessage、停止。UI 测试覆盖无角色标题、深色输入框外角底色、直接提交一次命令、防重复、目标固定、停止后拒绝写入、缺少 turn/completed 时结束、后续活动撤销提前结束、迟到结束不影响新任务、缺少模型环境变量时解除忙碌并显示原因。所有设备回调均为测试替身，未操作硬件。
- 在实际应用窗口中，通过主题按钮原生鼠标消息路径切换深浅主题并保留按钮焦点；检查统一顶部边框、AI 输入框圆角、模型/串口字段和移除的页脚提示。环境为 Wine，无 Windows 窗口管理器；不代表真实 Windows 桌面鼠标验收。
- SourceOnly 版本/依赖检查通过；Linux 环境中脚本要求的 git.exe provenance 步骤未运行。

## 发布门槛与现场边界

发布前必须完成仓库 Windows 全量构建、CTest、PE/Win7 导入检查、打包白名单和 ZIP 重开验证，以对应 Actions 记录为准。

未运行：用户 Linux Codex/内网 Gateway、其实际 CONTAINER_APIKEY 配置、真实 SSH、实体串口/电源、Windows 7/10/11 实机及长时间稳定性。服务器缺失环境变量仍须管理员在该 SSH 用户的 ~/.profile 或权限 600 的 ~/.config/serialctl/codex-env.sh 中配置后重连，客户端不能提供未知密钥。

对仅发送最终 agentMessage 的网关使用 1 秒静默兼容结束；后续推理/工具活动取消计时。若自定义网关将中间回复标记为最终后延迟更久才继续，仍需服务端修正结束语义。在线更新须由管理员持现有离线私钥签名并部署静态 feed；本次发布 ZIP 不代表已经部署用户的内网更新站点。
