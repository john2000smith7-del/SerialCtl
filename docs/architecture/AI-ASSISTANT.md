# AI 智能设备助手（V2.0.2）

## 页面与连接

顶部增加与电源、串口平级的 **AI** 入口。首次进入显示紧凑 SSH 连接弹窗，包含服务器地址、SSH 端口、用户名、密码、可选 Linux 绝对项目目录及测试连接。建立连接后，中间原终端位置显示 AI 对话页；右侧初始收起，可展开为关联终端的真实实时输出区。复用同一个 terminal_ HWND 和 SessionState，不复制输出、不增加连接；目标改变通过 AiTargetChanged 同步实际会话，后台任务仍固定本次 turnTarget。拖动间隔调整宽度，AiMonitorWidth/AiMonitorCollapsed 独立保存，不修改原命令/SFTP 侧栏偏好。

用户消息靠右并使用浅蓝底色；AI 每次回复位于独立圆角消息框。输入栏固定在中心区域内部底部，服务器默认模型只读显示，关联终端选择在输入栏内。背景复用浅色 window / 深色 raised token，消息及输入字段使用原 surface token。上方消息区独立支持滚轮、Home/End、PageUp/PageDown、方向键和覆盖滚动条拖动。阅读历史时流式输出不抢走阅读位置，提供“返回最新消息”。渲染缓存按消息更新，历史消息不会随每个 token 重新测量全文。

终端本身仍不增加输入栏，右键粘贴和拖选复制规则保持原样。选中文字可通过设置中的“发送选中内容到 AI”或 Ctrl+Shift+A 附加到 AI 草稿，发送前不会上传。

## 目标终端

AI 不默认关联本地串口。输入栏明确显示“请选择终端”，用户可以选择已有本地/远程串口、CMD、Telnet、SSH 会话；每项标明 kind，或选择“连接远程串口…”：

1. 输入远程 IP，不预填本机地址。
2. 复用现有异步 WebSocket 7000–7015 发现机制，查询该 SerialCtl 实例已经开放的串口。
3. 用户选择具体串口后连接，明确关联该 IP/串口；实例变化或串口不可达时沿用现有错误处理。
4. AI 任务启动时固定关联会话 ID。切换主窗口页面不会将正在分析的任务重定向到别的串口。

按当前 V1 范围，不增加客户端间写入锁、租约或新网关协议。AI 在对话内显示目标及命令，直接通过现有本地 SessionService 或已有远程串口的 QueuedConnection 提交。无需逐条二次确认；用户发送任务时固定本次目标，模型不能自行换口。提交仅代表进入发送队列，执行状态仍未知；以实际设备回显判断结果，不自动重发。共享串口原有多人写入语义保持不变，操作者需要避免同时输入。

## 客户端与协议边界

- `src/app/AiPane`：原生 Win32 控件、消息框绘制、滚动、输入、默认模型显示/终端选择、选定串口直接操作、会话索引。
- `src/ai/CodexSession` / `CodexProtocol`：请求关联和协议适配，把模型、对话、任务及串口工具转换为界面事件；App Server 方法名、参数和历史消息结构集中于适配层，窗口不处理 JSON-RPC。
- `src/ai/CodexClient`：独立工作线程，使用随包 Plink 的 `-T -batch` 启动 Linux `codex app-server --listen stdio://`，分别读取 stdout JSONL 和 stderr。UI 不直接读写进程管道。
- `src/ai/AiProtocol`：有界 UTF-8 JSONL 拆帧、滚动状态、固定工程师指导和四个终端工具和三个电源工具。
- `src/connections/PasswordPipe`：当前用户 ACL 的一次性本机命名管道，通过 Plink `-pwfile` 传递密码，密码不进入命令行或磁盘文件。
- `PuttyHostKey` 的新增安全密码选项仅用于 AI；原 SSH/SFTP 调用的默认行为不变。首次未知主机由用户核对完整指纹；变化的身份不自动接受。

服务器和可选项目目录保存在当前用户注册表；旧 Model 选择值不再读取或发送。密码默认不保存；勾选记住密码时使用 Windows Credential Manager（Win7 提供的 API），不写入 INI、日志或仓库。Linux Codex 使用该 SSH 用户已有模型和认证配置，Windows 不维护网关密钥，也不安装模型或独立 Bridge。

初始化后查询 `config/read` 获取服务器默认模型，完成前不发送用户任务；创建、恢复和运行回合使用该默认值，没有可用默认值时由服务器决定。客户端不显示模型选择、刷新或手工 ID；传输健康检查可继续使用 `model/list`。能否使用以实际调用为准，真实调用和工具请求到达前不标记为验证通过。JSON-RPC 的动态工具属于 Codex 实验接口，已用本开发环境 Codex CLI 0.163.0 检查协议形状；更旧或自定义 App Server 必须实际联调，不假定兼容。

