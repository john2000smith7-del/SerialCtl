# 内网自动更新设计与部署

## 范围与兼容约束

V1.0.7 增加每次 GUI 启动时的后台更新检查。配置只来自 `serialctl.exe` 同目录 `update.ini` 的 `[update] url=`；空值停用。已知服务器 `172.16.2.188`，路径尚未确定，模板不写入猜测地址。运行中的本地设备、远程连接、CMD、SFTP、命令/电源任务不因更新被强制停止；下载完成时只有无会话、无连接建立、未打开电源页面、无未保存命令/模态编辑或拖动等占用，才启动安装；忙时删除 staging 并提示空闲后重启检查，本次运行不再尝试突然重启。同目录其他 SerialCtl/CMD 桥接/PuTTY 实例仍运行时，本次直接拒绝安装，关闭后需重新启动检查。

客户端为原生 C++17/Win32，使用 Win7 已提供的 WinHTTP HTTP 下载和 CryptoAPI RSA/SHA-256 验证；不使用 WinHTTP WebSocket、ConPTY、PowerShell/.NET 更新脚本或额外 VC Runtime。保留 x64、`/MT`、6.01 子系统、YY-Thunks、固定 PuTTY 与 NI-VISA 18 输入。正式 ZIP 继续包含完整驱动，自动更新仅发送小型运行文件，不重新安装驱动。

## 信任与清单

`src/update/UpdateTrustKey.h` 只包含 RSA-2048 公钥。部署私钥在管理员离线位置保管，不进 Git、CI、正式 ZIP 或 HTTP 目录。客户端先验证 `update.json.sig` 对清单**原始 UTF-8 字节**的签名，再解析业务字段。签名为 RSA PKCS#1 v1.5/SHA-256、256 字节原始大端格式，不是 Base64；CryptoAPI 验签内部转换其要求的字节序。

清单精确格式：

```json
{
  "schema": 1,
  "product": "SerialCtl",
  "architecture": "x64",
  "version": "V1.0.8",
  "files": [
    {"name": "serialctl.exe", "size": 123456, "sha256": "此处必须为64位小写十六进制", "url": "files/V1.0.8/serialctl.exe"}
  ]
}
```

上例仅解释字段，不能直接发布：每份清单必须至少有 `serialctl.exe`、`plink.exe`、`psftp.exe`、`VERSION`，生产签名工具会生成完整内容。版本必须为规范 `V主.次.修订`，每段 0–65535、不带多余前导零；只接受高于正在运行的版本。`VERSION` 文件也必须与清单一致。清单最大 64 KiB，最多 32 文件，单文件非空且不超过 32 MiB，总量不超过 64 MiB。

文件名严格使用客户端固定白名单。允许程序、版本、发布说明、许可证、明确的 Python AI 客户端与验收说明；不允许配置、日志、驱动、源代码或任意路径。`name` 是扁平目标文件名，URL 必须为同源相对路径，只含 ASCII 字母、数字、`.`、`_`、`-`、`/`，不得有 `.`/`..` 路径段、前导 `/`、查询串、片段、转义或冒号。中文目标文件名通过 ASCII 服务器文件名映射。自动更新不覆盖 `update.ini`，也不复制正式整包的 `MANIFEST.json`/`SHA256SUMS.txt` 冒充本次更新后的全目录清单。

HTTP 提供可用性但不提供保密；签名保护发布内容的来源与完整性。更换 URL 不等于更换信任密钥。服务端或网络可阻断更新或重放已签发旧清单；客户端拒绝低于/等于本地版本，但首次旧版安装仍可能停留在某个较旧的已签版本。没有实现在线密钥轮换、撤销、到期或“服务器必须最新版本”的证明。私钥泄露时需带新信任公钥的人工受控部署，不能只改 INI。

## 管理员生成更新

管理员机器需要 Python 3 和 OpenSSL CLI。运行 SerialCtl 的电脑和网站服务器没有这项依赖。正式发布仍先执行：

```powershell
.\scripts\release.ps1
```

