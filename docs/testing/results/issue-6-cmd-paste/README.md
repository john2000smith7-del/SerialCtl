# Issue #6：CMD完整粘贴预检的补充验收

当前main基点`b945afec626393becb007936cef2d1d18af59681`，V1.0.8。
生产修复已由`8ecf183`引入，本次不重复改写PasteToTerminal或CmdLineEditor；只补测试和验收记录。
原始问题基点为`5f12678e4da327f146edb5ce12eb3b3ea9c8afd0`，不能将旧版缺陷直接当作当前状态。

## 源码与受控证据

- `src/app/MainWindow.cpp::PasteToTerminal`有界读取CF_UNICODETEXT；CMD最多65536 UTF-16单元。
  PreparePaste先合并原草稿/光标、检查所有完整行和尾行；再严格编码整个计划（包含未完成尾行），
  用生产SessionService::CmdBatch检查8191字节单行及规范化后65536字节批次。
  只有完整前缀入队成功后才CommitPaste，失败不改草稿、光标或历史。
- `src/terminal/CmdLineEditor.h::PreparePaste/CommitPaste`分离只读预检和提交；
  `src/network/SessionService.cpp::CmdBatch`规范化CR/LF/CRLF并拒绝控制字节和过长行。
- `tests/unit/TerminalModelTest.cpp::TestCmdPasteBoundaryMatrix`覆盖三种换行、三处光标与
  8190/8191/8192尾行的27种组合；核对预检不变、有效提交、后缀/光标和历史；补NUL/BS/ESC/DEL拒绝。
  本地完整TerminalModelTest已通过，使用Linux g++，不是Windows窗口。
- `run_paste_probe.py`原样提取生产CMD粘贴分支、编辑器和CmdBatch，以UI无操作、非消费发送队列和
  受控UTF-8编码器替身编译实跑。原始报告用例在原版本失败（前缀已接受，草稿丢失）；
  当前41项通过，包括上述边界、无换行不补Enter、编码拒绝、UTF-8尾行超字节上限、
  前缀发送拒绝保留草稿/光标/历史，以及规范化65536/65537/65544字节批次。
- 删除整个计划的编码/批次预检后，临时变异程序41项中3项失败，说明检查能检出回归。
  接收事件数不等于入队批次数：生产ApiServer::Publish会按16384原始字节拆事件。

复跑受控程序：先将run_paste_probe.py复制到独立临时目录，再从仓库根目录运行；
可设置PASTE_PROBE_REF为完整固定SHA。它在脚本旁生成提取C++、二进制和结果，不修改生产文件。
受控发送/编码器不证明真实Windows队列饱和或WideCharToMultiByte行为，必须与下面的原生专项区分。

## Windows真实窗口专项

标准smoke-ui.ps1在浅深主题均调用gui_cmd_paste.py；沿用真实鼠标/SendInput、只读绘制长度和
生产网关input/output事件。显式当前游标订阅，避免旧历史干扰；每项以真实CMD输出屏障确认此前FIFO数据
已经经过，不用固定短睡眠推断“没发送”。截图用可见桌面BitBlt，不用PrintWindow证明交互或无闪烁。

1. 27种换行/光标/尾行组合：有效前缀仅有预期GUI字节、8190/8191尾部仍为草稿；
   8192拒绝时GUI输入为0，再在原光标插入KEPT并显式Enter，用精确输入字节证明原草稿及光标均保留。
2. CR/LF/CRLF有效多行：真实首行输出、异步远程stdout不改尾部草稿，只有显式Enter才执行尾行。
3. UTF-8超8191字节尾行、65537 UTF-16单元剪贴板、尾部控制字符和真实CP437不可表示字符均拒绝整个计划；
   原草稿/光标保留。UTF-8的2730个中文字符（8190字节）尾行应接受为草稿。
4. 七行8191和末行8184字节以LF分隔：规范化超过65536字节，整批拒绝。
   末行8183时规范化恰为65536字节，应接受一次完整字节序列；事件可能拆为四块，不误计为四次执行。
   这些大行使用无副作用REM，测试不会执行真实设备命令。

[首轮运行39547399724](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39547399724)
已完成真实窗口专项，公开notice中可见两主题的前段边界/光标/零前缀输入记录，但长JSON被GitHub公开页面
截断在约4KiB，不能据此宣称后半段原始记录已独立读回。随后把65537字节用例隔离为空草稿，确保完整计划
恰好超过一字节；再增加不超过3800 UTF-8字节的完整场景摘要，仍在artifact保存详细事件和截图。
摘要长度用合成37场景的上界数据在Linux检查为3663字节；不是Windows功能验证替代。
[摘要重跑39548579895](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39548579895)
构建/19项CTest通过，但在粘贴专项之前，宏编辑器读回报末尾SLOW仅显示S：
`Macro field 1102 differs: ..._^S != ..._^SLOW`。SendInput仅保证注入排队，立即WM_GETTEXT可能先于
最后几个键处理；宏字段读回改为等待完整原生文本，保持严格相等、不重打字符。永久缺字仍失败。
本次未改生产代码、宏或粘贴断言。[最终运行39549514414](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39549514414)
在main `0fb9a60fdd4f4392a732012d9ece401e6715137a` 全部成功：构建/19项CTest、真实窗口、
更新窗口、Python客户端和更新签名工具回归及产物上传。

两主题各37项的完整公开摘要已独立读回并用verify_native.py复核：27种尾行/换行/光标组合、
3种有效多行/异步stdout/显式Enter、7种编码/控制字符/剪贴板和规范化批次边界。
真实DPI均为96；程序SHA-256为`3563f434ceca7f13659c55e00f200990f4432a2129cd65eb3a1a8d60df15e8a4`。

- 8192尾行的9种拒绝均为前缀GUI输入0，随后原草稿在相同光标插入KEPT并显式Enter的完整输入字节相符。
- CP437不可表示、UTF-8超字节上限、尾部ESC及超65536单元剪贴板同样保留原草稿/光标，未提前提交前缀。
- 65537字节规范化批次实际GUI输入0；65536字节实际输入65536，分4个事件块，拼接SHA-256为
  `e0a6455abc1f62d782195e3f659a0cfe44b044e20dfa6c89e014895db609fc08`，两主题均与预期REM批次完全一致。
- 三种普通换行首行只提交一次，异步真实stdout不改尾部草稿，显式Enter之后才有尾行结果。

Windows功能专项：**已通过**，详见windows-0fb9a60-observations.json。
生产修复仍为8ecf183；本轮只是测试和证据补强，不重复计算为新生产修复。
verify_native.py可复制到临时目录，与下载的公开运行页windows-39549514414.html同目录运行；
它重新校验两份原始摘要中的全部场景集合、精确字节数、草稿样本与实际输入SHA。前缀队列拒绝仅在受控提取程序中运行；没有伪造真实窗口饱和结论。
Win7真机、物理串口/电源、其他DPI和长期运行：**未运行**。
当前会话api.github.com策略仍阻止下载Actions原始产物，后续若有截图也不能宣称已独立视觉复审。