客户端提供 `serialctl_sessions`、`serialctl_read`、`serialctl_observe`、`serialctl_command` 以及 `serialctl_power_get`、`serialctl_power_output`、`serialctl_power_action`。每次终端操作都检查明确关联的会话，不允许模型改成别的目标。按用户授权开启 Codex 原生服务器 shell/文件工具，thread/start、resume 使用 danger-full-access / approvalPolicy never，turn/start 使用 dangerFullAccess。操作权限限于 SSH 用户与容器可访问文件；cwd 作为 JSON 参数传递，绝不拼入 SSH shell 命令。服务器工具与 serialctl_ 设备工具区分，前者不能伪装板端执行。若服务器仍请求命令/文件批准，仅活动的用户发起回合且线程/回合一致时自动接受；恢复历史、停止、断线和非当前回合拒绝。固定工程师指导随会话传入；可选的服务器 Skills 见 `skills/serialctl-embedded-debugging/SKILL.md`，安装由服务器管理员按既有 Codex Skills 方式完成，不改写服务器认证配置。

## 有界数据与恢复

每个已连接终端保存最近至多 64 KiB 已解码设备输出和 256 个接收片段，保留原编码编号、来源、接收时间范围和序列游标。AI 每次最多读取 500 行；观察每次最多 10 秒。截断、游标缺口和断线明确返回。命令输入、状态提示和本地回显不混入设备证据。

JSONL 单帧上限 4 MiB；SSH 发送队列、UI 事件队列和对话显示均有上限。任务支持停止，连接有启动超时及健康检查。网络失败不触发设备命令重试。历史只在客户端保存服务器/用户范围内的线程 ID 和短标题，真实对话由 App Server 保存和恢复；恢复历史本身不执行串口写入；新任务只可读写用户明确关联的终端。

## 验证边界

正式构建仍使用 `scripts/build.ps1`、`scripts/test.ps1` 和 `scripts/check.ps1` 的 MSVC x64、`/MT`、6.01 子系统与 YY-Thunks 路径。Linux 交叉构建和 Wine 测试仅为补充检查，不能代替 Windows 7/10/11 运行验收。

不得将离线 Plink 进程替身测试、合成 UI 消息或协议初始化成功等同于真实 SSH、网关推理、实体串口或电源验收；现场步骤见 `docs/testing/AI-ASSISTANT-ACCEPTANCE.md`。

## App Server 兼容与服务器环境

`thread/start` 和 `thread/resume` 使用 `web_search: "disabled"` 字符串配置，线程 ID 从 `result.thread.id` 取得。响应按请求 ID 关联，无关通知不参与响应排队。原生 `turn/completed` 优先处理；对缺少该通知的网关，在最终 `item/completed`（`agentMessage`，phase 为 null/缺省/final_answer）后静默 1 秒且无待处理请求/观察/原生命令或文件项时完成。后续推理、工具或流式活动取消该兼容计时，commentary 不触发完成；迟到的已完成回合事件不能结束新任务。自定义网关若把中间回复标为最终并在 1 秒后继续工作，仍需修正网关的结束语义。

SSH 以 `sh -lc` 启动，加载该 SSH 用户的登录环境；随后可选加载 `~/.config/serialctl/codex-env.sh`，其输出与 stderr 一起隔离，不进入 JSONL。只配置在交互式 `.bashrc` 的变量可能不可见。遇到 `Missing environment variable: CONTAINER_APIKEY`，应由服务器管理员将对应 export 放在该用户 `~/.profile` 或权限 600 的 `~/.config/serialctl/codex-env.sh`，再重新连接。Windows 不接收或保存模型网关密钥。客户端不能推断缺失密钥的值，也不自动改写远端 shell 配置。

## V2.0.2 终端与电源接入

MainWindow 的 AI 回调列出所有已连接的五种终端，kind、主机、代码页均来自实际会话；终端接收路径保存有界输出，不混入本地输入/状态。CMD 通过原 SessionService 整行提交，使用实际输入代码页，强制 CRLF；其他终端沿用所选会话换行和原传输，Telnet 协商及 SSH PTY 继续由现有连接处理。不扩大 WebSocket 公开资源范围。

电源工具独立于终端选择，仅作用于本机已经连接的 IT6332A。get 返回真实状态、测量及 GUI 勾选通道，不上传本地日志路径。output 接受 enabled/reason，先查询当前状态；客户端传入已读通道、身份和模式，主窗口检查仍一致后提交至原 PowerService 工作线程。每次写后必须重查，不允许模型指定任意通道、SCPI、连接、参数或自动化操作。独立 UUID 复用现有 action 幂等机制；排队不代表执行成功，action 回读中的逐通道 confirmed/unknown 及失败保持原义。停止后不再接收新操作，已提交动作可能继续，不自动重发。

App Server 把动态工具绑定在线程创建时，当前已检查的 thread/resume 不提供动态工具替换参数。升级前创建的历史仍可恢复；使用新增电源工具需新建会话，不能假称已为旧线程注册工具。

## V2.0.2 修订包 r2

原生命令开始、输出增量、完成和文件修改转换为单项工具消息；完成时显示真实状态、退出码、输出及 diff，单项显示有界。恢复历史同时恢复服务器工具证据；不会将恢复动作视为设备操作授权。停止阻止后续设备调用和批准，已提交服务器或设备命令可能继续，不自动重发。项目路径改变隔离会话索引范围；服务器的路径不存在或权限错误保留实际服务报错。产品版本不变，原 v2.0.2 标签和资产保留，修订用 v2.0.2-r2 发布；同版本客户端需手动替换一次。
