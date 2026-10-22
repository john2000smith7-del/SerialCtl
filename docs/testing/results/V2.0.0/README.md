# V2.0.0 双文件自动更新验证

## 已完成

- 生产候选源码：`7adbbad23f297d306a16597b000c3a8b5552e517`，分支 `feature/two-file-update-v2`。
- [Windows 正式构建 39722974470](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39722974470) 全部成功。23 项 CTest、MSVC x64、原 /MT 和 YY-Thunks 配置、PE 子系统 6.01/Win7 导入检查、包白名单及 ZIP 重开验证通过。
- Windows 原生提取实际 V2.0.0 完整 ZIP 通过：临时测试密钥签名、验签后只提取 24 个运行文件，未提取驱动或 update.ini；不执行包内程序，不使用生产私钥。
- 打包程序启动/退出、深浅主题和弹窗布局、受控 CMD、侧栏和隔离 SFTP 回归通过。启动自动更新烟测通过；10 项 Python 客户端测试、24 项签名发布工具测试通过。日志摘录见 [windows-build-summary.txt](windows-build-summary.txt)。
- 构建产物内的 `SerialCtl-V2.0.0-Win7-x64.zip`：130,237,765 字节，SHA-256 `aa7695d290b822e38cfbde0f9448a3a220f6229eb5ed8533dc4dd5c84139f1a3`。Actions 产物 ID `12402735574` 是外层下载容器，不能把容器摘要误作上述发布 ZIP 摘要。

## 本地补充

- LLVM-MinGW x64 交叉编译和 SourceOnly 版本/依赖检查通过。SourceOnly 的 git.exe 来源检查在 Linux 中未运行，由 Windows CI 补足。
- Wine 下更新客户端全套通过；覆盖两个 HTTP 请求、stored/DEFLATE 和动态 Huffman、提取后删除临时 ZIP、签名/CRC/摘要篡改、路径穿越、配置和驱动保护、同版不下载。旧协议回归保留。
- Wine 原生提取实际 V1.10.0 完整发布 ZIP 通过，验证已有 PowerShell 包的路径和压缩格式。测试使用临时密钥，不是正式更新签名，见 [wine-real-archive.txt](wine-real-archive.txt)。
- Wine 安装测试在旧协议第一个真实子进程案例未通过，未将其记为成功；正式 Windows 同套安装测试通过。保留原始 [wine-update-tests.txt](wine-update-tests.txt)。Python 发布工具本地 24 项通过，见 [python-feed-tests.txt](python-feed-tests.txt)。

## 交付阻塞与现场边界

上述构建是新密钥授权之前的候选证据。用户随后授权为 V2.0.0 建立新签名身份，签名私钥已在源码及发布目录之外创建并限制权限；公钥已更新。必须以新的源码 SHA 重新运行正式 Windows 构建，通过后使用真实私钥签名对应的精确 ZIP。下述缺少原私钥是此前状态，不应误作新身份仍未创建。

当前环境只有 `UpdateTrustKey.h` 中的验签公钥，没有匹配的生产签名私钥。现有权限也无法确认或使用 GitHub 中可能存在的签名秘密。因此尚未生成生产签名 update.json，未发布 V2.0.0 正式 Release，未把临时测试签名或未签名候选描述交付给用户。当前正式版本仍是 V1.10.0。

需落实原私钥的安全访问位置，或明确安排新密钥及长期安全保管，再完成真实签名和精确两文件交付。若改动内置公钥，必须重新构建并验证对应客户端。第一次从 V1.10.0 或更早版本迁移需要手动部署。

未运行：真实 Win7/10/11 实机、用户内网更新服务器、正式签名更新现场、断电/磁盘满、实体串口/电源和长时间运行。Windows Server 2022 CI、Wine 和受控 HTTP/文件替身均不能替代这些验收。网络策略未允许当前执行环境取回 Actions/文件服务的二进制，此记录中的 V2 ZIP 摘要及验证来自成功 CI，未宣称本地再次下载复核。
