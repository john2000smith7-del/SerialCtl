# 最终更新流程桌面复审

**更新失败提示与等待期间继续操作的证据通过；不能据此认定完整桌面视觉验收通过。** 本复审实际以 `view_image(detail=original)` 查看全部14张 `updater-*.png`，并核对操作JSON、HTTP日志及CMD结果文件。独立复审阶段未修改生产源码。

对象：CI `38315844534`，源码 `2dbc8a3e797a04cf63901f5c7ccbb73da46c9923`。`updater-gui.json`记录Windows NT `10.0.20348.0`；同轮 `ui-environment.json`记录1920×1080、96 DPI。采集为 **CopyFromScreen真实桌面**，操作为系统鼠标注入与SendInput，不是PrintWindow，也不是物理硬件键盘/鼠标实测。

完整原图来自上述 CI artifact；本目录的 [evidence](evidence) 保留原始日志和四张代表截图。发布包SHA256为 `72DC0926074B5968F5DD2F7FCD5B5E16E9C26E1D7DD2CA0627E1F072A7826F7D`，EXE为 `C6CFDCB844D4C4ECCC117B3384264E0ED651F61089A32FAC0B2C863EC9B4B183`；与最终包及其中EXE的独立哈希核对一致。

## 原图观察

下表每组 `before/theme` 两文件均已分别查看；before为浅色、theme为切换后的深色。

| 已查看原图文件 | 观察 |
|---|---|
| `updater-missing-before.png`、`updater-missing-theme.png` | 未配置URL时主界面正常呈现，终端空白；切换后导航、卡片与终端进入深色，无更新失败提示。 |
| `updater-empty-before.png`、`updater-empty-theme.png` | 空URL场景同样可切换主题，没有遮挡工作的更新对话框。 |
| `updater-url-a-404-before.png`、`updater-url-a-404-theme.png` | 左侧状态区完整换行显示“自动更新失败，继续使用当前版本：Update server must return HTTP 200 without a redirect”，两主题可读，未压住下方动作。提示为通用HTTP响应错误，图中文字并没有直接显示404状态码。 |
| `updater-url-b-invalid-signature-before.png`、`updater-url-b-invalid-signature-theme.png` | 两主题均可读“自动更新失败，继续使用当前版本：Update manifest signature is invalid”，文字在左卡内换行，主界面继续显示。 |
| `updater-slowheaders-before.png`、`updater-slowheaders-theme.png` | 等待响应头期间主窗与主题切换画面可见，没有更新模态遮挡；实际响应性/关闭结果来自操作记录，而非仅由截图推断。 |
| `updater-slowbody-before.png`、`updater-slowbody-theme.png` | 缓慢接收响应体期间仍可切换到深色，导航与命令区保持可见。 |
| `updater-slowbody-cmd.png`、`updater-url-a-404-cmd.png` | 深色界面显示“本地CMD #1”“已连接”，中心可见包含对应唯一标记及重定向文件路径的echo命令，之后回到CMD提示符。长路径按终端宽度换行。命令真正完成由结果TXT和JSON共同支持，不能只凭回显认定。 |

**保留的视觉缺口：** 六张深色 `*-theme.png` 及两张后续 `*-cmd.png` 中，左下“全部断开”仍保留浅色按钮填充和明显浅色矩形底，约在屏幕 `x=46–198,y=662–694`；同窗右侧按钮已经使用深色填充。它也出现在missing/empty场景，不能归因为更新失败流程。本轮只能确认这些采样画面的主题不一致，未定位重绘根因或持续时间。因此不写“全界面主题一致”“无白块”或“全部视觉问题已修复”。原审核的其他问题也不因本次更新烟测变为已关闭。

## 操作记录交叉核对

[updater-gui.json](evidence/updater-gui.json)为6/6 PASS，六次退出码均为0；正常关闭分别为34、28、32、28、58、45ms，范围28–58ms。慢响应头/体两例分别为58/45ms。此为本轮短时受控测量，不是长期性能保证。

JSON逐用例请求、汇总请求与 [updater-http.log](evidence/updater-http.log)完全一致：missing/empty无请求，404一条，非法签名两条，slowheaders/slowbody各一条，共五条；没有 `/files/` 请求。五份存在的INI前后哈希相同，六例记录不可变程序文件未改变。

[404的CMD结果](evidence/updater-url-a-404-cmd.txt)精确为 `SERIALCTL_UPDATE_GUI_url_a_404`；[slowbody的CMD结果](evidence/updater-slowbody-cmd.txt)精确为 `SERIALCTL_UPDATE_GUI_slowbody`，均匹配JSON。结合当前脚本的实际按钮点击、SendInput及文件内容等待，这支持更新失败后/慢速响应期间真实CMD仍可执行这条命令。输入内容全部为ASCII，不能扩大为中文或IME输入已验证。

## 限制

本轮是发布包在Windows Server 2022上的回环HTTP负面更新集成验证；HTTP服务为受控替身。没有验证生产签名成功升级、真实内网服务器、Win7实机、125/150/200%系统DPI或硬件。missing/empty只证明没有访问该受控HTTP服务且未创建更新暂存，不能证明全机没有网络流量。慢响应用例证明等待中可以操作和正常关闭，不能证明完整超时流程结束行为。

14张静态桌面截图既非连续录像，也未覆盖全部交互状态；**不作“无闪烁/无残影”结论**。CI全绿支持脚本断言，不自动等同原始桌面审核问题全部修复或整套UI最终验收通过。
