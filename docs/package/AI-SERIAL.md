# Linux AI 访问共享串口

Windows 上先用 SerialCtl 打开 COM3、COM5 等串口。Linux 只需 IP 和 COM 名称，无须知道实际 TCP 端口。工具会验证 SerialCtl 协议并探测 TCP 7000–7015；只列出已经打开共享的串口，不会擅自打开未使用的硬件。

把 `serialctl_client.py` 放到 Linux 工作目录（Python 3，无第三方依赖）：

```bash
python3 serialctl_client.py list 192.168.1.10
python3 serialctl_client.py stream 192.168.1.10 COM5
```

`list` 输出 JSON：实际服务端口，以及 COM 名称、波特率、数据位、Win32 parity/stop_bits 枚举、流控、客户端数和状态。旧版本服务没有元数据时对应值为 null。

```python
from serialctl_client import list_ports, open_serial

ip = "192.168.1.10"
print(list_ports(ip))
with open_serial(ip, "COM5", timeout=5) as com5:
    com5.send(b"help\r")  # 调用方明确决定换行符，工具不转换二进制内容
    print(com5.recv())   # bytes；空 bytes 表示关闭；超时抛出 socket.timeout

# 多串口可同时保持连接，每个对象各自独立，不会混入另一个 COM 的数据。
with open_serial(ip, "COM3") as com3, open_serial(ip, "COM5") as com5:
    com3.send(b"status\r")
    com5.send(b"help\r")
```

所有客户端均可读写，暂无互斥。串口关闭时其客户端断开，其他 COM 不受影响。CLI stream 将 stdin 原始字节发送到串口，stdout 输出串口原始数据；stdin 结束时连接关闭。交互式 Linux 终端输入可能自带 LF，请按设备要求转换。非默认自定义服务可传 `--port` 或 Python `port=`；一个 SerialCtl 实例共用一个服务，默认探测选最低已响应端口。

协议兼容说明：安全查询使用 `SERIALCTL/1 LIST\n`，回复以 `SERIALCTL/1 PORTS\n` 开头，逐行 COM，`.\n` 结束。新版本在其后附 `SERIALCTL_INFO\t3\n` 和八列 TSV（name/baudrate/data_bits/parity/stop_bits/flow_control/clients/state），然后关闭查询连接。旧客户端忽略扩展。命名连接使用 `SERIALCTL/2 OPEN COM5\n`，成功回复 `SERIALCTL/2 OK WRITE\n`。之后 D 数据帧或 C 控制帧使用 1 字节类型 + 4 字节大端长度 + 内容，单帧上限 1 MiB。新版不使用写入租约。单 COM 时仍兼容原始 TCP；多 COM 时原始连接被拒绝，必须先选择 COM。
