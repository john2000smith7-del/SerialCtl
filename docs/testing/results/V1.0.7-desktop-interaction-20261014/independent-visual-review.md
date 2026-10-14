# SerialCtl 真实桌面交互独立视觉复审

复审日期：2026-10-14 UTC。复审者独立读取了生产仓库 `/workspace/serialctl-main/AGENTS.md`、`README.md`、`docs/architecture/UI-DESIGN-GUIDE.md`，并通过 `view_image` 逐张检查 attempt-4 的 28 张、attempt-5 的 20 张、attempt-7 的 18 张原始桌面 PNG（共 66 张，逐张记录见下）。未修改仓库代码，未执行远端写操作。

## 当前结论

**视觉复审未通过：普通侧栏折叠/展开的中间帧存在已观测的切换按钮重影。** 折叠帧 `0094` 同时显示两个右箭头及蓝色轮廓；展开帧 `0102` 同时显示两个左箭头，其中一个跨在右卡片左边界上。相邻中间帧还有残留蓝弧，稳定结束帧恢复为单按钮。这是应用客户区内部的局部重影，不能由窗口外框拖动时的系统轮廓解释。

已读的最小窗口、较大窗口、最大化与侧栏宽度调整稳定帧，没有看到明显白块遮挡、卡片重叠、按钮中文裁字或终端越界。**这一结论只针对所读帧，不等于不存在帧间闪烁。** 没有将 JSON 中的 PASS 或终端绘制计数当作视觉无闪烁证明。

## 环境与证据边界

- 本轮证据目录：`attempt-4/`；动作清单：`attempt-4/x64-light-desktop-user-actions.json`。
- JSON 记录 Windows Server 2022、96 DPI、1920×1080 桌面；浅色生产 GUI，真实 CMD 连续 stdout。输入方法记录为 `SetCursorPos + mouse_event`，图像来源为可见桌面 `GetDC(NULL) + BitBlt`，不是 `PrintWindow`。本复审核验了实际图像；这些环境/采集方法来自记录，并非本复审者现场重新执行。
- 最小目标外框 980×620，较大外框 1180×760；标题拖动后左上角约为 (40,40)。
- 标题移动/外框缩放 held 帧显示黑色或橙色矩形系统轮廓，客户区仍处于原位置或原尺寸。释放帧才完成位置/布局变化。因此该轮不能证明启用“拖动时显示窗口内容”后的连续外框缩放表现。主执行者正在补充该设置下的新一轮。
- 快速同点 10 次鼠标 click 结束于折叠态。原生按钮可能合并双击通知，未逐次证明发生 10 次业务状态翻转；本轮不把它判为“反向动画丢失一次”，也不判通过。应以每次实际按钮 caption/业务状态变化复测。
- 此处尚未覆盖深色、高 DPI、Win7、真实 SFTP 文件列表列宽拖动；不能从命令页侧栏拖动推定 SFTP 列宽通过。

## 已观测问题与建议

### V-01 普通折叠/展开中切换按钮重复绘制（确定观测；建议优先修复）

- `attempt-4/x64-light-desktop-0094-sidebar-collapse-animation-01.png`（22:48:09.510514 UTC）：右卡片顶部 y≈173–201 同时有两个右箭头；旧按钮约 x1129–1157，新按钮约 x1148–1176，轮廓重叠。
- `attempt-4/x64-light-desktop-0095-sidebar-collapse-animation-02.png`：按钮左侧仍有旧蓝色弧线，残影正在消退。
- `attempt-4/x64-light-desktop-0102-sidebar-expand-animation-00.png`（22:48:10.639775 UTC）：两个完整左箭头蓝边按钮同时出现，约 x1084–1112 与 x1131–1159；左侧按钮压在当前右卡片左边界附近。
- `attempt-4/x64-light-desktop-0104-sidebar-expand-animation-02.png`：当前左箭头右方保留旧按钮的蓝弧。
- `0097`、`0101`、`0110` 稳定帧未保留以上重影，说明这里观察到的是过渡阶段的绘制问题。
- 建议定位动画中可见 toggle HWND 移动与父背景重绘的顺序/脏区覆盖。源码只提供定位线索：`src/app/MainWindow.cpp:1959` 的动画 timer 调用布局后使父窗口无背景擦除失效；`:2428`–`:2431` 每帧移动仍显示的 toggle；`:2498` 始终显示该按钮。尚未通过修改/复现实验证明根因，不把该推断当事实。
- 修复验收应保留真实桌面采集，覆盖普通 collapse/expand 与实际状态逐次翻转的 reversal；检查旧、新按钮区域与右卡片边界，包含浅色/深色。