保留生成包的源码 SHA、ZIP SHA-256、完整测试日志；检查人工验收边界。将该 ZIP 解压到一个新目录，再生成**新的**发布目录：

```powershell
python .\tools\new_update_feed.py `
  --package-dir C:\SerialCtlRelease\unpacked `
  --output-dir C:\SerialCtlPublish\V1.0.8 `
  --private-key D:\OfflineKeys\update-signing-private.pem
```

版本来自解压包 `VERSION`，上面的输出目录名称不是版本源。输出目录必须不存在。离线工具默认读取同一源码仓库 `src/update/UpdateTrustKey.h`，也可使用 `--trust-key-header <匹配客户端的公钥头文件>`；它检查私钥导出的公钥和客户端公钥一致，拒绝错误签名密钥。工具核对正式包的产品/版本、测试标记与每个待发布文件的 SHA-256，复制后再校验，签名后重新验证签名。`MANIFEST` 的测试标记不是独立可信认证，管理员必须选用有可追溯构建证据的包。

生成目录：

```text
update.json
update.json.sig
files/
  V1.0.8/
    serialctl.exe
    plink.exe
    psftp.exe
    VERSION
    ...批准的说明文件
```

网站只需静态文件 GET，返回真实文件字节。允许下载 `.json`、`.sig`、`.exe`、`.py`、`.md`、`.txt`、无扩展名 VERSION 与 `.bin`；IIS 如拦截未知扩展名需配置静态 MIME，不要启用服务器端执行 `.py`。不能返回登录页、HTTP 重定向或经过文本转换的内容。当前实现不弹出网站登录凭据对话框；建议更新目录只读匿名访问并由网络/主机 ACL 限制写权限。HTTPS 可以使用，但 Win7 的 TLS/证书补丁与服务器设置必须实机验证；不会忽略证书错误。

先上传版本化 `files/V.../`，再切换 `update.json` 与 `.sig`。最好在服务器端原子切换整个发布目录；若先后切换两文件导致短暂不匹配，客户端应验签失败并继续旧版，下次启动重试。不要覆盖已经发布版本目录的文件。保留旧版本目录直到客户端升级窗口结束。`update.json` 和 `.sig` 建议禁用缓存，版本化文件可缓存。

## 安装、用户数据和故障

后台下载到受控 staging，文件大小/摘要全部通过才交给更新工作进程。安装前再次验证签名、清单、文件，确认没有同目录其他实例后，等待发起更新的 GUI 正常退出，随后再次检查实例并备份替换；可处理错误回滚旧文件。检查/下载不阻塞 GUI；不自动提权，目录不可写或占用时保留旧版本并记录失败。主程序按空闲状态决定进入安装，更新器不能自行断开设备。

`update.ini`、`%LOCALAPPDATA%\SerialCtl` 中的命令/配置/日志、驱动和未列入清单的用户文件保留。正式 ZIP 白名单新增的是仓库中可编辑的空 URL 模板，不是开发者本机配置。完整包 `MANIFEST` 记录更新安全与保留规则。

安装阶段日志位于 `.serialctl-update/<随机标识>/update.log`，旧文件位于同目录 `backup/`，持久事务记录为 `.serialctl-update/transaction.txt`。替换后等待新版主窗口创建并确认事务提交；未确认或提前退出时尝试回滚。失败版本记录在 `.serialctl-update/failed-version.txt`，后续启动跳过该版本，发布更高版本后可继续升级。仅管理员确认原因已解决后，可在全部实例退出时移走失败标记重试；保留恢复记录与备份。主窗口创建确认不是全面业务健康检测。

任务退出、进程等待、磁盘满、访问拒绝和强制终止都需要回归。成功替换并启动不等于任意新版业务逻辑都健康；没有承诺自动检测所有功能退化。突然断电、文件系统损坏、第三方安全软件和真实 Win7 的恢复行为必须单独记录，不应从模型测试推断通过。

验收表见 [AUTO-UPDATE-ACCEPTANCE.md](../testing/AUTO-UPDATE-ACCEPTANCE.md)。
