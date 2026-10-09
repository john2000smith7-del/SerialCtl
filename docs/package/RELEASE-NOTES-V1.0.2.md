# SerialCtl V1.0.2 发布说明

修复中文标签显示和右侧命令/SFTP 面板边界，继续以 Windows 7 SP1 为最低目标。

- 界面统一选择已安装的中文字体：Microsoft YaHei UI、Win7 的 Microsoft YaHei、SimSun；避免不同汉字由不同字体补字。
- 连接、命令和 SFTP 输入窗口的标签使用实际字体高度，避免固定资源高度裁切中文字形。
- 折叠侧栏保留完整 48 像素卡片及额外外边距，28 像素展开按钮居中且不超过卡片边界。
- 最窄侧栏中的标签页、添加命令和折叠按钮保留共同的 8 像素间距。
- 扩大 x86/x64 双主题检查，覆盖四种连接窗口、单条命令和宏、最小窗口与最窄侧栏、命令及模拟 SFTP 折叠状态，校验标签字高和按钮边界。

下载 Assets 中的 `SerialCtl-V1.0.2-Win7.zip`，完整解压后运行：

- 32 位系统：`SerialCtl-V1.0.2/windows/x86/native/serialctl.exe`
- 64 位系统：`SerialCtl-V1.0.2/windows/x64/native/serialctl.exe`

保留同目录中的 `plink.exe` 和 `psftp.exe`。使用原有 MSVC /MT、YY-Thunks 和 PE 子系统 6.01；未替换配套 PuTTY 程序。

GitHub 自动验证运行在 Windows Server 2022。真实 Win7 SP1、不同 DPI/语言包、实体串口及现场 SSH/SFTP 验收尚未运行；模拟 SFTP 界面检查不等同于真实服务器传输测试。
