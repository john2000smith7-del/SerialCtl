# 独立桌面图像复审（两批）

审核者：独立桌面视觉复审子代理 `new_paint_visual_review`。日期：2026-10-16。只读审查产品和原始证据，未修改截图、产品或原始测试日志。

本批程序源码 SHA：`42936e8ad3d878824409c5a5dff4cbf4c55bace1`；Windows 构建运行：`38854030334`（来源由主审核者提供）。`ui-environment.json` 实读确认 Windows NT 10.0.20348 / Windows Server 2022、96 DPI、1920×1080、4 逻辑处理器、约 16 GiB 内存。不是 Win7、125/150/200% DPI 或真实硬件验证。

## 结论及证据边界

- **#29 的原复现条件在本批 6 张新实屏截图中未复现。** 新 `updater-*-theme.png` 的“全部断开”背景均为深色，红色文字与圆角边界正常。旧同名 `updater-url-a-404-theme.png` 明确有浅色矩形；新同名图没有。6 张新图来自 `smoke-update.ps1` 的实际屏幕 CopyFromScreen，允许比较主题切换后状态。它们不是连续录像，不能证明切换每一时刻均无闪烁，也不能替代后续 20 次主题往返检查。
- **#31 白块尚不能仅由本批动画截图宣布通过。** 已逐张查看浅、深色各 10 张 `animation-frame-0..9`：侧栏背景连续、折叠箭头位于固定右侧，没有旧图的 6 块浅色矩形。但是这些采样全部在往返动画中，命令内容按钮仍隐藏；未覆盖原缺陷最关键的“动画结束、按钮首次显示”瞬间。需要后续连续无损录像正常展开循环中的结束帧。
- **SFTP 列宽拖动的 20 个实屏中间采样没有发现白块、旧列头高亮残留或底部控件叠压。** 列头高亮右边界随名称列调整，未见多个旧边界同时残留。每主题实际只是这一次短拖动；SFTP 来自受控替身，不证明真实 SFTP 服务或全部拖动路径。
- **命令模态布局及确认返回 owner 的样本正常。** 浅/深色命令对话框文字、加号、输入框及确定/取消边界完整；这两张是 PrintWindow，仅证明布局。`modal-foreground-before` 与 `modal-owner-after` 是真实桌面，前者编辑对话框位于主窗前，后者主窗位于记事本前，草稿已出现在列表；脚本对确认后的 `GetForegroundWindow()==main` 有断言。取消流程虽运行但本批没有独立的取消后前后台截图/焦点断言，不能从确认分支推广为所有取消/外部抢前台情况通过。
- **交叉复审已排除终端正文缺失候选。** 我最初从默认 high 展示误读部分 `animation-frame` 行为类似“05:10:41.1407”。主审核者打开原始 1920×1080 像素后指出该行实际是 `05:10:41.140 SERIALCTL_DYNAMIC_107`，正文完整。本人再次使用 `view_image(detail="original")` 对浅色 6/8/9、深色 6/7/9 原文件复查，确认完整正文；撤回原候选，不认为存在截断/丢数据缺陷，也不据此修改终端。原文件未变化，SHA-256 见逐文件表。这是依据原始证据解决分歧，非投票。

本批总计实际通过 `view_image` 查看 **54 张新原始 PNG + 2 张旧失败原始 PNG**，逐文件记录如下。不是对整个证据目录所有图片的审核。没有用 PrintWindow 图片作“无闪烁”结论。后续录像复审另附，尚未纳入本段。

## 新图逐文件记录

文件均相对 `build-ui/serialctl-ui/`。SHA-256 对原文件字节计算；未裁剪、重画或修图。

