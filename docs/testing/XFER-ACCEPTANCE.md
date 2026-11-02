# Xfer 验收与边界

本轮入口是右侧“命令／SFTP／Xfer”，仅已连接的本地串口或支持能力协商的远程串口可用，提示“串口传输文件”。实际设备端必须具有相应协议实现，本文件不宣称任何板卡已实测。

## 软件回归

- TransferProtocolTest：CRC公开向量、二进制边界、空文件、序号回绕、坏CRC、丢ACK重传、危险名称、长度溢出／上限、取消，以及固定种子的1200组畸形字节流。该核心可独立用ASan／UBSan运行，不等于Windows或UART验收。
- QueuedTransferTest：在实际堵塞的普通写之后排队旧U-Boot按键，收到提示符后丢弃旧任务；测试传输独占、错误owner拒绝、原样二进制、私有接收和尾部恢复。
- TransferControllerTest：在Windows上使用生产ApiServer、SessionService、SharedSerialConnection及文件控制器，通过受控传输端测试四模式双向、逐字节文件核对和普通输入恢复；另外验证手动完成边界、阻塞写取消、断线、空部分文件清理和拒绝覆盖。不是COM驱动或物理板卡。
- xfer_ui_fixture.py：运行最终ZIP中的主程序，用实际鼠标和SendInput操作远程串口发现、协议选择、原生文件对话框和收发，保留双主题原始桌面截图。受控端人为按输出／保存块加入10ms节奏，以观察传输中连续点击和拖动；不能据此推断真实波特率吞吐量。

独立互操作脚本protocol_interop_posix.py与XferInteropProbePosix.cpp使用完整双工socketpair，对比生产协议核心与独立lrzsz可执行程序，包含X／Y／Z、两个方向和1／128／12345／65539字节共24组。Repository policy在Linux自动执行；XMODEM发送的0x1A补齐明确计入预期，不假称所有模式都自带文件长度。

测试参考程序来自Debian官方lrzsz_0.12.21-10+b1_amd64.deb，SHA-256为60c15258a977b837671f99f60a7876b1dfa7cebd9ed1a7dcd16d922b6e1b9cfe。CI先核对包哈希再提取，不链接、移植或随产品分发参考程序或GPL实现代码。脚本接收probe、reference-dir和新的output目录参数，结果及两个进程的原始日志保存在该目录。

## 必须单列的实机检查

真实Win7 SP1、USB／PCI串口适配器、低波特率／硬件流控、拔插、现场远程网络、设备固件命令、设备实际落盘和上电打断都须单独记录。现有软件参考／回环结果不能代替这些测试。协议握手确认也不等于板端业务或烧写已经完成。

接收每次单文件，拒绝批次第二文件和覆盖现有文件；不支持YMODEM-G或ZMODEM跨任务续传。原始模式没有校验和握手，应先在PC开始接收再触发设备发送，未知长度手动完成。XMODEM未知长度保留补齐；接收上限4GiB，ZMODEM文件小于4GiB。失败时保留非空部分文件，必须检查内容后再使用，不把部分文件当成功文件。