为便于观察已生成只用于证据阅读的三倍局部裁剪（原图保持不变）：

- `review-crop-0094-sidebar-collapse-animation-01.png`：折叠双箭头。
- `review-crop-0102-sidebar-expand-animation-00.png`：展开双箭头与跨边界按钮。

### 不应误报为缺陷的现象

- `0004`、`0013` 的长黑色/橙色矩形线是覆盖窗口/桌面的系统拖动轮廓，不是本轮已证实的应用白块或擦除残影。
- 侧栏展开动画中终端右端暂时受到可见窗口边界裁剪，稳定帧再按新宽度换行；与设计指南“动画结束时更新 terminal cells/PTY 一次”的策略一致。仅凭此现象不能认定数据丢失。
- `0083` 的 `...during re` 下一行为 `al mouse drag`；`0110` 的 `...output dur` 下一行为 `ing real mouse drag`。局部三倍裁剪检查后字形完整，是软换行，未见终端字符缺失。对应辅助文件 `review-crop-0083-sidebar-splitter-2-released.png`、`review-crop-0110-sidebar-expand-settled.png`。
- 最小窗口右后方露出的黑色控制台属于桌面上另一窗口，未发现覆盖在 SerialCtl 客户区内的证据。

## 逐张视觉记录

以下文件均相对于 `attempt-4/`，每张均已通过 `view_image` 打开；“未见”只描述该张静止帧。