| 文件 | 采集方式 | 本图观察 | SHA-256 |
|---|---|---|---|
| `updater-empty-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `b19b253d875645aa55118be5e82675a357af7d418e5320e9d0b2a789bf69e8ab` |
| `updater-missing-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `b19b253d875645aa55118be5e82675a357af7d418e5320e9d0b2a789bf69e8ab` |
| `updater-slowbody-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `b19b253d875645aa55118be5e82675a357af7d418e5320e9d0b2a789bf69e8ab` |
| `updater-slowheaders-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `b19b253d875645aa55118be5e82675a357af7d418e5320e9d0b2a789bf69e8ab` |
| `updater-url-a-404-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `a87640028d59eb3cc45edadd3899771bbfdb68e9bee6f8f0652bb578a1ea139a` |
| `updater-url-b-invalid-signature-theme.png` | 实际桌面 CopyFromScreen | 全部断开背景深色；无旧浅色矩形。 | `900e88890f84dd51b7427c44fdf5a269830423b7c4cc9d461be6574e979f431c` |
| `x64-dark-animation-frame-0.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `ef8ad90255672b5ad1833af73ecb6021698e9dceea20508146c6424e823c7807` |
| `x64-dark-animation-frame-1.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `075ba6f89ff0505196ff5b89c48efde76e47f1445683c5ea1fd2e3907b86d5a7` |
| `x64-dark-animation-frame-2.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `ab67cfa585c03e61977de214048ab9592f9ace193580cd47846ecdbc29a68b00` |
| `x64-dark-animation-frame-3.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `7ebfcd7c733bd7ee2c54fcad6a5393467ed4f5d069923da765574f80c26c4f96` |
| `x64-dark-animation-frame-4.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `f53d89f25fb8ea2d0ad106c0abb09a5a15767605fad1626c92de4789dd7db842` |
| `x64-dark-animation-frame-5.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `de3a5137904bd08ba6dd0e55b8ca4ad7fe2a2615dd066a4b8b12cb98a2580746` |
| `x64-dark-animation-frame-6.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `144edbcc3f39e3711e96a9af61c5e92c2f6f9e7d60e4b8a792e2993bc6535dd2` |
| `x64-dark-animation-frame-7.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `9c1fde17acf4c6c4d3015bfe02592e11746ff2a03365d2778cbaa171e64e5dc0` |
| `x64-dark-animation-frame-8.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `39259372ab3502968edbeb66813b0741784895b62f2664d458a6509ffc1a17b9` |
| `x64-dark-animation-frame-9.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `4be80091367784a4410d500ed22f69181a6f076aa5d865eb7fc2a5c39518ca75` |
| `x64-light-animation-frame-0.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `c8b50c170f589800faa248eb97d46aebb04b729fdbcf71210efe8bcb7160c342` |
| `x64-light-animation-frame-1.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `7a043c11f48512c662dad6cc5305656cadd84fbf4c198741e566b3b1a82ea192` |
| `x64-light-animation-frame-2.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `0402ba4c4f0078fd20d5fe2ce8cf8890fab62dfcbe097f45e6d56423f89662aa` |
| `x64-light-animation-frame-3.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `a9a7c38bac525adcc8d2c36872cb3d815153e06d926c241debb6b6f8e4974d2c` |
| `x64-light-animation-frame-4.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `d3bb20f1310dfd44253ebc3f426a9d0115d249b1845f023315bf20d1a2282812` |
| `x64-light-animation-frame-5.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `50d844037cb85965e071cace817f9c2565b24259c7c3dc80612f4de9e989798e` |
| `x64-light-animation-frame-6.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `131d92f99775baf6302fc224ff39531876208b6cf8501000f4700f3596ee8e52` |
| `x64-light-animation-frame-7.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。 | `3ee5fd6ec7a5f720422677d096eb98e0637c6eb743ad3f27186c229041e9a330` |
| `x64-light-animation-frame-8.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `21917821728e59edc3bb3a2a9a7e3cbd4cb0f50e8467938bd33a4b4f7da5847e` |
| `x64-light-animation-frame-9.png` | 实际桌面 CopyFromScreen | 侧栏动画中，箭头单一且锚点稳定；内容隐藏，无浅色矩形。正文缺失初读候选经 original 像素复查撤回。 | `fc2a0bd11329277b4dde942025a0005924b5d4667e89b477efc5673aab8862ce` |
| `x64-dark-sftp-column-frame-1.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `059f4b68dd44ca723f95ac2df2238cbe6a9869aa32fd481383b3a1a5fbd373f1` |
| `x64-dark-sftp-column-frame-2.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `aa192becae69a7b7f1dec009ba3999cf642264a51b94ff79d959f452dfca8da0` |
| `x64-dark-sftp-column-frame-3.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `f20d4c56949d252ca039551f5ae7d2b7ceefa7e67fc14ea3f17cead5520d616f` |
| `x64-dark-sftp-column-frame-4.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `b28239f6d0b4f48aa4b1534612581031dfe77700fcf88e415022f6439136aa3f` |
| `x64-dark-sftp-column-frame-5.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `502f5127c329a436e0284b2106fbf80efe7f576c03a2e9941c71e2aac8acb95f` |
| `x64-dark-sftp-column-frame-6.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `222e9703e37e2b93bc0b53a06bc3cd28f6d408ba72c4e2215dbfd68ac941ff6b` |
| `x64-dark-sftp-column-frame-7.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `21157bd1cea1d4f4f2b823ef5ccee97b6d8da1f849ee3784d06cfadcaa07b6da` |
| `x64-dark-sftp-column-frame-8.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `8340001ab331ad5a228f2a6b4b62e4a95a2cfdc3be6a53be96e6e23d5c4c94d1` |
| `x64-dark-sftp-column-frame-9.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `268736d2c9d62c2dd60649f702e87a6e955eb784ef90d6b01b47cd8a818db835` |
| `x64-dark-sftp-column-frame-10.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `a6b5696dee84e8a1b84daa65f9f0ba9a1d77840d3533c096069548c2e226b223` |
| `x64-light-sftp-column-frame-1.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `fdf4f4133e35fcff318783d2efee4e792c1145c1ce322a9405f3dc272eb7cd9b` |
| `x64-light-sftp-column-frame-2.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `04ff010065be365d197804ef3fbff86fee49a890f37a08c2a6e45abafadf7da2` |
| `x64-light-sftp-column-frame-3.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `e6a52ba37bde25fcaa5cf1ef5b7e1d64adac71326caf8c8d5ee1c1f486831a9a` |
| `x64-light-sftp-column-frame-4.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `50dcbf7a1a6f6b5e1372ea32d3e6bedfe0912697d74dc9e2a1e417c3a6bdd769` |
| `x64-light-sftp-column-frame-5.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `6327b15c9602a4c98929b2d3520cacb062cb9b7e64443b929e2d8950b93fc34b` |
| `x64-light-sftp-column-frame-6.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `eb90c975c764acac241c6f40f32e9ea9f61adc61b51f0bab759efc341a805625` |
| `x64-light-sftp-column-frame-7.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `838abf1b044309a63a113aad6554de6a6fdd9f388f765b7dac20e6bcdb633bdd` |
| `x64-light-sftp-column-frame-8.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `d26256d34b7b8c9f281cbd0496f33a0f82e4ba0ae333cad9adcadb613d327325` |
| `x64-light-sftp-column-frame-9.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `f853bd316abcb447f4c345a758bf4ba0893813127583ce2429198716067ece82` |
| `x64-light-sftp-column-frame-10.png` | 实际桌面 CopyFromScreen | 列头、文件列表及底部按钮未见白块、旧高亮或叠压。 | `348c3630f44eca4e3255558b290d4bf9231531a142327253cd0691251ab2483a` |
| `x64-dark-command.png` | PrintWindow，仅布局 | 中文标签/加号/输入框完整；空输入时确定按钮禁用。 | `b0d8c07b85a81e7cc5d8ef0aff79c57aa7398395c2063f2a45be419a1b7943c7` |
| `x64-light-command.png` | PrintWindow，仅布局 | 中文标签/加号/输入框完整；空输入时确定按钮禁用。 | `1858023fb87baa580e88182d55e9219eceb6df1d316ba019e91273c974859ec3` |
| `x64-modal-foreground-before.png` | 实际桌面 CopyFromScreen | 编辑对话框在主窗前；记事本位于后方。 | `0cc37cf8e34daa50d8623097617c339ffdaa32ff20ac2460a4c57639c19f0729` |
| `x64-modal-owner-after.png` | 实际桌面 CopyFromScreen | 确认后主窗在记事本前；新增草稿可见，无模态遗留。 | `d5cee503d94c6f648a8e31bcfea0c1c868f76771ffc88d15fc98cc99f4c5ac22` |
| `x64-command-draft.png` | PrintWindow，仅布局 | 草稿命令可见，保存带星号；不作为落盘或动态重绘证明。 | `e58edb6edfb11885e02151dc481ae3550a28a5c05a59a925953bc93e5830cffe` |
| `x64-unsaved-close.png` | PrintWindow，仅布局 | 退出未保存提示与 Yes/No/Cancel 可见；这是系统对话框语言，不等同按钮主题残留。 | `1ea074880672a0b7f086526f1c5d92dd341e395d11116a776c9a1ce471f3b3fc` |
| `x64-light-api-cmd-visible.png` | PrintWindow，仅布局 | 展开后命令按钮完整；新输出行正文正常，稳定后新增输出行可见。 | `70084ef56444f31f821476c1ccffdf1ca623da49035439c7fa01755070fd7a6a` |
| `x64-dark-api-cmd-visible.png` | PrintWindow，仅布局 | 展开后命令按钮完整，全部断开深色；稳定后新增输出行可见。 | `5e8b801cbdde3d5e385727e36614cc5bebf9226591fdef1cbdd33d5fcdcc0d08` |

## 旧失败对照

| 原始文件 | 观察 | SHA-256 |
|---|---|---|
| `/workspace/serialctl-desktop-fixes-20261015/actual/x64-dark-desktop-0105-sidebar-expand-animation-03.png` | #31：右侧上方 2 块、底部 4 块浅色空白矩形，明确反例。 | `48652707d900d56e835755f1f06363a9d3db849947f0a1827a56bb3e7cacf1a9` |
| `/workspace/serialctl-main/docs/testing/results/V1.0.7-auto-update/evidence/updater-url-a-404-theme.png` | #29：深色主界面中全部断开保留浅色矩形，明确反例。 | `a1a299e8fe35a8c50c7d50d2ac4efbde03f17741c516432c98e33d11f64cebdb` |


## 第二批：真实鼠标输入与无损录像结束帧复审

程序仍为 `42936e8ad3d878824409c5a5dff4cbf4c55bace1`；实际 Windows 运行 `38855024430`。主审核者提供的录制方案为真实桌面 CopyFromScreen 区域连续采样、真实鼠标快速反向操作，四场景分别为浅/深色 × 命令/SFTP。每场景 100 次快速切换和 20 次正常展开/收起循环。原始录像及解码来源见 `decoded/decoded-review-manifest.json`。本人实际打开并复审 **79 张不同解码原像素 PNG**：深色命令 21、深色 SFTP 20、浅色命令 19、浅色 SFTP 19；不是逐帧人工看过全部 2476 帧。

**独立结论：本次 Windows Server 2022、96 DPI、指定窗口/负载/采样条件下，支持关闭 #31 和 #15 原复现问题。** 实际查看的快速切换采样箭头保持单一；正常展开 00、09、19 三次循环覆盖内容隐藏、按钮出现以及稳定后的相邻采样。命令栏的两个页签和底部四按钮、SFTP 的页签/路径/列表/底部按钮出现时没有旧样本的浅色白块、重复箭头或控件叠压。第一批“尚未覆盖首次显示”的证据缺口由本批补齐。此结论不扩展为所有机器、DPI、任意时长均无闪烁。

| 场景 | 所查看帧中首张内容完整帧（循环 00 / 09 / 19） | 鼠标释放后实际时间（ms） | 观察 |
|---|---|---|---|
| 深色命令 | 00298 / 00452 / 00618 | 259.280 / 271.530 / 260.340 | 六按钮有正常深色背景、文字及边界，无旧白块。 |
| 深色 SFTP | 00311 / 00457 / 00618 | 228.680 / 215.250 / 229.450 | 路径、列表、页签及底部按钮正常，无白块/叠压。 |
| 浅色命令 | 00314 / 00454 / 00610 | 190.890 / 256.010 / 221.630 | 完整命令卡片及按钮出现，无旧边界残留或重复折叠箭头。 |
| 浅色 SFTP | 00309 / 00452 / 00611 | 257.030 / 256.900 / 242.410 | 路径、列表、底部按钮正常，无旧边界残留或重复折叠箭头。 |

时间来自 manifest 的实际帧时刻减实际观察到的鼠标释放时刻，不是设定采样延时。深色命令的 00297 在约 228ms 仍隐藏，00298 在约 259ms 已正常显示；不能用代码 nominal 200ms 推断全部机器均在 200ms 完成。部分 320ms 采样已进入下一次收起，内容隐藏符合输入序列，不能误判为重绘丢失。

主审核者另对 2476 帧全部解码，报告 1245 个深色帧的白块规则检测 0 反例，32 张原 PNG 与相应视频解码像素完全一致；这是其自动检查结果，不冒称本人逐帧人工审查。实际录制约 24fps，报告最大相邻采样间隔 78.877ms。因此仍不能排除采样间隙内的更短暂异常，也不声称 60fps 或无漏采。录制区域主要覆盖侧栏，不能据此评判被区域排除的终端文本和其他窗口。SFTP 使用替身；Win7 与 125/150/200% DPI 仍未运行。

以下逐文件表仅列本人实际打开的 79 张，完整 SHA、捕获矩形、帧索引、真实时间及观察亦保存于 `decoded/independent-visual-reviewed.json`。截图未裁剪、合成或修图；解码保留原视频像素。初选清单 `independent-visual-selection.json` 是选择计划，最终实际审图清单以 `independent-visual-reviewed.json` 为准。

| 原像素 PNG（相对 decoded/） | 输入阶段：释放后实际 ms | 观察 | SHA-256 |
|---|---|---|---|
| `x64-dark-desktop-paint-decoded-00005.png` | rapid-000: 34.113 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `9ad7802d9d4a73e27be4e27129fda590b36130f99bddd479651462826e7cc2e2` |
| `x64-dark-desktop-paint-decoded-00069.png` | rapid-024: 17.613 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `be9382607e99e9a9473750941d0078625e36cd2e30ce0f3f09975f06e91da1fc` |
| `x64-dark-desktop-paint-decoded-00134.png` | rapid-049: 17.405 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `87f33315d2ae548d8e5e4b83df71af4f48d3ed5a67cf05da4d923904f95fcaaa` |
| `x64-dark-desktop-paint-decoded-00204.png` | rapid-074: 16.557 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `1f1984ce8fcf765f0c75eb05fec2b3f0405dba661ac36522c3c56f1ac5142bb0` |
| `x64-dark-desktop-paint-decoded-00274.png` | rapid-099: 14.309 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `0344cefb893aedbb20f85a315e56d69acc4987c194c559028831d9deaac474d7` |
| `x64-dark-desktop-paint-decoded-00295.png` | normal-expand-00: 118.753 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d3e5af6368f45bc7882ed9105b3809aa534f73d17172388b66038299dcb7eed5` |
| `x64-dark-desktop-paint-decoded-00296.png` | normal-expand-00: 165.416 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4114229df2292c441308eadf4b9b9ce1d12517df215a3ae3c31b556c832d4508` |
| `x64-dark-desktop-paint-decoded-00297.png` | normal-expand-00: 228.115 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4114229df2292c441308eadf4b9b9ce1d12517df215a3ae3c31b556c832d4508` |
| `x64-dark-desktop-paint-decoded-00298.png` | normal-expand-00: 259.280 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-desktop-paint-decoded-00299.png` | normal-expand-00: 290.585 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-desktop-paint-decoded-00300.png` | normal-expand-00: 337.462 | 已进入下一次收起，内容隐藏；背景干净，折叠箭头单一。 | `7a9a4e8b0c19b2f2dacf23bc0a983383ef2a51298bbd8c62c3698cf7dda01fef` |
| `x64-dark-desktop-paint-decoded-00449.png` | normal-expand-09: 130.814 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d3e5af6368f45bc7882ed9105b3809aa534f73d17172388b66038299dcb7eed5` |
| `x64-dark-desktop-paint-decoded-00450.png` | normal-expand-09: 193.389 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4114229df2292c441308eadf4b9b9ce1d12517df215a3ae3c31b556c832d4508` |
| `x64-dark-desktop-paint-decoded-00451.png` | normal-expand-09: 240.280 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4114229df2292c441308eadf4b9b9ce1d12517df215a3ae3c31b556c832d4508` |
| `x64-dark-desktop-paint-decoded-00452.png` | normal-expand-09: 271.527 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-desktop-paint-decoded-00453.png` | normal-expand-09: 302.700 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-desktop-paint-decoded-00615.png` | normal-expand-19: 119.477 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d3e5af6368f45bc7882ed9105b3809aa534f73d17172388b66038299dcb7eed5` |
| `x64-dark-desktop-paint-decoded-00616.png` | normal-expand-19: 166.183 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4114229df2292c441308eadf4b9b9ce1d12517df215a3ae3c31b556c832d4508` |
| `x64-dark-desktop-paint-decoded-00617.png` | normal-expand-19: 229.063 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d604a3ffed9b016313c2e887654a465dad95b8535c96f2355e8db3874db3744a` |
| `x64-dark-desktop-paint-decoded-00618.png` | normal-expand-19: 260.336 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-desktop-paint-decoded-00619.png` | normal-expand-19: 307.075 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `ff8524490054ae0b02138777f693f2ae95dbff3d092018d1076a4b4be3118826` |
| `x64-dark-sftp-paint-decoded-00006.png` | rapid-000: 34.489 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `5d3b82ed662296236fbea3ce7ee7419204ee4c9eb7941fdb683fda498ff0797e` |
| `x64-dark-sftp-paint-decoded-00071.png` | rapid-024: 18.384 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `b259682621e13c823a1bf14652f416df0790cabe8ae2ed6f30df5884ebea59d0` |
| `x64-dark-sftp-paint-decoded-00143.png` | rapid-049: 17.942 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `2a3a88f005aeb777f850cdd40f0681eea0e06790c31f9127f596ab77055a7ed1` |
| `x64-dark-sftp-paint-decoded-00216.png` | rapid-074: 17.193 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `b3625b77f134062997de00f22e3ae0144207a69edbac1b511066c81a5f4f706f` |
| `x64-dark-sftp-paint-decoded-00288.png` | rapid-099: 18.187 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `33d268f553e99f52f3b8d8c6c38c30288fe027179b5a8859e247c41a589000e1` |
| `x64-dark-sftp-paint-decoded-00308.png` | normal-expand-00: 87.953 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `f7a49be3ef471b3d808071394a47ab5d1baec3ed01141991a800f0b245e722c1` |
| `x64-dark-sftp-paint-decoded-00310.png` | normal-expand-00: 181.748 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `9c807493a54924e08b91d381da39d805689eb35d1050628b483155588e0631aa` |
| `x64-dark-sftp-paint-decoded-00311.png` | normal-expand-00: 228.683 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00312.png` | normal-expand-00: 275.385 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00313.png` | normal-expand-00: 306.744 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00454.png` | normal-expand-09: 74.351 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `f7a49be3ef471b3d808071394a47ab5d1baec3ed01141991a800f0b245e722c1` |
| `x64-dark-sftp-paint-decoded-00456.png` | normal-expand-09: 168.127 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `f17162015276bc38b091dfa088435f5ef1667bd28fd575e713daefbdf1f67fc5` |
| `x64-dark-sftp-paint-decoded-00457.png` | normal-expand-09: 215.248 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00458.png` | normal-expand-09: 277.839 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00459.png` | normal-expand-09: 308.961 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00615.png` | normal-expand-19: 88.551 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `f7a49be3ef471b3d808071394a47ab5d1baec3ed01141991a800f0b245e722c1` |
| `x64-dark-sftp-paint-decoded-00617.png` | normal-expand-19: 182.728 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `9c807493a54924e08b91d381da39d805689eb35d1050628b483155588e0631aa` |
| `x64-dark-sftp-paint-decoded-00618.png` | normal-expand-19: 229.447 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00619.png` | normal-expand-19: 276.544 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-dark-sftp-paint-decoded-00620.png` | normal-expand-19: 338.742 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `0efeb0652d4984b5b7c9f021350d301c9458db46f12a540895860305be32b09f` |
| `x64-light-desktop-paint-decoded-00006.png` | rapid-000: 22.834 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `1159c7973e2aad3b9be1cb68fd594b1763280cf7979ffd284c10726bb8c11136` |
| `x64-light-desktop-paint-decoded-00076.png` | rapid-024: 21.697 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `0ed32247172b611b281cb51d910ef2fac167984a1240c78273d7211e27cf03b7` |
| `x64-light-desktop-paint-decoded-00149.png` | rapid-049: 21.964 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `4bed3ae279f99370125e77af8717d49964863276a95262c57683ee063d24c907` |
| `x64-light-desktop-paint-decoded-00221.png` | rapid-074: -9.706 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `e6ad318d301a27fcd23094df96e1ce107e946ca72c26293a29b1b66781812985` |
| `x64-light-desktop-paint-decoded-00292.png` | rapid-099: 22.307 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `59388f738c48328da5ab586b4962878309fb7150afc6883eb19eab6efcf7d913` |
| `x64-light-desktop-paint-decoded-00312.png` | normal-expand-00: 112.666 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `80f33319e2077e562ecad4ae02c5a59d6feae78b0612bc155249f1fca309890e` |
| `x64-light-desktop-paint-decoded-00314.png` | normal-expand-00: 190.890 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00315.png` | normal-expand-00: 253.332 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00316.png` | normal-expand-00: 300.284 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00450.png` | normal-expand-09: 68.926 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `7ba28fee0e5ca77444f55bd1fd1fd67166147281bed46627ba275a4be5e23f7f` |
| `x64-light-desktop-paint-decoded-00452.png` | normal-expand-09: 178.236 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `ac00f8bb9bbb20a3ece05769fbf18f0eff7211715c2027aa5025a0a47761c88b` |
| `x64-light-desktop-paint-decoded-00454.png` | normal-expand-09: 256.011 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00455.png` | normal-expand-09: 303.296 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00456.png` | normal-expand-09: 334.592 | 已进入下一次收起，内容隐藏；背景干净，折叠箭头单一。 | `d5fdd7f8c6490b7f1d93f3492c3b9559639e95f985f6f265e0cd994511e37eaa` |
| `x64-light-desktop-paint-decoded-00608.png` | normal-expand-19: 111.605 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `797e59228faa74bd66792921d7d46dc8c3b764ec98c5f2b1a574a96d9eb42c3e` |
| `x64-light-desktop-paint-decoded-00609.png` | normal-expand-19: 174.383 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `ac00f8bb9bbb20a3ece05769fbf18f0eff7211715c2027aa5025a0a47761c88b` |
| `x64-light-desktop-paint-decoded-00610.png` | normal-expand-19: 221.626 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00611.png` | normal-expand-19: 267.771 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-desktop-paint-decoded-00612.png` | normal-expand-19: 299.353 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `8496217eeaf5730e08825a716bd87c2290a377684b15927f8f2e000ba8ea3cc7` |
| `x64-light-sftp-paint-decoded-00006.png` | rapid-000: 22.336 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d728f4d400d4c38da9b0b1e0de69cc60744d708a8a1c91eaf6a90cd2505a7b3b` |
| `x64-light-sftp-paint-decoded-00075.png` | rapid-024: 21.603 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `8b2cfbe0faadc3a096ba162eef62b06ee97f2ace3af9415378ef0176b45ab433` |
| `x64-light-sftp-paint-decoded-00145.png` | rapid-049: 22.350 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d97caf3606e821c91281d7a880da6a4a4cde43924e98238a588bbbd5f608d73b` |
| `x64-light-sftp-paint-decoded-00215.png` | rapid-074: 22.168 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `f6f1c396048b1165943ec24f205b4b6acd529f67d65a2ee5efc8894b08ab51a6` |
| `x64-light-sftp-paint-decoded-00286.png` | rapid-099: 21.937 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `d8e34e23913228cb741b979541f78c4a2d1068aa19d8a84cfc1710d054e1874d` |
| `x64-light-sftp-paint-decoded-00306.png` | normal-expand-00: 116.034 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `eaeb1884884cce381103eaa42c210ca76a48d746f3433f94eec62e7104e9de5c` |
| `x64-light-sftp-paint-decoded-00307.png` | normal-expand-00: 179.025 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `891f7dc11429a5aa7dafcb12dabaa54f4dfd7f5eb753dd31bf9d1b7fa857e7aa` |
| `x64-light-sftp-paint-decoded-00309.png` | normal-expand-00: 257.027 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |
| `x64-light-sftp-paint-decoded-00310.png` | normal-expand-00: 303.563 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |
| `x64-light-sftp-paint-decoded-00311.png` | normal-expand-00: 334.983 | 已进入下一次收起，内容隐藏；背景干净，折叠箭头单一。 | `90981d8b3417e21bfa8de100c023a508dbc0bb1e069eec5d4acf17e0577c24b0` |
| `x64-light-sftp-paint-decoded-00449.png` | normal-expand-09: 116.250 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `c99ef4202c4b0a0b6e3763a1b328357df6e537e944804cc0bef4d962e43cd158` |
| `x64-light-sftp-paint-decoded-00450.png` | normal-expand-09: 163.294 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `03a0d59f780818e27a663668457cd55a4d7c4fdaffe71fa80bf92a9745d39fbb` |
| `x64-light-sftp-paint-decoded-00452.png` | normal-expand-09: 256.902 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |
| `x64-light-sftp-paint-decoded-00453.png` | normal-expand-09: 303.709 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |
| `x64-light-sftp-paint-decoded-00454.png` | normal-expand-09: 319.453 | 已进入下一次收起，内容隐藏；背景干净，折叠箭头单一。 | `90981d8b3417e21bfa8de100c023a508dbc0bb1e069eec5d4acf17e0577c24b0` |
| `x64-light-sftp-paint-decoded-00608.png` | normal-expand-19: 116.234 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `eaeb1884884cce381103eaa42c210ca76a48d746f3433f94eec62e7104e9de5c` |
| `x64-light-sftp-paint-decoded-00609.png` | normal-expand-19: 163.311 | 过渡中内容隐藏（可能有轮廓），无异常矩形/重复折叠箭头；此帧不单独证明按钮首次显示正常。 | `891f7dc11429a5aa7dafcb12dabaa54f4dfd7f5eb753dd31bf9d1b7fa857e7aa` |
| `x64-light-sftp-paint-decoded-00611.png` | normal-expand-19: 242.409 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |
| `x64-light-sftp-paint-decoded-00612.png` | normal-expand-19: 304.742 | 完整控件可见；主题一致，无六按钮白块/重复折叠箭头。 | `38c61125fe476fcc7211aac4217dacc3068e2e885c8099bc16e116ab5020ff0e` |


