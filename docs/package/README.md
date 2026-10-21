# SerialCtl V1.0.9

Win7 SP1 x64 原生便携程序。解压后 serialctl.exe、固定 plink.exe 与 psftp.exe 保持同目录；Windows 主程序不需要 Python、.NET 或另外安装 VC Runtime。NI-VISA 18 完整离线驱动保持 drivers 目录，仅电源 USB 访问需要相应驱动。

软件启动自动提供单一 WebSocket v1 服务（TCP 7000–7015）。用户先在 GUI 打开 COM 或 CMD。远程串口填写 IP、明确 COM，通过相同服务发现和连接。多个实例需要指定端口；不猜测第一个串口，不再支持 telnet/IP:7000 串口透传或旧 HTTP REST。

Linux Python 3 标准库客户端：serialctl_client.py、serialctl_api.py 是同一个 serialctl_ws.py 核心的入口，必须放在同一目录。

```sh
python3 serialctl_client.py 192.168.6.86 list
python3 serialctl_client.py 192.168.6.86 send COM8 'uname -a' --ending CRLF
python3 serialctl_client.py 192.168.6.86 watch COM8
python3 serialctl_api.py 192.168.6.86 power
python3 serialctl_api.py 192.168.6.86 on
python3 serialctl_api.py 192.168.6.86 off
```

只有 IP 和 COM 即可发现并选择；歧义时用 `--port 7001` 或 `--instance <返回值>` 明确实例。发送返回 queued，不能等同于命令执行成功；持续 watch 无轮询。电源操作只作用于本机当前勾选通道，无远程参数、SCPI 或任务接口。更多示例见 AI-SERIAL.md 和 AI-API.md。

CMD 支持本地左右移动、Backspace/Delete/Home/End、历史、中文、复制粘贴和多行提交。网络必须发送完整行，按 session.get 查询的代码页编码。隐藏 Win7 控制台输入桥接支持多字节字符，stdout 保留原始管道；本轮不支持 Ctrl+C 和全屏交互，全部断开会关闭 CMD 进程树。后台输出不强制回到底部，主动键入/执行命令回到实时位置。

终端拖选完成后自动复制，双击选词同样复制；右键直接粘贴一次，不额外追加回车。剪贴板含换行时，CMD 会提交完整行并保留末尾未换行的草稿。原有复制粘贴快捷键继续可用。顶部“设置”包含回显、时间戳、编码、换行方式、日志、诊断和终端操作，按全局与当前会话区分。

连接页底部“全部断开”取消连接建立、SFTP/命令任务并释放本地设备；电源断开在后台处理，任务拥有的通道尝试掉电并确认，断线时输出状态不能保证。顶部“设置”可保存网络连接诊断 JSON，或启用 RX/TX Base64 字节核对；未启用时保留常规文本日志。真实空行有时间戳，屏幕填充没有。

本包的 Win7 与真实串口/IT6332A 验收状态必须查看相应交付记录；Windows 2022 CI 不代替现场。8 小时现场运行、Win7 焦点和实际电源均需人工验收。

## AI 智能串口助手

点击顶部 AI，填写 Linux Codex 服务器的 SSH 地址、端口、用户名和密码，测试后进入对话。Windows 不需要安装 Codex；模型和网关认证沿用服务器配置。密码默认不保存，选择记住时由 Windows 安全凭据存储；首次连接须核对 SSH 主机指纹。

对话输入栏位于原终端区域内部底部，可选模型、发送和停止；消息支持上下滚动和历史恢复。串口由用户明确选择，不默认本地 COM；选择“连接远程串口…”后填写来源 IP，查询其已经开放的串口并连接。AI 可读取真实日志、观察新输出，命令须逐次确认，提交不代表设备执行成功。不增加跨客户端写入仲裁。使用步骤与原 Linux 客户端说明见 AI-SERIAL.md。

尚未完成真实 SSH/网关/实体串口及 Windows 7/10/11 现场验收，不能据自动测试宣称已完成这些验收。

## 内网自动更新

编辑程序旁 `update.ini` 的 `[update]` / `url=`，填入服务器完整 `update.json` 地址；默认留空，不预设尚未确定的服务器路径。每次启动后台检查，发现更高版本并验证签名后，在没有会话、连接建立或任务时更新并重启。服务器不可达、下载失败或校验失败不妨碍当前版本运行。更新保留 `update.ini`、用户命令、日志和 NI-VISA 驱动。详细操作、日志和恢复办法见 `AUTO-UPDATE.md`。

第一次使用须手动部署带更新器的 V1.0.7；V1.0.6 及以前不支持自动更新。更新功能没有修复本次独立审核登记的全部历史问题。
