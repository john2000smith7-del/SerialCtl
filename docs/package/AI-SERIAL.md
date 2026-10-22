# AI 智能串口助手与 Linux 客户端

## Windows 原生 AI 对话

1. 点击顶部 AI，在连接弹窗输入 Linux 容器的 SSH 地址、端口（默认 22）、用户名和密码。连接测试会验证 SSH、检测 Codex 并尝试启动 App Server；首次主机指纹需要人工核对确认。
2. Linux 登录用户须已有可正常运行的 Codex CLI、模型网关和认证配置，并让非交互登录环境能找到 codex。Windows 不要求另装 Codex、模型或 API Key。
3. 建立连接后，在原终端位置对话；底部内部输入栏可选模型，Enter 发送，Shift+Enter 换行。自动发现失败时可手动填写模型 ID；推理和工具能力以实际服务响应为准。
4. 先选择关联串口。远程目标选择“连接远程串口…”，输入来源 IP，查询并选定该主机 SerialCtl 已开放的串口；来源 GUI 须先打开 COM。连接不自动选择第一个串口。
5. 点击“分析当前日志”或“持续观察”。读取范围有上限，AI 依据实际输出区分事实与推测。选中终端内容后可用 Ctrl+Shift+A 或设置中的入口附加到 AI，发送前不上传。
6. AI 需要诊断命令时，对话显示准确目标和完整命令，并直接提交至当前明确选定的串口，无需逐条二次确认。发送成功只表示入队，仍须读设备回显。多个客户端写入没有仲裁，应避免同时输入。
7. 消息支持滚轮、拖动滑块、Home/End、PageUp/PageDown；阅读旧消息时新回复不会强制跳到底部，可点击“返回最新消息”。历史由服务器保存，按当前 SSH 服务器和用户恢复。
8. 停止任务、断线或关闭 AI 不会自动重发串口命令，也不关闭原串口或电源。AI V1 不控制电源，不提供角色切换或专家团队。

密码默认不记住；勾选后使用 Windows Credential Manager，不写明文配置。SSH 主机身份变化会阻止连接。现场服务器、网关、硬件及 Windows 7/10/11 验证仍须完成，自动测试不能代替实际验收。

## 现有 Linux 客户端（SerialCtl WebSocket v1）

先在来源 GUI 打开 COM。服务自动监听 7000–7015；传入 IP 与 COM，不要使用 telnet，不用猜第一个 COM。serialctl_client.py 与 serialctl_ws.py、serialctl_api.py 同目录，Python 3 无额外依赖。

```sh
python3 serialctl_client.py 192.168.6.86 list
python3 serialctl_client.py 192.168.6.86 send COM8 'uname -a' --encoding utf-8 --ending CRLF
python3 serialctl_client.py 192.168.6.86 watch COM8
```

```python
from serialctl_client import SerialStream
with SerialStream('192.168.6.86', 'COM8') as port:
    port.sendall(b'\x00\xff\r\n')  # 不过滤合法字节；queued 不等于设备已执行
    data = port.recv(4096)
```

同一 COM 多客户端接收与输入，无独占写租约；每批次原子入队，多步任务仍须自行协调。多个实例报告歧义，用 --port/--instance 明确选择。同一协议适用于远程桌面客户端、AI、串口与 CMD；SSH/SFTP 没有开放。

实时输出 subscribe 后事件唤醒，无 HTTP 轮询。服务满队列终止慢客户端，重连须用最后 seq；HISTORY_GAP 不得伪装为完整输出。串口未打开、目标消失、实例错误、协议错误、CLIENT_LIMIT 等分别报错。旧 SERIALCTL/1/2/3 与 REST 不再兼容，必须升级同包客户端。

首次 SerialStream / watch 连接遇到已淘汰历史会在 stderr 明确报告 HISTORY_GAP，然后开始实时接收；不把旧历史当作完整。重连使用 Client.subscribe(resource, last_seq) 或 watch --after N，缺口严格报错，由操作者决定是否新建实时订阅。
