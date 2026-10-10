# AI 串口接入（SerialCtl WebSocket v1）

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
