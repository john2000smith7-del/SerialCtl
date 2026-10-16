# V1.0.7 重绘修复与真实桌面回归

2026-10-16。**已修复深色侧栏首次显示的浅色块，以及主题切换后“全部断开”残留浅色背景；保留上一轮固定侧栏按钮的修复，并补齐百次反向点击与连续录屏验证。** 本轮对应 #31、#29、#15，不把此前全部协议、安全、产品和现场验收项目改判通过。

## 版本、程序来源与环境

| 项目 | 实际记录 |
|---|---|
| 分支与工作区 | `main`，远端仅 main；独立工作目录 `/workspace/serialctl-main`，未覆盖其他工作区 |
| 生产修复 SHA | `42936e8ad3d878824409c5a5dff4cbf4c55bace1`；父提交 `b8fd3a7fd8a465e98da0c24d87687d9a6bc094a2`，承接上一轮 `906275761ea9729a87e2090b9458ebbd96ba0b19` 的锚点与焦点修复 |
| 成功桌面测试 SHA | `3a4fa759310b133b5ce8031cd4cca4598a63ffe6`；工作流验证生产、依赖、版本、打包输入与构建 SHA 一致 |
| VERSION | PROJECT_VERSION / CPP_VERSION 均为 V1.0.7；本轮没有创建新 tag 或 GitHub Release |
| 构建 | [38854030334](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38854030334)，完整 release.ps1 成功；MSVC 19.44.35229.0 / VS 工具 14.44.35207 |
| 实际桌面 | [38855024430](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38855024430)，Windows Server 2022 / 10.0.20348，1920×1080，96 DPI，Python 3.12.10 |
| 平台检查 | 原生 C++17、x64 Release、/MT、PE 6.01、YY-Thunks，原固定 PuTTY/许可证及 NI-VISA 18 输入检查通过；不等于 Win7 实机通过 |
| 便携包 SHA-256 | `251cd63b17f433caecb8a83a514bd1cbf28563c10e9945aa880a30794cb2bda9` |
| serialctl.exe SHA-256 | `c6eed3e671b24312079fe782511f413a8712666ba1ea90deee364bda9c3cec72` |
| plink.exe SHA-256 | `fb15bf12b038abb6174cb073f710eac55a68a1479769f81c0ee02d03a0c5c246` |
| psftp.exe SHA-256 | `065a5ea3086e54fb65ba9f2e05f4b72d984fc82dda978dbd1017c9b45ee4feb8` |

依据：[运行来源收据](evidence/recordings/interaction-receipt.json)、[实际 EXE 摘要](evidence/recordings/program-executable-sha256.json)、[包摘要](evidence/recordings/program-package-sha256.json)、[完整构建日志](evidence/build-job.log)、[桌面日志](evidence/desktop-job.log)。程序来自本次成功构建的 [portable artifact](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38854030334/artifacts/12026183669)，不是旧 EXE。

## 实际修复