| 精确文件名 | 观察与判断 |
| --- | --- |
| `x64-light-desktop-0000-initial.png` | 初始三列、命令卡、页脚与终端完整；未见白块或按钮裁字。 |
| `x64-light-desktop-0004-caption-drag-to-40-40-held-04.png` | 窗口内容仍原位置，黑/橙轮廓向右下移动；终端新行继续出现。系统轮廓限制，不能据此证明全窗口内容拖动平滑。 |
| `x64-light-desktop-0009-caption-drag-to-40-40-released.png` | 窗口整体移至约 (40,40)，卡片/标题/按钮完整；未见原位置应用残留。 |
| `x64-light-desktop-0013-resize-round-1-minimum-held-04.png` | 缩小目标以黑色矩形覆盖原客户区，客户区仍原尺寸；轮廓穿过命令卡和页脚，不作应用重叠缺陷。 |
| `x64-light-desktop-0018-resize-round-1-minimum-released.png` | 最小窗口三列可辨，左右按钮均容纳；终端按较窄宽度换行，左下状态文本换行未压住“全部断开”。未见白块/重叠。 |
| `x64-light-desktop-0027-resize-round-1-large-released.png` | 恢复较大窗口后终端重新换行；命令卡/底部四按钮完整，无旧轮廓遗留。 |
| `x64-light-desktop-0064-caption-double-click-maximize.png` | 最大化填满工作区，左右卡片与终端边界清楚、页脚完整；未见字体裁切或背景漏绘。 |
| `x64-light-desktop-0069-sidebar-splitter-1-held-04.png` | 右栏变宽、终端变窄；命令条和页脚随栏宽变化，未见相互覆盖。终端边界局部文字暂裁，待稳定重排。 |
| `x64-light-desktop-0074-sidebar-splitter-1-released.png` | 右栏宽度稳定，“添加命令”完整出现；四个页脚按钮、命令行文本均完整，终端软换行。 |
| `x64-light-desktop-0078-sidebar-splitter-2-held-04.png` | 反向拖窄中命令页签、添加命令与切换按钮仍在自身卡片内，无白块或明显边界残影。 |
| `x64-light-desktop-0083-sidebar-splitter-2-released.png` | 较窄右栏中添加命令适应为加号；命令卡与页脚完整。终端局部放大确认是正常 `re` / `al` 换行。 |
| `x64-light-desktop-0092-sidebar-splitter-3-released.png` | 再次调宽后“添加命令”恢复全文；稳定卡片边界正常，未见按钮残留。 |
| `x64-light-desktop-0093-sidebar-collapse-animation-00.png` | 命令内容已隐藏，空白右卡收窄；顶部一个右箭头。空白卡片为隐藏动作后的过渡，不是遮挡终端的白块。 |
| `x64-light-desktop-0094-sidebar-collapse-animation-01.png` | **确定异常：两个右箭头及蓝边同时显示、部分重叠。** 其余左右卡片边界完整。 |
| `x64-light-desktop-0095-sidebar-collapse-animation-02.png` | **确定异常：当前按钮左侧还残留旧蓝弧。** 卡片进一步收窄。 |
| `x64-light-desktop-0097-sidebar-collapse-animation-04.png` | 已到窄 rail 外观，单一右箭头；终端行重排到更宽视区，无旧按钮重影。 |
| `x64-light-desktop-0101-sidebar-collapse-settled.png` | 折叠稳定，右 rail 与单按钮、终端边界整齐；未见残影。 |
| `x64-light-desktop-0102-sidebar-expand-animation-00.png` | **确定异常：两个左箭头按钮同时显示，一个跨在当前右卡片左边界。** |
| `x64-light-desktop-0103-sidebar-expand-animation-01.png` | 单左箭头居于正在加宽的空卡中；终端可见行右端暂裁，未见命令控件压入中心。 |
| `x64-light-desktop-0104-sidebar-expand-animation-02.png` | **确定异常：左箭头右侧还留旧蓝弧。** 命令内容仍隐藏。 |
| `x64-light-desktop-0110-sidebar-expand-settled.png` | 完整命令页恢复，单 toggle；终端按新宽度软换行。三倍局部检查未发现所谓漏字。 |
| `x64-light-desktop-0111-sidebar-reversal-00.png` | 折叠进行中，空右卡与单右箭头；未见大面积白块或动作侵入终端。 |
| `x64-light-desktop-0112-sidebar-reversal-01.png` | 右卡接近 rail，单右箭头；未见重复按钮。 |
| `x64-light-desktop-0113-sidebar-reversal-02.png` | 右卡又加宽、箭头朝左，证明至少一次方向反转实际发生；单按钮，无明显重影。 |
| `x64-light-desktop-0115-sidebar-reversal-04.png` | 又显示右箭头与收缩中的空卡；未见重叠，不能由该编号认定此前每一 click 均触发。 |
| `x64-light-desktop-0117-sidebar-reversal-06.png` | 显示左箭头与展开中的空卡；终端仍有新行，未见白块遮挡。 |
| `x64-light-desktop-0120-sidebar-reversal-09.png` | 显示右箭头且接近收起；单按钮，无明显重影。 |
| `x64-light-desktop-0121-failure.png` | 最终为折叠 rail，终端完整继续输出。仅说明实际最终状态；脚本的偶数点击预期尚受双击合并歧义影响。 |

## 后续复审待办

1. 读取主执行者启用 `SPI_DRAGFULLWINDOWS` 后的新轮：重点看真实客户区随外框移动/改变尺寸的 held 帧。
2. 新快速 reversal 以交替点击位置及每次实际 caption/状态变化确认 10 次业务动作，再判断最终状态。
3. 对 V-01 的修复候选重复普通折叠/展开；仅动作断言通过不足以关闭视觉问题。
4. 补实际 SFTP 列宽拖动帧后单独评估表头、列表、滚动 lane 与长文件名的裁切/重叠；当前命令页证据不能替代。

## attempt-5 补充复审：已启用全窗口内容拖动

**新轮仍未通过视觉复审：V-01 在启用全窗口内容拖动后再次出现。** 新读 `0093` 有两个右箭头，`0102` 有两个左箭头及跨边界按钮。这排除了“只是系统外框轮廓造成”的解释。对已读的标题移动、三轮外框缩放 held 帧，实际客户区跟随鼠标移动/改尺寸；未见黑/橙系统轮廓，也未见明显的大块白色残留、命令卡重叠或中文按钮裁字。

