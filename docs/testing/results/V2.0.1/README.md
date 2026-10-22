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

正式 Windows 构建、原生按钮像素检查、签名发布和下载回读结果随后记录。尚未验证用户内网站点、真实 Win7/10/11、实体设备或长时间运行。
