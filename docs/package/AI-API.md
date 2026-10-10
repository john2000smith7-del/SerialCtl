# AI API

点击“AI API”，选择允许访问的资源及读取、终端输入、电源输出、参数、任务、连接权限和允许的电源通道，再启用。更新授权会生成新密钥并关闭旧网络连接；关闭 API 不关闭本地终端或电源。新打开的会话需重新授权。端口默认 7080，占用时尝试 7081–7095。

现有 `serialctl_client.py` 和旧 TCP COM 协议保留。API 窗口中的“兼容串口 TCP（无密钥）”可独立关闭，关闭后旧客户端会断开；本地串口继续工作。新 API 的 Token 不保护仍启用的旧 TCP。CMD、电源及 SSH 只经授权 API 提供。

远程 Linux 安装 Python 3，无需第三方模块：

```bash
export SERIALCTL_TOKEN='从本地 API 窗口复制的密钥'
python3 serialctl_api.py 192.168.1.10 discover
python3 serialctl_api.py 192.168.1.10 list
python3 serialctl_api.py 192.168.1.10 send COM5 'help' --encoding gbk --ending CR
python3 serialctl_api.py 192.168.1.10 watch COM5
python3 serialctl_api.py 192.168.1.10 on 1 2
python3 serialctl_api.py 192.168.1.10 off 1 2
```

发现使用只读 `SERIALCTL/3 DISCOVER`，扫描 7000–7015，合并同一实例。仅电源/CMD也可发现。多实例或重名时要求使用 `--port`/`--instance` 或资源 ID，不自动选最低端口。HTTP 只发送到返回的 API 端口，避免被旧单串口入口当成设备数据。未经授权不自动退回旧匿名写入。

API 路径均从 `/api/v1` 开始，带 `Authorization: Bearer <token>`。GET `/resources` 只显示授权资源；GET `/sessions/{id}` 查询终端编码/换行；POST `/sessions/{id}/input` 使用 `{"data":"base64原始字节"}`；GET `/sessions/{id}/events?after=0` 轮询字节和输入来源事件，返回 cursor，历史已过期时 gap=true。WebSocket `/sessions/{id}/stream` 输出 binary 原始字节及 text 审计/确认，输入用 masked binary。单次输入最大 64 KiB。网络操作在本地会话显示状态，CMD 输入也显示在终端，日志保留 AI 来源；串口原始数据不添加 AI 前缀。

GET `/power-supplies/power-1` 查询状态；GET 同路径下 `/usb-resources` 枚举 USB；POST `/connect`、`/disconnect`、`/channels/output`、`/channels/parameters`、`/task`、`/stop` 提交动作。输出请求 `{"channels":[1,2],"enabled":true,"requestId":"唯一ID"}`；参数使用 channels、voltage、current；任务使用 channels、onMs、offMs、count。GET `/actions/{id}` 查询执行结果。返回 queued 表示已排队，completed 表示已完成回读。动作按通道返回 confirmed、unknown、skipped；批量顺序执行，不保证原子或同时变化。

空通道、越界、未授权通道在写入前拒绝。requestId 在当前实例保留的最近 256 个动作范围内去重，同 ID 不同参数拒绝；过期动作不能作为重试保证，重启后旧 ID/Token 无效。操作未知时先查询实际状态，避免重放加电。资源 ID 属于当前实例，不能跨实例或跨重启缓存使用。

当前传输为带 Token 的 HTTP/WS，适用于可信局域网或 VPN。跨不可信网络需通过 VPN/TLS 网关，不直接暴露公网。Win7 本地 CMD 使用普通用户权限和管道，不是 ConPTY，不支持全部全屏程序或可靠 Ctrl+C；关闭会话结束进程树，断开订阅保留会话。

`POST /api/v1/power-supplies/power-1/channels/protection` 使用参数权限，JSON 为 `{"channels":[1],"enabled":true,"voltageLimit":12,"currentLimit":2}`。电压保护写入设备 OVP 并回读，电流上限为软件采样保护。命令返回 action，需按 action ID 查询完成或失败。