此轮 `x64-light-desktop-user-actions.json` 记录 `system_window_drag.before=false`、`during_test=true`、`restored=false`；其中 `restored=false` 表示设置恢复为原来的 false 值。桌面截图中移动/缩放的实际客户区几何变化与启用状态吻合。该临时环境设置不是产品代码修改。

### V-01 的新增复现证据

- `attempt-5/x64-light-desktop-0093-sidebar-collapse-animation-00.png`：正在收窄的右卡顶部出现两个完整右箭头，约 x1078–1106 与 x1116–1144，y173–201。两个图形间有空隙，排除了只是单个图标抗锯齿边缘的解释。
- `attempt-5/x64-light-desktop-0102-sidebar-expand-animation-00.png`：两个完整左箭头，约 x1084–1112 与 x1131–1159，y173–201；左侧按钮压在卡片左边界。
- 同轮 `0094`、`0095`、`0103`、`0104` 当前抽样帧是单按钮；稳定 `0110` 命令页恢复正常。抽样中不是每帧均重影，不减弱已捕获异常帧的事实，也不能用单按钮帧推定全过程无重影。

### 快速 reversal：移动目标输入仍有歧义

本轮确实改为相隔大于系统双击范围的点位，且记录实际 caption，但在第二次试图点击（index=1）即停止，没有完成 10 次已确认状态翻转。

- index=0：初始按钮范围 [1156,173,1184,201]，点击 [1161,187]，caption 从“折叠侧栏”变“展开侧栏”，这是一次已确认业务状态改变。
- index=1：读取范围 [1098,173,1126,201]，选择 [1120,187]；caption 未翻转。`WindowFromPoint` 命中按钮的时间是点击前，不能证明鼠标按下及松开时仍命中同一按钮。
- 记录中取点/移动约 18.638 s，`left_down` 18.656 s，`left_up` 18.665 s；中间还有约 18 ms 的目标移动机会。动画结束后按钮范围已变为 [1158,173,1186,201]，因此这个目标在持续移动，不能把“发送了鼠标 down/up”直接等价为“应用已收到 BN_CLICKED 但丢失”。
- `0111-sidebar-reversal-00.png` 的按钮位于中途约 x1098；`0112-failure.png` 的按钮已到右 rail 约 x1162，视觉证据也支持目标移动。两张均只有一个右箭头，没有新的大面积残影。
- 当前分类为“移动目标下未确认单击送达的测试/交互疑点”。应在按下及松开时记录 HWND/边界或捕获通知，跟踪按钮后再验证真实反向切换；也可从产品交互角度评估动画期间保持切换按钮固定位置，避免用户追逐目标。保持按钮固定是建议，不是已经确认的必要修复方案。

### attempt-5 逐张记录

以下 20 张均相对于 `attempt-5/`，已逐张通过 `view_image` 检查。