## 终端误读候选的补充复核记录

终端候选已撤回。为防止图像展示比例或相似画面呈现导致再次误读，除重新打开 original 像素外，直接对原始 6 个 PNG 执行 OCR，原文输出保存于 `terminal-candidate-ocr.json`。例如浅色 frame-6 的 `SERIALCTL_DYNAMIC_107/108/109/110` 均被读取为完整字段；OCR 仅作辅助，原始像素仍为主要证据，不以 OCR 证明全终端无丢字。原始截图 SHA 见第一批逐文件表，未因该候选修改产品。

交叉检查时还打开以下 4 张旧稳定样本，均未见上述候选所声称的正文缺失；这些为对照，不是新版通过证据。第一批计数 54 新图 + 2 旧失败图之外，另有这 4 张旧稳定对照；加第二批 79 帧，本人累计打开 139 个不同图像文件（重复查看不重复计数）。

| 旧稳定原图 | SHA-256 |
|---|---|
| `/workspace/serialctl-desktop-fixes-20261015/build-ui/serialctl-ui/x64-light-animation-frame-6.png` | `1ab6e572b13b87384e4a12523c1c95003207209ee218b49c3f0d32d3597d323f` |
| `/workspace/serialctl-desktop-fixes-20261015/build-ui/serialctl-ui/x64-light-animation-frame-9.png` | `c9349e79925316ed91004eb2afa6bba08349b27f9c03882c02de6d8d4b8b36d9` |
| `/workspace/serialctl-desktop-fixes-20261015/build-ui/serialctl-ui/x64-dark-animation-frame-6.png` | `e4929408e3fd1c38f5cf548c56651f5ff2a979d46d4205364c37ff9b4672c9b5` |
| `/workspace/serialctl-desktop-fixes-20261015/build-ui/serialctl-ui/x64-dark-animation-frame-9.png` | `ea02c551b8eda5cf449c7229977cd087297d46eec7583690f3571141a8418657` |