行号固定于生产 SHA；[完整修复 diff](https://github.com/john2000smith7-del/SerialCtl/commit/42936e8ad3d878824409c5a5dff4cbf4c55bace1)。

| 问题、影响与触发 | 修改位置与方式 | 回归结论 |
|---|---|---|
| #31 / P2：深色侧栏展开结束，顶栏和底栏出现六个浅色矩形，影响连续观察输出 | `MainWindow.cpp:1385` 仅对 BS_OWNERDRAW 抑制默认 WM_ERASEBKGND，由既有 DrawOwnerItem 完整绘制主题背景、文字和圆角外部；`:2506/:4944/:4957` 用同一 UiLayoutBatch 提交本次几何与显隐，日期列依据计划宽度，不再先逐个显示旧位置的控件 | 新增命令/SFTP×浅/深四场景，每组 100 次快速反向、20 轮完整收放；深色全部 1,245 个录制帧未检出历史同类白块，结束时的实际原像素帧另行人工复审 |
| #29 / P3：主题改变后左下“全部断开”仍为旧浅色，鼠标悬停可能掩盖漏刷新 | `MainWindow.cpp:3035` ApplyTheme 在更新颜色状态后枚举全部直接子控件使其失效，补齐漏掉的 footer；保留 PowerPane 自身主题更新 | 两组各 20 次真实主题点击，不悬停目标按钮；40 次主题变换全部早期/稳定取色匹配，另检查 hover、pressed、移出取消后的背景。6 张更新器启动实屏旧复现图也未再见浅块 |
| #15 / P2：旧版本 toggle 移动导致双箭头、快速释放落空 | 保留上一轮固定最终轨道锚点及 toggle 专用双击转换；本轮修正同区域的显隐/擦除并补充录屏 | 新增 560/560 次释放均翻转目标状态，400 次快速点击的 396 个相邻间隔为 59.021–152.438ms；全部采样中锚点固定，所看原帧无旧双箭头 |

没有新增颜色、控件尺寸或业务入口。保留原 200ms 动画目标时长及终端更新边界；不把 Windows 实际调度和呈现保证为严格 200ms。未采用全局置顶、禁用反向点击、每帧强制全窗刷新或 WS_EX_COMPOSITED。FFmpeg 仅安装在测试 runner，不进入生产包。

## 实际验证及证据

| 检查 | 结论 | 证据与范围 |
|---|---|---|
| 构建、16 项原生/集成测试、PE/导入检查、白名单打包及复开 | 通过 | [构建日志](evidence/build-job.log)：16/16；版本、许可证、驱动输入与包检查成功 |
| 更新器启动异常路径、Python 客户端、签名 feed 工具回归 | 通过，限原测试范围 | 同一完整构建流程；[更新器记录](evidence/build-ui/updater-gui.json)。未在真实内网部署生产签名更新 |
| 真实窗口拖动、三轮 980×620↔1180×760、最大化/恢复、侧栏拖动 | 通过 | 两主题各 17 个实际操作、134 帧；持续真实 CMD stdout；[桌面完整日志](evidence/desktop-job.log)及 evidence/desktop 下 user-actions JSON |
| SFTP 名称/大小列宽拖动、折叠/展开 | 通过，受控替身 | 两主题各 10 个实际操作、87 帧；隔离 SSH/SFTP 子进程被替换，GUI、布局和绘制为实际程序；不证明真实服务器/文件传输通过 |
| 400 次快速反向、80 轮普通收放 | 通过 | [四组 paint JSON 与原录像](evidence/recordings)，共 560 次释放；每次验证实际命中、按下/释放边界、目标 caption、最终可见性和独立输出绘制标记 |
| 主题残留 | 通过，本次范围 | [light-theme](evidence/recordings/x64-light-theme.json)、[dark-theme](evidence/recordings/x64-dark-theme.json)：86 个取色样本全部 100% 匹配；其中 80 个覆盖主题改变的早期/稳定帧，6 个覆盖按钮状态 |
| 35 个真实 CMD、列表滚轮/键盘/滑块/刷新锚点、全部断开 | 通过，短时 CMD 范围 | 真实系统输入和既有 lifecycle 断言；子进程清理 0.360s，0 个行为失败；物理串口、电源任务和真实 SFTP 清理未运行 |
| CMD 逐键编辑和执行 | 通过，原 ASCII 用例范围 | 两主题系统 SendInput、真实 CMD 输出及文件/环境状态验证。不是中文 IME、多行剪贴板或全屏交互支持证明 |
| 命令对话框及 owner | 部分通过 | [独立视觉复审](visual-review.md)：确认分支前后台实屏正常；PrintWindow 仅验证布局，不能推广为取消和全部遮挡情形的完整验证 |
| Win7、125/150/200% DPI、物理串口、IT6332A、真实网络/SFTP、8小时 | 未运行 | 本报告不以 Server 2022、PE 检查或短时截图替代上述条件 |

### 连续录像的真实采样能力

采集为实际桌面 GetDC(NULL)+BitBlt，DIB 读取前 GdiFlush；没有向应用发送绘制指令，没有 PrintWindow。原始 FFV1 bgr0 为无损 RGB，矩形位置在每个 JSON 中声明；鼠标系统光标不进入 BitBlt。

| 场景 | 编码帧数 | 实测采样 FPS | P95 间隔 ms | 最大间隔 ms | 漏过的计划采样时隙 | 编码队列丢帧 |
|---|---:|---:|---:|---:|---:|---:|
| 深色命令 | 623 | 24.172 | 62.742 | 78.877 | 922 | 0 |
| 深色 SFTP | 622 | 24.257 | 62.734 | 78.125 | 914 | 0 |
| 浅色命令 | 616 | 24.337 | 62.796 | 78.435 | 901 | 0 |
| 浅色 SFTP | 615 | 24.512 | 62.698 | 78.084 | 889 | 0 |

共 **2,476 帧、约101.8秒实际采样**。请求60fps未达到，不能写成“60fps无丢帧”；MKV按名义60fps播放，因此播放速度不等于实际操作速度，实际时间必须读取 `*-paint-frames.jsonl` 的 seconds/capture_end_seconds/encoded_index。两帧之间仍有不可见时间，不能宣称绝对无闪烁或性能达标。

[独立解码检查](evidence/decoded-analysis.json)重新解码全部录像：帧数与时间线一致，32 张采集器保存 PNG 与对应视频帧 RGB 逐字节相同；另用独立向量实现检查全部深色帧，未见目标大块中性浅色。该检测曾在历史失败相邻三图得到 **0→16→0 网格候选**，16不是16个白块；见 [测试复核](test-review.md)。它只针对已知浅矩形，不能代替文字/边界/所有残影的视觉检查。

[解码原像素帧与时间选择清单](evidence/decoded/decoded-review-manifest.json)覆盖快速反向，以及普通第1/10/20轮收放后约40–320ms。只做无损视频解码和 PNG 编码，不缩放、重画或修图。人工观察发现某些240ms帧仍在隐藏控件，故另检查260–320ms附近第一批可见按钮帧；不能用隐藏内容的截图证明首次显示已正常。

## 独立复审、失败记录与取舍

- [源码复审](source-review.md)确认默认背景擦除、几何提交前显隐和漏刷新三条实现路径，并明确每条静态推断的边界；实际修复按此实施。
- [测试复核](test-review.md)发现并修正 DIB 同步、只读列表调用和主题双击时序问题；校准了原 #31/#29 失败图。
- 首轮桌面 run [38854535904](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38854535904) **失败**，保留 [原始日志](evidence/desktop-attempt-1-job.log) 和 [主题 JSON](evidence/attempt-1/x64-light-theme.json)。20次主题取色已全部匹配，但测试 SetCursorPos 移出后0.215ms便释放，列表2→0；尚未证明原生按钮处理了移出。不能丢弃失败记录，也不能直接把它认定为新的业务断开缺陷。
- 测试提交 `3a4fa75` 加入只读 BM_GETSTATE 等待 BST_PUSHED 清零，未发送人为取消/重绘消息，保留会话断言。第二轮两主题实测原生取消分别等待19.239/15.764ms，状态108→104，释放前后会话保持2→2、3→3，真实 stdout继续；支持首轮是测试未同步原生输入处理。它不证明任意更快输入时序都已验证。
- [独立视觉复审](visual-review.md)实际查看139个图像文件，其中79张为新录像原像素帧，区分实屏与 PrintWindow。[最终交叉复核](cross-review.md)记录候选现象撤回及证据取舍；结论按原像素决定，不按人数投票。

## 完整原始材料与复跑

仓库 `evidence/` 保留全部本轮专项目录的录像、JSON、逐帧时间线、主题原图、解码代表帧、精选构建原图和原始日志。[evidence-manifest.json](evidence-manifest.json)记录 SHA-256；`.gitattributes` 禁止改写原证据换行。完整基础窗口测试原图另保留于原始 artifact：

| 原始 artifact | bytes | SHA-256 |
|---|---:|---|
| [实际桌面12027425361](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38855024430/artifacts/12027425361) | 580199709 | `3162ff9a27c05c9ac87758bbc585be5d1b022137907a6d82a7cb18a65d4948a0` |
| [专项目录/录像12026498772](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38855024430/artifacts/12026498772) | 47624453 | `b34612b00889df53f1876242457b68384aef92f60d8bc6fbbd69f49cdd9bc77e` |
| [构建UI12027215632](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38854030334/artifacts/12027215632) | 140347334 | `67a7651f5c3a4b2fa08f2bafdd2d6acd4c6d53a383c8e4b9f5c17e77d1f22095` |

GitHub artifact有保留期限，需长期留存时下载归档。完整桌面ZIP通过 [38894976533](https://github.com/john2000smith7-del/SerialCtl/actions/runs/38894976533) 在验证原摘要/长度/CRC后仅作二进制拆分；本地拼接后复核原摘要，未改变图片或日志。

复跑：先用 `scripts/release.ps1` 构建检查，安装测试依赖 `python -m pip install imageio-ffmpeg==0.6.0`，再执行 `scripts/smoke-ui.ps1 -OutputDirectory <证据目录> -ActualMouse`。GitHub 可派发 `desktop-interaction.yml`，传入匹配生产 SHA 的成功 `build_run`。新增脚本是 `tests/integration/gui_paint_user_actions.py`、`gui_theme_user_actions.py`；入口负责启动生产程序和持续输出，不能单独以不存在的 HWND调用。解码复核用本目录 [analyze_recordings.py](analyze_recordings.py)，参数为原录屏目录及输出目录，需要 FFmpeg、Pillow、NumPy。

## 后续现场验收

1. Win7 SP1 x64及真实100/125/150/200%系统DPI分别启动同一摘要的程序，浅/深、980×620和最大化、持续输出下重复侧栏/列宽拖动。保存桌面全屏录像、DPI/系统/EXE摘要，逐帧看按钮首次出现、中文和加号边界。预期内容完整、单一toggle、无原浅块；如复现须保留第一反例。
2. 真实串口、SFTP服务器、IT6332A分别运行并取消任务、全部断开及正常退出，检查COM/子进程/任务释放及本任务控制的通道；对掉线后的掉电状态须与仪器实测一致。当前这部分未运行。
3. 两主题启用/禁用、hover/pressed/focus/遮挡恢复、命令保存和取消补足现场路径；当前连接场景没有覆盖禁用的全部断开按钮。截图按控制状态命名，记录输入及焦点/owner句柄，不以强制重绘掩盖。
4. 连续运行至少8小时，记录CPU、内存、句柄、线程、日志增长及转发/显示延迟的测量起止点。当前约百秒绘制采样和35会话短测不作为8小时稳定性或硬件容量结论。
