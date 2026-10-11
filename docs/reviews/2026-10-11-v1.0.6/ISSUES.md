# 问题与优化交接索引

总追踪：[Issue #26](https://github.com/john2000smith7-del/SerialCtl/issues/26)。可从下表进入单项问题，按正文的修改位置、复现与验收条件逐项修复。

所有结论固定于 V1.0.6 / `5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`。Issue 创建不表示缺陷已修复；每项后续应记录修复 SHA、实际证据和未运行项。候选风险允许以实验排除或降级，不靠投票认定。

## 当前问题

| 编号 | GitHub Issue | 证据定性 |
|---|---|---|
| F01 | [#2 [F01][P1] 电源队列满时 OFF、stop、disconnect 被容量检查提前拒绝](https://github.com/john2000smith7-del/SerialCtl/issues/2) | 已确认：原样生产Submit受控运行；实体电源未运行 |
| F02 | [#3 [F02][P1] 共享终端自动回复缺少唯一所有者，历史回放可能生成新的设备输入](https://github.com/john2000smith7-del/SerialCtl/issues/3) | 生产TerminalModel实跑及完整源码路由确认；双窗口UART端到端待补 |
| F03 | [#4 [F03][P2] events历史响应只检查result长度，完整JSON超限导致客户端断连](https://github.com/john2000smith7-del/SerialCtl/issues/4) | 已确认：生产历史函数受控实跑和公共Python解帧验证 |
| F04 | [#5 [F04][P2] 宏把入队称作执行完成，停止按钮未说明仅停止后续提交](https://github.com/john2000smith7-del/SerialCtl/issues/5) | 源码确认；慢命令真实GUI专项待补 |
| F05 | [#6 [F05][P2] 超长CMD多行粘贴先提交前缀再失败，清空原草稿](https://github.com/john2000smith7-del/SerialCtl/issues/6) | 已确认：原样生产粘贴分支+生产编辑器受控实跑 |
| F06 | [#7 [F06][P2] 日志同步导出阻塞UI，慢盘时可能触发接收溢出与连接停止](https://github.com/john2000smith7-del/SerialCtl/issues/7) | 同步阻塞路径已确认；真实慢盘数据丢失/无限挂起为候选 |
| F07 | [#8 [F07][P2] 后台会话错误未保存在会话状态，切回后只能看到通用断开](https://github.com/john2000smith7-del/SerialCtl/issues/8) | 源码确认；动态颜色刷新影响未验证 |
| F08 | [#9 [F08][P2] 终端缺少到主操作区的键盘焦点路径，主控件未设置Tabstop](https://github.com/john2000smith7-del/SerialCtl/issues/9) | 源码缺口确认；物理键盘焦点轨迹待补 |
| F09 | [#10 [F09][P2] 侧栏以物理像素保存却先按未缩放上限加载，高DPI重启宽度缩小](https://github.com/john2000smith7-del/SerialCtl/issues/10) | 源码及公式复现确认；真实125/150/200%DPI未运行 |
| F10 | [#11 [F10][P1候选] 全部断开后掉电确认失败可能丢失持续告警，需保存异步最终结果](https://github.com/john2000smith7-del/SerialCtl/issues/11) | 反馈代码链缺口确认；故障注入与实体输出未运行 |
| F11 | [#12 [F11][P2候选] 十步命令编辑窗口无工作区高度上限和内部滚动，高DPI越界待验证](https://github.com/john2000smith7-del/SerialCtl/issues/12) | 源码无高度上限；真实小工作区/高DPI未运行 |
| F12 | [#13 [F12][P2候选] WebSocket握手只有单次recv超时，持续慢滴可延长槽位占用](https://github.com/john2000smith7-del/SerialCtl/issues/13) | 源码缺少绝对deadline；Windows慢滴实测未运行 |
| F13 | [#14 [F13][P3] 共享串口卡片仍显示TCP，易误导旧用户使用已移除入口](https://github.com/john2000smith7-del/SerialCtl/issues/14) | 源码确认；双主题改后裁字待验收 |
| F14 | [#15 [F14][P2] 快速反向侧栏动画在真实桌面中间帧出现双折叠按钮](https://github.com/john2000smith7-del/SerialCtl/issues/15) | 已观察：两主题CopyFromScreen原图；根因与持续时间待定位 |
| F15 | [#16 [F15][P3] 十步上限后添加指令已禁用但仍绘为可用蓝色](https://github.com/john2000smith7-del/SerialCtl/issues/16) | 源码确认+两主题原始PrintWindow截图支持 |

## 优化与验证路线

| 编号 | GitHub Issue |
|---|---|
| O01 | [#17 [O01][优化] 逐步抽离SessionRuntime，降低设备/日志与MainWindow消息循环的耦合](https://github.com/john2000smith7-del/SerialCtl/issues/17) |
| O02 | [#18 [O02][优化] 补Publish/队列观测与多资源压力，再收短全局锁内序列化和复制](https://github.com/john2000smith7-del/SerialCtl/issues/18) |
| O03 | [#19 [O03][优化] 明确多资源历史淘汰公平性和总预算，并加强内部类型化能力边界](https://github.com/john2000smith7-del/SerialCtl/issues/19) |
| O04 | [#20 [O04][优化] 内网部署收敛监听接口与来源，明确CMD等同用户shell的信任范围](https://github.com/john2000smith7-del/SerialCtl/issues/20) |
| O05 | [#21 [O05][优化] 按需求设计可选写租约、输入来源可见性与任务完成协议](https://github.com/john2000smith7-del/SerialCtl/issues/21) |
| O06 | [#22 [O06][优化] 补协议、磁盘故障和GUI行为断言，收窄现有验收中过宽PASS表述](https://github.com/john2000smith7-del/SerialCtl/issues/22) |
| O07 | [#23 [O07][优化] 完成Win7、真实硬件/网络、高DPI与至少8小时现场验收](https://github.com/john2000smith7-del/SerialCtl/issues/23) |
| O08 | [#24 [O08][优化] 在远程串口实例歧义提示旁暴露指定端口入口](https://github.com/john2000smith7-del/SerialCtl/issues/24) |
| O09 | [#25 [O09][优化] 明确SFTP断连后的恢复与重试归属，按需要提供显式重新绑定](https://github.com/john2000smith7-del/SerialCtl/issues/25) |

## 建议顺序与关闭条件

1. 先处理 F01/F02，故障注入核实 F10；同步修 F03–F05，减少错误执行和数据/草稿损失。
2. 近期处理 F06–F09、F11–F15 与 O06，按 UI 指南验收浅/深主题、焦点和 DPI。O04 的内网信任/来源范围在部署前明确。
3. O02 先观测后优化，O01/O03/O05 分阶段演进，O08/O09 按实际用户工作流决定；每项正文记录收益、代价、依赖和风险。
4. O07 是现场验收清单。缺 Win7、实体串口/电源、真实 SFTP/LAN、高 DPI、IME 或 8 小时环境时保留“未运行”。

关闭时必须附修复提交、结果断言和原始日志/截图；候选项需记录支持或排除的实验。不要把排队成功写成执行完成，不把容量上限写成已测性能，不把 PrintWindow 的静态布局写成动态无闪烁。自动更新新功能另由功能 PR 实现，不替代本表问题修复。

[统一报告](REPORT.md) · [证据保留范围](evidence/README.md) · [人工验收](reports/09-manual-acceptance.md)
