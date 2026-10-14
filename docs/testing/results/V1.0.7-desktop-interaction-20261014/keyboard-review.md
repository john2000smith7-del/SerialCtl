# CMD 真实键盘输入独立复核

复核对象：`attempt-8/x64-light-cmd-real-input.json`，以及当前 `tests/integration/gui_cmd_user_input.py` 的输出匹配修正。此复核不修改生产代码，也不把第 8 次运行的整体 FAIL 改写为 PASS。

第 8 次运行已证明首条编辑后的命令确实执行；失败来自测试对 CMD 提示符边界的错误假设。

- `edited.txt` 的记录字节为 `KEY_EXEC_F18D7A55706D_OK\r\n`，重新计算 SHA-256 与报告一致。测试只预建空目录，文件内容由键盘提交的 `echo ...>文件` 生成，故不能仅靠界面回显满足这一断言。
- WebSocket 事件 465 为提交的 `echo` 输入，468 为提交的 `type` 输入；它们没有纳入输出匹配。事件 469 明确属于 `output`，内容独立等于 `KEY_EXEC_F18D7A55706D_OK\r\n`。
- 事件 467 是没有尾部换行的 `D:\a\SerialCtl\SerialCtl>` 提示符。把它和 469 拼接后，标记前并非行首或换行，原正则因此误报“缺少独立输出行”。当前修正保留完整标记及 CRLF，只取消前置行首要求，符合实际 CMD 输出行为。
- 本次所有字符及按键前记录的焦点均为终端 HWND `328158`，前台均为主窗口 `131526`，归属 PID `2944`；输入轨迹包含鼠标点击和 Home、Right、Left、Delete、End、Backspace、Enter。最终提交的命令已经从刻意输入的 `echX` 修正为 `echo`。

修正后的误判风险较低：目录与标记每次唯一，订阅从执行前游标开始，必须同时通过文件精确字节断言和仅来自 `output` 的标记加 CRLF 匹配。写文件命令在标记后还有重定向目标，`type` 命令没有标记文本，因此这些命令即使被 CMD 回显，也不能单独满足当前标记匹配。文件 SHA-256 是报告内字节一致性检查，不能单独代替运行时文件读取；本脚本同时做了运行时文件读取断言。

剩余边界：第 8 次运行在首个输出检查停止，尚未执行环境变量和工作目录保留验证，需以第 9 次原始证据为准。脚本依赖既有活动空闲 CMD 与“最后一个 connected CMD”对应；若运行时存在其他操作方，报告不能单凭 HWND 证明 WebSocket 资源就是当前标签。唯一文件和标记显著降低串台误通过概率。键盘序列验证组合后的执行结果，并非逐键截图或逐键光标位置断言；ASCII 的 `KEYEVENTF_UNICODE` 输入也不等于真实键盘扫描码布局或中文 IME 验证。`popd` 与清理环境变量会实际提交，但当前没有单独断言恢复后的原目录及变量缺失，因此不能把清理动作的提交扩大解释为恢复状态已独立验证。

第 9 次原始证据已完成下述补核；此前仅有执行日志的状态已更新。


## 第 9 次原始证据补核

已直接读取 `attempt-9/x64-light-cmd-real-input.json`、`attempt-9/x64-dark-cmd-real-input.json`，逐项解码 `files` 与全部 `kind=output` 事件，并打开浅、深主题 before/after 共四张原始桌面 PNG。两个报告均为 `PASS`、退出码 0，均保留六项通过检查；浅色是 `session-1`，深色是 `session-2`，GUI PID 均为 1100。

| 主题 | 唯一标记后缀 | 文件与原始 stdout 交叉核对 |
| --- | --- | --- |
| 浅色 | `ED53963E3305_OK` | `edited.txt` 恰为 `KEY_EXEC_ED53963E3305_OK\r\n`，`relative.txt` 恰为 `CD_EXEC_ED53963E3305_OK\r\n`，`state.txt` 恰为 `STATE_EXEC_ED53963E3305_OK\r\n`；三项 SHA-256 均一致 |
| 深色 | `4E628F89D4B7_OK` | `edited.txt` 恰为 `KEY_EXEC_4E628F89D4B7_OK\r\n`，`relative.txt` 恰为 `CD_EXEC_4E628F89D4B7_OK\r\n`，`state.txt` 恰为 `STATE_EXEC_4E628F89D4B7_OK\r\n`；三项 SHA-256 均一致 |

表中的 `\r\n` 表示实际的 CR、LF 两个字节。六份文件内容的摘要已重新计算，均符合报告。浅色有 19 条输出事件，深色有 20 条输出事件；每主题的 KEY 文件内容在原始 stdout 出现两次，CD、STATE 文件内容各出现一次，与两次读取 edited.txt、一次读取另外两个文件相符。每主题另有 10 条输入事件，未参与 stdout 匹配。输入事件显示：先分别提交 `set` 和 `pushd`，再分别以相对文件名写入 CD 标记和变量展开后的 STATE 标记，最后 `type relative.txt state.txt`。因此证据支持真实键盘执行、`set` 变量跨命令保留，以及 `pushd` 后工作目录跨命令保留；没有把它扩大为所有 CMD 功能的通过。

两主题每个有记录的键盘输入前，焦点均为终端 HWND 66102，前台均为主窗口 328134；前后会话状态均保持 connected，代码页均为 65001。截图中浅色选中“本地 CMD #1”，深色选中“本地 CMD #2”，与对应资源一致；before 图显示既有网关测试结束后的空闲提示符，after 图可直接看到各主题准确的 CD、STATE、KEY 标记及后续提示符。四张 PNG 文件的 SHA-256 均与 JSON 重新核对一致。深色终端较窄，较长命令在右边界被截断并有水平滚动条；截图足以辨认本次输出标记，不宜据此宣称全部长行布局已经通过。

最终复核结论：第 9 次在 Windows Server 2022 的浅、深主题中，ASCII SendInput 编辑后实际执行、文件精确字节、只读 stdout 证据及 `set`/`pushd` 状态保留均通过。原始 stdout 和 after 截图也可见 `popd` 后重新出现原工程路径，但脚本未为目录恢复或变量清除设置独立断言；保留此前清理验证边界，不把恢复动作整体列为独立验收项。中文 IME、键盘布局、实体按键、其他 Windows 版本均未由此证明。
