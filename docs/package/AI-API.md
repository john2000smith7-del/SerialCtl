# AI 接口（与远程串口相同的 WebSocket v1）

```python
from serialctl_ws import Client
with Client('192.168.6.86') as client:
    print(client.resources())
    session = client.resource('COM8')
    client.subscribe(session)
    print(client.input(session, b'uname -a\r\n'))
    event = client.next_event(session)  # 阻塞直到输出/输入/状态到达
    print(event)
```

查询现有 CMD：从 resources 明确取 kind=cmd 的 ID，再 session.get 查询 inputCodePage/outputCodePage；编码匹配后提交 CRLF 完整命令行。输入响应 accepted/state=queued/execution=unknown，不是执行成功。服务不提供网络建立/断开本机会话。

电源仅 `power.get`、`power.output(enabled)`、`action.get(id)`。输出操作只使用本机 GUI 勾选通道；请求不得替换 channels，也不得设置参数/保护/模式/预设/复位/SCPI/任务。

```python
with Client('192.168.6.86') as client:
    action = client.output(False, request_id='unique-off-request')
    print(client.wait_action(action))
```

电源相同 requestId 的原有 action 语义保留；断线后先 query action.get，不能自动重发状态不明的操作。终端输入的响应缓存只在当前连接内最近256次有效。额外权限一律 OPERATION_DENIED，软件启动不需要授权页或 API 开关。手册协议详见源码 docs/architecture/WEBSOCKET-PROTOCOL.md。