| 精确文件名 | 观察与判断 |
| --- | --- |
| `x64-light-desktop-0002-caption-drag-to-40-40-held-02.png` | 客户区整体已移至较初始更右下的位置，标题、三卡、页脚同移；未见系统轮廓或桌面应用残留。 |
| `x64-light-desktop-0006-caption-drag-to-40-40-held-06.png` | 客户区继续右下移动，布局完整；终端记录号递增，未见白块。 |
| `x64-light-desktop-0012-resize-round-1-minimum-held-03.png` | 按住鼠标缩小中，外框及左右卡片实际缩小，命令卡/页脚完整；终端行右端暂受视区裁剪。 |
| `x64-light-desktop-0016-resize-round-1-minimum-held-07.png` | 接近最小尺寸，三列保持分离，左侧状态文本换行而未压到按钮；右端暂裁的终端文本仍在卡内。 |
| `x64-light-desktop-0018-resize-round-1-minimum-released.png` | 最小尺寸稳定，终端完成换行，按钮字形完整；外露黑色控制台在 SerialCtl 右后方，未覆盖客户区。 |
| `x64-light-desktop-0022-resize-round-1-large-held-04.png` | 放大中实际卡片和页脚同步变大/下移；终端下方新增可用空白区符合窗口扩高，未见异色白块边界。 |
| `x64-light-desktop-0033-resize-round-2-minimum-held-06.png` | 第二轮缩小中，右卡文字与页脚完整；终端最底行在视区下边缘局部裁切，未越界或与外部控件叠压。 |
| `x64-light-desktop-0041-resize-round-2-large-held-05.png` | 第二轮放大中，三卡和底部按钮完整；未见旧边界残留。 |
| `x64-light-desktop-0060-resize-round-3-large-held-06.png` | 第三轮放大中，终端、右卡和左状态同步适配实际客户区，未见卡片重叠或按钮裁字。 |
| `x64-light-desktop-0069-sidebar-splitter-1-held-04.png` | 内部分隔拖动中右卡实际变宽；命令卡/页脚留在右栏，未见白块或重影。 |
| `x64-light-desktop-0083-sidebar-splitter-2-released.png` | 拖窄后稳定，加号保持完整，命令卡与四个页脚按钮分离；终端显示两行软换行。 |
| `x64-light-desktop-0093-sidebar-collapse-animation-00.png` | **V-01 再现：两个完整右箭头并列。** 空右卡正在收缩。 |
| `x64-light-desktop-0094-sidebar-collapse-animation-01.png` | 此抽样帧为单右箭头，右卡进一步收缩；未见旧箭头。 |
| `x64-light-desktop-0095-sidebar-collapse-animation-02.png` | 接近 rail，单右箭头；终端仍保留旧换行直到动画结束。 |
| `x64-light-desktop-0102-sidebar-expand-animation-00.png` | **V-01 再现：两个完整左箭头，左侧按钮跨右卡边界。** |
| `x64-light-desktop-0103-sidebar-expand-animation-01.png` | 此帧单左箭头，右卡加宽；终端右端暂裁但未跨卡片边界。 |
| `x64-light-desktop-0104-sidebar-expand-animation-02.png` | 此帧单左箭头，命令控件仍隐藏，未见局部蓝弧重影。 |
| `x64-light-desktop-0110-sidebar-expand-settled.png` | 命令页与添加命令恢复，单按钮；文字和页脚未重叠。 |
| `x64-light-desktop-0111-sidebar-reversal-00.png` | 第一次已确认点击后的收缩中，单右箭头位于卡内中间；命令控件隐藏。 |
| `x64-light-desktop-0112-failure.png` | 最终收起为窄 rail、单右箭头，终端按更宽尺寸重排；第二次单击未确认送达，不以此判应用丢动作。 |

截至本补充，后续待办 1 的环境限制已补齐到已读 held 帧；待办 2 的 10 次真实业务反转、待办 3 的 V-01 修复验证、待办 4 的 SFTP 列宽视觉检查仍未完成。

## attempt-7 补充复审：深色缩放、两主题 SFTP 列宽与连接列表滚动

新增逐张查看 18 张原始 PNG。**V-01 在深色再次复现：`x64-dark-desktop-0102-sidebar-expand-animation-00.png` 右侧同时显示两枚左箭头及两套蓝边，约 x1005–1033 与 x1068–1096，y173–201。** 这使该问题不再仅限浅色主题。本轮仍不能给“视觉全部通过”的结论。

深色外框缩放的三张 held 帧中，主窗口及卡片实际随鼠标改尺寸，没有看到系统黑色轮廓或大块浅色残留。终端视区边缘的文字裁切局限在终端内部；当前静止帧不足以证明帧间无闪烁。

两主题 SFTP 名称/大小边界拖动各查看 held 与 released，共八张；表头、`test.txt`、`12 B`、`Jan 01 2026` 均留在自身列区。深色名称列变窄时显示 `test....` 形式的省略文本，未压住大小列。浅蓝/深蓝表头区域是 hover/pressed 高亮，不应误报成白块或脏区。该样本仅一个短文件名，不能推定长文件名、多行目录、滚动条/传输抽屉交互全部通过。

`gui-connections-drag-0.png` 至 `-3.png` 实际呈现连接列表从中下部移到顶部；细 overlay 拇指位于卡片右侧 lane，未覆盖可读连接名或状态。底部仅显示部分下一行卡片，是滚动视口裁剪。第一张的状态文字占据列表下方空间，后续状态消失后列表增高，未见文字压在“全部断开”按钮上。这些不是弹窗截图，不能从文件名中的 GUI/drag 推定对话框覆盖通过。

