# SerialCtl V1.0.6

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

连接页底部“全部断开”取消连接建立、SFTP/命令任务并释放本地设备；电源断开在后台处理，任务拥有的通道尝试掉电并确认，断线时输出状态不能保证。终端右键可保存网络连接诊断 JSON，或启用 RX/TX Base64 字节核对；未启用时保留常规文本日志。真实空行有时间戳，屏幕填充没有。

本包的 Win7 与真实串口/IT6332A 验收状态必须查看相应交付记录；Windows 2022 CI 不代替现场。8 小时现场运行、Win7 焦点和实际电源均需人工验收。
