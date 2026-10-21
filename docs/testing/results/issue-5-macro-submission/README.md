# Issue #5：宏提交进度与停止语义

基线main `119ad60`，V1.0.8。进度只计成功进入共用FIFO的批次，不承诺设备或CMD执行结束。
结束原因明确分为Silent、Submitted、UserStopped、Interrupted；提交计数在清理运行状态之前形成最终提示。

- 最后一步：已提交N/N，执行结果见输出；单条常用命令同样只报告已提交。
- 用户停止：停止后续提交，保留N/N，说明已提交内容可能继续执行；不删除已接受的FIFO内容。
- 连接变化、拒绝或计时器失败：保留已提交数量和具体失败原因，明确执行状态未知，不自动重发。
- 沿用已有命令卡、按钮、进度环、主题和自适应状态区，不新增任务互斥、ACK、网络操作或运行时。

## 本地受控回归

原样提取生产SendCommand、SendNextCommandStep、StopCommandSequence，用非消费队列替代发送层，
以受控计时器、UI无操作替身和ASCII编码适配编译运行；不是Windows窗口、CMD或硬件验证。

六项检查：提交两步时模型尚未执行任何内容；重复启动不重复提交；停止保留第一批且排队的迟到timer不再提交；
断线不重发；第二批拒绝时保留数量与原失败原因；已接受第一批后的计时器失败；单命令也不承诺发送/执行结束。
其中重复启动纳入第一项，合计六个场景。基线六项语义断言均失败，修复全部通过；实际状态与队列数量见两份JSON。
旧版保留已提交队列的行为原本存在，失败点为承诺和错误状态表达，不能据此称六个不同实现缺陷。

复跑时将run_macro_probe.py复制到临时目录，从仓库根目录执行；可用MACRO_PROBE_AFTER_REF指定固定修复SHA。
它在脚本旁生成before/after C++提取程序与JSON，不修改生产文件。前版本非零退出表示预期复现失败断言。

## Windows真实窗口与CMD专项

标准scripts/smoke-ui.ps1的浅、深两个主题均调用gui_macro_submission.py，使用真实鼠标、SendInput、
只读原生控件文本查询和生产网关input/output事件，不直接发送按钮通知或替代CMD。

1. 两步宏先运行慢ping，再输出标记；界面已提交2/2时，实际完成标记尚不存在，随后真实CMD才完成。
2. 间隔5秒的宏第一批提交后点击同一卡片的Stop；界面说明停止范围，第一批仍完成；
   越过原第二步定时点后以实际CMD输出屏障核对仍只有一份GUI输入，没有第二批输出。
3. CMD自行exit使后续提交中断；核对1/2、未知状态、不自动重发，随后恢复新CMD供后续冒烟使用。

标记命令使用caret分隔实际输出标记，避免把提交事件或命令字面量误当作执行完成。
JSON保存实际输入事件、状态、时间、程序SHA-256、DPI和原屏截图摘要；截图由可见桌面BitBlt取得，不使用PrintWindow。
测试结果还输出GitHub notice，便于在无法下载artifact时读取实际状态观测，不只依赖绿色结论。

[首次Windows运行39532042684](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39532042684)
的构建/19项CTest通过，窗口专项失败，公开原始annotation为
`CLIENT_RECEIVE_QUEUE_FULL: consumer stopped reading; connection closed`。
测试曾从after=0回放此前100轮绘制基准的历史，并在真实键盘填写宏时暂停消费，触发公共客户端128事件上限。
专项修正为显式查询gap/current cursor后订阅新事件，记录起点游标；不扩大接收队列、不忽略测试期间事件。
生产宏函数未因此更改。[第二次Windows运行39543689697](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39543689697)
的构建/19项CTest再次通过，但专项报`Timed out: two GUI steps accepted`。进一步保留实际状态、输入事件、
选中行、物理点击坐标和程序摘要到失败annotation，以区分测试命中与生产路径，未放宽断言。
[第三次Windows运行39544648858](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39544648858)
公开原始失败现场保存在windows-14df501-observation.json：物理点击命中第5行、首批GUI输入已接受、
连接正常、状态仍为1/2。此前脚本未读回输入框实际值，特别是数字EDIT不应假设Ctrl+A一定全选。
改为真实Home+Shift+End选择单行，并对命令、名称和间隔逐项读回断言，禁止测试在错误间隔下继续；
未改生产宏函数或放宽三秒断言。是否为根因由下一次原生运行确认。
Windows专项、截图复审：**待运行/复核**。
Linux仅做Python语法检查和上述受控模型。
原始Actions产物下载仍受当前会话api.github.com网络策略限制，不能冒充已经看过截图。
Win7真机、物理串口/电源、其他DPI和长时测试：**未运行**。/MT、PE6.01、YY-Thunks及固定依赖输入未更改。
