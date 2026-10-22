# V2.0.1 顶栏、主题按钮与间距修复

顶部导航框结束于设置之后，主题操作独立在右侧。顶部可见框底部至主卡片的距离由 28 改为 12 个逻辑像素，与左右卡片间距相同，布局与背景绘制共用同一几何定义。

主题按钮圆角外使用窗口底色，保留自身焦点边框；主窗口原生 BUTTON 的背景刷也按主题按钮/卡片按钮返回对应当前配色。公共按钮恢复逻辑补充 BM_CLICK 和 WM_CHANGEUISTATE。此前默认 UI 烟测发送父窗口 WM_COMMAND，未覆盖原生按钮按下、释放和键盘路径；新增正式程序的原生鼠标消息、空格键和 BM_CLICK 测试，检查操作确实切换主题且焦点保留，同时读取可见桌面的主题按钮圆角和底部“全部断开”按钮背景。DWM 同步不强制程序重绘；不据这些采样宣称所有中间帧都绝无闪烁。

随包 update.ini 预填用户明确确认的 `http://172.16.3.188/external/serial/update.json`。打包检查只允许这一版本化非秘密模板；已有用户 INI 在线升级时仍保留。UI 隔离副本会清空 URL 以避免访问用户内网，正式压缩包中的模板不变；自动更新烟测继续使用受控 HTTP 服务。

## 本地验证

- LLVM-MinGW x64 交叉编译通过。正式 MSVC /MT、YY-Thunks、PE 子系统 6.01 设置保留。
- Wine 8/Xvfb：ai_pane、ui_buffered_paint、ui_combo_paint 三项通过，见 [wine-tests.txt](wine-tests.txt)。
- 实际程序 1180×760、980×620 窗口中左、上、右卡片间距均为 12；原生鼠标消息、空格和 BM_CLICK 反复切换，主题圆角和底部按钮当前配色检查通过，见 [wine-native-theme.txt](wine-native-theme.txt)、[wine-minimum.txt](wine-minimum.txt)。
- 检查实际应用的[浅色](wine-light.png)和[深色](wine-dark.png)可见桌面截图。截图裁出真实客户区；环境为无窗口管理器的 Wine/Xvfb，不是设计重绘，不代表真实 Windows 物理鼠标或其他 DPI 验收。
- PowerShell C# 正式窗口烟测扩展编译通过；SourceOnly 版本/依赖及 24 项 Python 发布工具回归通过。Linux 下 git.exe 来源检查未运行，由正式 Windows 构建补足。

## 正式 Windows 构建与发布

- 源码 `acc269256650e57db882fcf0e391a486aade331e`，[Windows Server 2022 构建 39735381642](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39735381642) 成功。23 项 CTest、MSVC x64、/MT 和 YY-Thunks 配置、PE 子系统 6.01 与 Win7 导入检查、打包白名单、完整 ZIP 重开验证通过。
- 正式 ZIP 提取出的程序在深浅主题下通过新增导航边界、独立主题按钮、12 像素等间距，以及原生鼠标消息、空格、BM_CLICK 之后圆角和底部按钮配色检查，焦点保留。既有弹窗布局、受控 CMD、侧栏、隔离 SFTP 和自动更新启动烟测也通过；10 项 Python 客户端及 24 项签名工具测试通过。见 [windows-build-summary.txt](windows-build-summary.txt)。
- [发布流程 39737006241](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39737006241) 重新取得上述构建的精确 ZIP，核对 SHA-256 并验证内嵌签名 JSON 完全描述该 ZIP 后发布。[正式版本 v2.0.1](https://github.com/john2000smith7-del/SerialCtl/releases/tag/v2.0.1) 设为最新，标签指向上述源码，资产恰好两个。见 [publish-summary.txt](publish-summary.txt)。

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| SerialCtl-V2.0.1-Win7-x64.zip | 130,238,010 字节 | `48a4cd76a9562c464d658efa76fb7c977c3c2ce3adfb821db651dbe7e61efcd1` |
| update.json | 4,470 字节 | `67d056c9c3979735a2cb5129c15badd0c9cf1b4f071c236d38e33d84dd1167bf` |

## 发布文件独立回读

从正式 Release 重新下载两个文件，验证内嵌签名和完整 ZIP 摘要；重开全部 322 条目验证 CRC、每项 SHA256SUMS、VERSION、干净源码提交、三份 x64/6.01 PE。serialctl.exe 中编译的公钥与源码和签名身份相同；update.ini 确认预填用户批准的网址，包内无私钥、开发源文件、PDB/OBJ 或任意本地配置。见 [published-zip-verification.json](published-zip-verification.json)。验证后的同一组下载文件放入 bin，原 V2.0.0 两文件移入工作目录备份。

沿用用户授权的 V2.0.0 密钥，没有重新生成。公钥 SPKI SHA-256 为 `196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a`，私钥保持在仓库外，未上传 CI 或发布包；[production-signing.json](production-signing.json) 仅含公开回执。

实际发布的 JSON 和 ZIP 还交由无测试宏、无外部公钥覆盖的生产验签/提取/暂存代码验证，LLVM-MinGW/Wine 下通过：内置原公钥接受真实签名，只提取 24 个运行文件，不提取驱动或 update.ini，删除 ZIP 后再次核对暂存文件；改变签名一个十六进制字符被拒绝。未执行包内程序或安装更新。见 [native-production-trust.txt](native-production-trust.txt)，保留 Wine 缺少 winedevice 服务的环境提示，不将该补充检查称为实机 Windows 验证。

用户在 `/external/serial/` 先上传完整 ZIP，再替换配套 update.json；不解压或改名。V2.0.0 配置正确网址后可自动升级，原 INI 按既有保护规则保留，空白或旧网址须改一次；更早协议的客户端需手动安装 V2 系列一次。

未验证用户内网站点、正式签名的现场在线安装、真实 Win7/10/11、实体串口/电源、真实 SSH/Gateway、其他 DPI 或长时间运行。受控 CI、原生消息和 Wine 验证不代替这些现场验收。
