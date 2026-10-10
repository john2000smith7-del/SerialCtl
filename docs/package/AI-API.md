# 自动网络接口

V1.0.4 起，网络服务随 SerialCtl 启动，无需打开授权窗口、勾选权限或复制密钥。接口范围固定：查询已打开的串口、CMD、电源及其状态；读写已打开串口和 CMD；按本地当前勾选通道操作电源加电/掉电。SSH、Telnet、远程串口会话不加入新接口；旧串口 TCP 与 serialctl_client.py 继续兼容。

电源参数、保护、连接/断开、任务创建/开始/停止仅在本机操作。AI 可用脚本循环调用加电/掉电，不使用软件内置循环任务接口。API 不允许传入 channels；要改变操作通道，先在本机电源页勾选。

Linux 安装 Python 3，使用随包标准库客户端：

```bash
python3 serialctl_api.py 192.168.1.10 discover
python3 serialctl_api.py 192.168.1.10 list
python3 serialctl_api.py 192.168.1.10 send COM5 'help' --encoding gbk --ending CR
python3 serialctl_api.py 192.168.1.10 watch COM5
python3 serialctl_api.py 192.168.1.10 power
python3 serialctl_api.py 192.168.1.10 on
python3 serialctl_api.py 192.168.1.10 off
```

客户端仅需 IP。发现通过只读 SERIALCTL/3 DISCOVER 扫描 7000–7015，取得 HTTP 端口，默认 7080，占用时尝试 7081–7095。多实例或同名资源时指定 --instance、--port 或资源 ID。不要向旧串口原始 TCP 入口发送 HTTP。退出程序停止服务；断开本地串口/CMD 后，该资源不再提供收发。

接口路径从 /api/v1 开始，无需 Authorization：

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | /resources | 查询本实例资源及固定 operations |
| GET | /sessions/{id} | 查询串口/CMD 的连接、编码和换行 |
| POST | /sessions/{id}/input | 发送 {"data":"base64原始字节"}，单次 1–65536 字节 |
| GET | /sessions/{id}/events?after=0 | 获取输出、输入来源和状态事件，返回 cursor；过期时 gap=true |
| WebSocket | /sessions/{id}/stream | binary 输出原始字节，text 提供输入审计/确认；输入使用 masked binary |
| GET | /power-supplies/power-1 | 连接、测量及 selectedChannels 状态 |
| POST | /power-supplies/power-1/channels/output | {"enabled":true,"requestId":"唯一ID"} 加电；false 掉电 |
| GET | /actions/{id} | 查询电源按钮动作的排队、完成、失败与回读结果 |

本地和 AI 的 CMD 操作共享同一个进程，输入与结果在 GUI 中可见。串口流保留原始字节，日志和事件标记来源。电源输出返回 queued 时尚未完成，按 action ID 查询 completed/failed，不把排队当作已加电。

requestId 在当前实例最近 256 个动作内去重。同 ID 不同参数（包括重试时本地勾选通道变化）会拒绝，重启后旧 ID 无效。状态不明时先查询，避免重放加电。新客户端与 V1.0.3 的 Token/通道请求格式不同，使用本次压缩包里的 serialctl_api.py。

无需认证的接口用于受控局域网；不要直接映射到公网。是否允许入站由 Windows 防火墙决定。Win7 CMD 为普通用户权限的管道控制台，适合命令和脚本，不支持完整全屏控制台交互或可靠 Ctrl+C。
