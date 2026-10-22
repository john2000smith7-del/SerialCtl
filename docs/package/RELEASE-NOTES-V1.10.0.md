# SerialCtl V1.10.0

- 顶部选择栏使用统一长条外框，取消每个入口的独立描边；修复主题按钮及全局下拉框在选项、焦点和主题切换时出现的默认矩形覆盖和残留底色。
- 修复深色主题下 AI 输入框圆角外的黑色背景方框。消息框移除身份/状态标题，底部不再显示 Enter 提示；保留左右消息布局、蓝色强调和历史滚动。
- AI 可直接读取并向本次明确选定的本地/远程串口输入命令，不再逐条二次确认。不默认本地串口，保留远程 IP 查询、目标校验、停止和防重复提交；发送成功仍只代表队列接收，实际结果以设备输出为准。AI 不操作电源。
- 修复旧版会话历史索引，恢复有效历史。协议使用字符串 web_search 配置、嵌套线程 ID 与请求 ID 匹配，忽略交错无关通知；兼容仅发送最终 agentMessage、缺少 turn/completed 的服务，避免一直显示忙碌，并防止迟到事件结束新任务。
- 模型缺少可选推理元数据时允许实际调用验证；模型错误显示具体原因。SSH 启动支持可选 ~/.config/serialctl/codex-env.sh，缺少 CONTAINER_APIKEY 等变量时提示正确的服务器配置位置，不要求在 Windows 输入网关密钥。
- 保留串口、SSH/SFTP、CMD、电源、内网签名在线更新和 Linux 客户端；Win7 SP1 x64、C++17 / Win32、/MT、6.01 子系统、YY-Thunks 与原 PuTTY/NI-VISA 输入保持不变。

发布仅使用完整 Windows 构建、全量自动测试、PE/导入检查、包白名单及 ZIP 重开验证通过的程序。Windows Server 2022 CI 和离线进程替身不代表已通过 Windows 7/10/11 实机、用户服务器/网关、实体串口或电源验收。若服务器密钥只配置在交互式 ~/.bashrc，管理员仍需移动相应 export 到 SSH 用户的 ~/.profile 或权限 600 的专用 codex-env.sh。

在线更新：在程序旁 update.ini 的 [update] 下填写 url=完整 update.json 地址，保存并在空闲时重启。管理员必须部署与内置公钥匹配签名的 update.json、update.json.sig 和 files 目录；GitHub ZIP 下载链接不能直接作为更新清单地址。详情见随包 AUTO-UPDATE.md。