关于既有 #29“全部断开浅色块”：本轮已读的 `x64-all-disconnected.png` 为深色、空连接列表，底部状态为“全部连接已断开并清空”，按钮显示深蓝 focus/pressed 外观；**这一张未见浅色矩形块，未复现不等于已修复**。没有依据将既有问题关闭。

四份 user-actions JSON 中名称和大小列动作记录为 PASS；本节视觉结论来自实际 PNG，不由这些 PASS 代替。整份记录仍保留 rapid-reversal 失败，不把列宽通过提升为整轮通过。

### attempt-7 逐张记录

以下文件均相对于 `attempt-7/`。

| 精确文件名 | 观察与判断 |
| --- | --- |
| `x64-dark-desktop-0016-resize-round-1-minimum-held-07.png` | 深色缩小 held，三卡相互分离、按钮文本完整；终端最底行及右端在自身视区内裁切，未见浅色块。 |
| `x64-dark-desktop-0033-resize-round-2-minimum-held-06.png` | 第二轮缩小 held，状态与全部断开之间有间隔，命令卡/页脚完整；无大块浅色残留。 |
| `x64-dark-desktop-0041-resize-round-2-large-held-05.png` | 第二轮放大 held，终端与右卡同步变化，未见旧卡片边界或重叠。 |
| `x64-dark-desktop-0093-sidebar-collapse-animation-00.png` | 深色折叠中命令内容隐藏，此帧一个右箭头；空右卡背景连续。 |
| `x64-dark-desktop-0102-sidebar-expand-animation-00.png` | **V-01 深色复现：两个完整左箭头及蓝色轮廓并列。** |
| `x64-light-sftp-0004-sftp-column-boundary-1-held-04.png` | 浅色名称边界拖动 held，文件行、大小、日期分离，表头完整。 |
| `x64-light-sftp-0009-sftp-column-boundary-1-released.png` | 浅色名称拖动释放，名称表头高亮宽度变化，文件行无跨列。 |
| `x64-light-sftp-0031-sftp-size-boundary-1-held-04.png` | 浅色大小边界 held，浅蓝大小标题背景为交互态，大小/日期文字完整。 |
| `x64-light-sftp-0036-sftp-size-boundary-1-released.png` | 浅色释放后日期表头出现 hover 高亮，`12 B` 与日期仍各在自身列。 |
| `x64-dark-sftp-0004-sftp-column-boundary-1-held-04.png` | 深色名称边界 held，路径与列表背景连续，无浅色块、跨列文字。 |
| `x64-dark-sftp-0009-sftp-column-boundary-1-released.png` | 深色名称变窄后文件名省略显示，大小/日期不被覆盖。 |
| `x64-dark-sftp-0031-sftp-size-boundary-1-held-04.png` | 深色大小边界 held，深蓝高亮不越过表头行；文件名/大小/日期分离。 |
| `x64-dark-sftp-0036-sftp-size-boundary-1-released.png` | 深色释放后文件名省略，日期表头高亮与自身列对齐，无可见重影。 |
| `gui-connections-drag-0.png` | 列表显示 CMD #21 起，overlay 拇指在右 lane；下方状态和全部断开按钮独立，下一行被视口裁切。 |
| `gui-connections-drag-1.png` | 列表滚到 CMD #12 起，拇指上移，右 lane 未覆盖文字；末尾半行不压页脚。 |
| `gui-connections-drag-2.png` | 列表滚到 CMD #5 起，名称与连接状态完整，卡片不延伸到 overlay 拇指区域。 |
| `gui-connections-drag-3.png` | 到 CMD #1 顶部，细拇指在 lane 顶部，无旧拇指轨迹或明亮全高轨道。 |
| `x64-all-disconnected.png` | 空连接列表、终端空白、命令按钮禁用状态合理；全部断开按钮区域深蓝，无所述浅色块。仅此帧未复现 #29。 |

本节补齐了两主题 SFTP 列宽代表帧及深色连接 overlay 滚动的独立视觉检查；尚不能替代长名称/大量文件样本、连续视频及 V-01 修复后的复验。
