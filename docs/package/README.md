# SerialCtl V1.0.1 Windows 便携包

本压缩包同时提供 Windows x86 和 x64 原生程序：

```text
windows\x86\native\
windows\x64\native\
```

32 位 Windows 7 必须使用 x86；64 位 Windows 7/10/11 推荐使用 x64，也可在支持 WOW64 的系统上使用 x86。

完整解压后，在目标架构目录直接运行 `serialctl.exe`。请始终让以下三个程序位于同一目录：

- `serialctl.exe`
- `plink.exe`
- `psftp.exe`

本程序无需安装 .NET Framework 或 Visual C++ Redistributable。串口使用前仍需安装对应 USB/串口设备的 Windows 7 驱动。

`MANIFEST.json` 描述包身份和 payload，`SHA256SUMS.txt` 用于核对包内文件。共享串口没有认证或加密，只适合可信内网。
