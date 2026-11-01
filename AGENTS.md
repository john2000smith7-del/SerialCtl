# SerialCtl 开发与仓库管理规则

本文件位于仓库顶层，供接手项目的 AI 和维护者首先阅读。用户当次明确要求优先；已有授权应沿用，不重复索要相同授权。历史交接材料、实验分支和旧测试绿灯不能代替当前源码、当前需求和本次验证。

## 1. 接手顺序

修改前依次阅读：

1. 本文件与根目录 `README.md`。
2. `docs/architecture/README.md`、`docs/architecture/UI-DESIGN-GUIDE.md`。
3. `docs/architecture/build-release.md`、`docs/architecture/AUTO-UPDATE.md`、`docs/package/AUTO-UPDATE.md`。
4. `release-signing/README.md`、`release-signing/identity.json`。
5. 相关 `docs/testing/`、源码及测试。

先检查工作区、worktree、远端最新 main、VERSION 和最新正式 Release。保留他人及用户未提交修改，不通过 reset/clean 覆盖。不能把旧交接文档列出的“待办”自动当作新授权，也不能直接应用交接包中的未验证补丁。

## 2. 分支与授权

- `main` 是唯一长期分支和唯一正式发布主线。仓库访问策略为私有，必须通过 GitHub 实际 visibility/private 状态确认；接口报权限错误不等于设置成功。GitHub 源码、日志和 Release 下载需要相应授权，已有公开副本不会因私有化被收回。
- 临时开发分支统一命名为 `feat/<topic>_YYYYMMDD_HHMM`，例如 `feat/ui_capsules_20261031_0908`。topic 使用清楚的英文、数字、短横线或下划线；时间统一北京时间（Asia/Shanghai，UTC+8）。同分钟重名时更换清楚的主题说明，不覆盖已有分支。
- 现有 windows-build.yml 的 push 规则匹配 main、`feat/**` 和 `feature/**`。本项目统一使用 `feat/`；单独 `xxx_20261031_0908`、`docs/xxx_...` 或 `fix/xxx_...` 不会命中该自动构建规则，不擅自扩大工作流触发器来迁就错误命名。
- 分支完成适用验证后才能合并。确认 main 已包含提交后，立即删除该远端临时分支及可安全删除的对应本地分支。main 的正式验收和发布继续在 main 上完成；如需修复，另开带时间的新分支，不以等待发版为由长期保留已合并分支。
- 开发基于执行时最新 main，在独立临时分支或隔离 worktree 进行。合并前重读 main，保留并发修改，不强推覆盖他人的工作。 获准推送后用 `git push -u origin <临时分支名>` 触发 CI；核对 Actions 事件为 push、head_branch 为该名称、head_sha 为本次提交，不能只凭命名就声称已经编译通过。
- 分支清理前后用 `git ls-remote --heads origin` 或完整分页的 GitHub API 枚举远端。本地可能只抓取 main；`git branch -r` 不代表远端分支全貌。
- 删除前逐个核对分支头 SHA、合并关系、独有提交、资料链接和关联 PR。未合并内容先列出差异及保留建议，得到用户确认后再处理；不能为消除差异而整批合并旧分支。
- 删除前再次核对远端头，变化时停止并重新审查；操作必须明确排除 main。临时分支合并不等于其所有历史资料都已保留，应按提交图和内容核对。
- 本地分支若被 worktree 使用，先确认该 worktree 的未提交、未跟踪、忽略文件和应保留证据，再按授权清理。不要递归删除整个工作区或测试证据目录。
- 保留既有正式 tags、GitHub Releases、发布资产及历史提交。分支删除不授权删除标签、重写历史、移动正式标签或覆盖发布包。
- 需要保存未合并历史时，使用用户确认的持久归档位置并验证可恢复。只记提交 SHA、保留临时缓存或使用缺少 blobs 的 partial clone 都不算完整离线备份。
- 推送、合并 main、删除远端分支、发布和修改仓库设置须有用户明确授权；已有对本次具体操作的授权有效。用户要求“先方案，确认后整理”时，先提交清单和草稿，不提前执行。
- 新版本优先用现有 `workflow_dispatch` 在 main 发起验证和发布，不为传递一次性发布参数额外积累 release 分支。


获准推送后的命名示例（Bash；先检查工作区并获取最新 main）：

```bash
serialctl_branch="feat/ui_capsules_$(TZ=Asia/Shanghai date +%Y%m%d_%H%M)"
git switch -c "$serialctl_branch" origin/main
# 完成修改与适用验证后，按本次授权推送：
git push -u origin "$serialctl_branch"
```

## 3. 架构与产品边界

- 保持原生 C++17／Win32，不添加 .NET 或 WebView 产品运行依赖。仅交付 x64；最低运行目标 Windows 7 SP1，同时面向 Windows 10/11。
- 保持 `/MT` 静态运行库、PE 子系统版本 `6.01`、YY-Thunks 及兼容代码。现代 Windows 上运行成功不代表 Win7 实测通过。
- 不随意替换定制 PuTTY 的 plink.exe、psftp.exe。替换前必须审查固定版本、源码补丁、许可证、Win7 导入及 SSH/SFTP 行为。
- 整理代码须保留现有功能、界面和操作行为；先给检查结果和整理方案，在用户已确认的范围内修改，不做无关重构。
- 自动更新和宠物行为没有当前明确需求时不自行修改；历史材料中的旧设想不构成授权。
- `VERSION` 是唯一机器可读版本源，PROJECT_VERSION 与 CPP_VERSION 必须一致。软件版本变化同步维护根 README、VERSION.md、发布包 README 和对应 RELEASE-NOTES。
- 仅修改本文件等不影响产品及打包输入的维护文档，不自行升级软件版本或重发旧包；保留原 Release 的真实构建提交记录，不能把后来的文档提交说成旧包构建来源。新的正式软件交付仍须从当次精确 main 提交重新完成验证。

## 4. 目录责任

- 根 CMakeLists.txt 是唯一生产构建入口；`src/` 为生产源码，`legacy/` 仅为历史参考，不参与生产构建和打包。
- `tests/unit/` 是确定性模型测试，`tests/integration/` 是受控进程、协议及界面测试，`tests/performance/` 是性能测量工具。
- `third_party/` 保留固定依赖、源码包、许可证、补丁和必要运行工具，不擅自升级。
- `build/` 只放可重建中间产物。`bin/` 只保留最新正式验证通过的完整 ZIP 和其匹配的签名 update.json；未验证输出不得直接覆盖。
- 目录必须有明确责任，不添加空目录、占位文件或 .gitkeep。临时实验和私密材料不进入发布目录。

## 5. 界面及当前操作规则

- UI 修改遵守 UI-DESIGN-GUIDE，复用共享颜色、间距、圆角、控件高度和绘制机制。按钮使用已有 primary/secondary/text/icon/danger 角色；确需新角色先更新设计规则。
- 弹窗使用共享紧凑网格、内容自适应高度和一致底部动作区。深浅主题均须检查 hover、pressed、focus、checked、disabled 状态。
- 相邻输入框和按钮比较可见圆角外框的最终运行时像素高度；内部 inset Edit 的文字区高度不等于可见外框高度。
- 保留窄窗口、缩放、拖动和滚动行为，不用缩小字体或隐藏关键操作掩盖布局问题。
- 终端选择完成即复制，右键粘贴一次。终端工具入口保留在顶部设置，明确全局与当前会话范围；保留 SFTP 和文本编辑右键菜单。
- 普通 SSH 使用直接可编辑端口，默认 22，旧历史按实际保存端口恢复，不恢复已移除的平台工号模式。AI 独立账号配置按现有实现保留。
- AI 模型名称是只读胶囊，长名称省略并有完整提示；服务器未返回时如实显示，不编造模型名、不增加模型选择操作。
- 左下状态区保留实际消息、错误和诊断，仅标题被简化为细线。刷新图标保留原命令、键盘操作、提示及无障碍名称。
- 电源 Test 启动默认关闭，仅本地密码验证后解锁当前进程；开启不连接或上下电。关闭拒绝后续 GUI/AI/远程操作并异步断开，不额外 ON/OFF/reset；已接受命令的真实未知或回读结果继续保留。禁止远程解锁或持久保存授权。
- AI 工作空间由服务器、账号和已确认会话决定。目录切换、新会话或历史恢复失败时保留原会话和工作空间。

## 6. 构建、打包与 Windows 执行

- 常规验证使用仓库 `scripts/build.ps1`、`scripts/test.ps1`、`scripts/check.ps1`。正式打包使用 `scripts/release.ps1`，不能用单次编译成功代替完整流程。
- 现有 Windows CI 在 `windows-2022` 托管虚拟机运行，使用 CMake、Visual Studio 2022／MSVC 和 PowerShell。Linux 工作区可负责编辑和调度；Windows 上的编译、执行和验证必须实际完成。
- `release.ps1` 执行源码预检查、干净 x64 Release 编译、CTest、PE／版本／依赖／Win7 导入检查、显式白名单打包、重新打开归档及完整 ZIP 的原生提取验证。
- ZIP 无外层包裹目录，根部 serialctl.exe、匹配的 plink.exe 和 psftp.exe 保持同目录，保留规定的驱动、许可证及运行文档。
- 包内禁止开发源码（明确允许的 Python 运行客户端除外）、PDB、OBJ、CMake 缓存、测试程序、Git 元数据、任意用户配置和密钥。
- 用户已批准的 update.ini 默认地址模板是唯一允许的配置例外：`http://172.16.3.188/external/serial/update.json`；升级保护现有用户 INI。
- 构建工具链依赖不等于产品运行依赖。CI 使用 Python、C# 自动化或录像编码器，不能据此向产品添加相应运行库。

## 7. 验证与证据真实性

- 每次正式发布必须完成单元和集成测试；按行为风险增加有意义回归，不删除断言、跳过必要检查或放宽阈值来制造通过。
- 完整工作流必须 conclusion=success，所有必需作业实际成功。`if: always()` 可使失败后的步骤继续运行，不能仅凭下一步在执行就宣布前一步通过。
- 核对工作流名称、分支、完整源码 SHA、构建 ID 和 ZIP SHA-256。诊断分支、部分成功步骤、相近源码和旧版绿灯不能代替最终 main 验收。
- UI 修改实际运行正式程序并检查主窗口及每个变更弹窗；单独的原生控件测试可构造长名称、窄窗口等边界数据，但必须说明是受控数据。
- 独立桌面测试使用 Windows 实际鼠标／键盘事件操作已验证 ZIP 解压出的程序，记录点击、拖动、焦点、剪贴板、CMD 和受控 SFTP。截图从真实桌面或原生控件 DC 采集，不能用绘图、网页或静态模拟图冒充程序运行。
- 桌面截图常用 GetDC(NULL)+BitBlt，原生控件测试也会采集窗口 DC；连续录像使用现有无损编码。获取日志、PNG、JSON 和录像后进行核对，不能只根据上传成功判断画面正常。
- 保留源提交、实际 EXE／ZIP 哈希、测量 DPI、输入方式、采样时间、资源统计和原始证据。仅在隔离的临时测试账号运行会调整配置或桌面的夹具，保留并恢复测试前状态。
- 性能报告明确首次、热样本数、P95 与最大值，保留全部有效样本和失败记录。完整初始子控件 GDI 绘制、WM_NULL 响应、后台就绪及物理 DWM 呈现是不同口径，不得混用。
- 有限截图或录像不证明帧间绝无闪烁；renderer scaling 不等于真实系统 DPI；Windows Server 2022 不等于 Win7 实机；loopback／受控子进程不等于现场 SSH、AI 网关或物理设备。
- 未实际执行的 Windows 版本、SSH／网关、串口板卡、电源、长时间运行和多显示器测试，必须明确写“未运行”。发送已排队不等于设备执行成功。

## 8. 原签名身份与私钥保管

- 正式更新沿用找回的原 V2.0.0 身份，不生成新密钥替代。原 V2.0.2 及后续同身份客户端的兼容必须保留；历史替换公钥版 V2.0.4 是另一信任群体，不宣称自动覆盖所有历史安装。
- 当前签名路径：`/workspace/.serialctl-signing/serialctl-update-v2-rsa2048.pem`，目录 0700、文件 0600。它位于仓库外，clone 不会取得，换环境可能不存在。
- 活动公钥 SPKI SHA-256：`196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a`。签名前通过公开派生值核对私钥与 `src/update/UpdateTrustKey.h`、identity.json 一致，不能输出私钥本身。
- 本文件只记录路径、公开指纹和操作规则。原签名私钥正文及恢复口令不得放入 Git、聊天、CI secrets／日志／artifact、发布包或更新服务器；正式私钥只在授权签名环境使用，CI 接收公开签名 JSON。访问令牌不得写入源码、聊天或日志，CI 使用现有受控凭据与必要权限，不要求用户发送明文令牌。
- 现存原 PEM 未做额外口令加密；0700／0600 是访问权限，不是加密，也不能阻止同账号进程或具备相应管理权限者读取。不能承诺该工作区是永久、安全备份。
- 活动身份在 identity.json 中未配置加密仓库备份或外部密钥库。找不到原私钥时说明签名阻碍，可继续已授权的代码和测试；不得生成替代钥匙、改客户端信任根或交付未签名 JSON。
- 仓库现有 `release-signing/serialctl-update-v2.0.4.encrypted.pem` 及 replacement-v2.0.4.json 属于历史替换身份，不能拿来替代或假称原私钥备份。保留该密文与历史记录；口令由拥有者持有。其 PKCS#8 AES-256-CBC／PBKDF2-HMAC-SHA256／600000 次迭代保护不作无关更换。
- 隔离测试允许使用临时测试签名，不能替换正式信任头、公钥身份或正式更新签名。每次正式签名必须使用原私钥。

## 9. 正式双文件发布流程

1. 在临时分支完成已授权修改、审查及适用的 Windows 验证。windows-build.yml 自动覆盖 main、feat/**、feature/**；其他临时分支需要手动触发，不能因没有自动任务就省略验证。
2. 获准后合并最新 main，确认提交已进入 main 后删除临时分支；然后确认 main 对应完整提交的 Windows 工作流全部成功，并完成匹配产物的真实桌面操作检查。
3. 选定这次 main 构建的最终完整 ZIP，记录源码提交、构建 ID、ZIP SHA-256 和版本，后续签名、升级和发布保持同一组产物。
4. 使用仓库 `tools/new_update_feed.py` 与原私钥签署。首选从原始 ZIP 生成；受限环境也可签署与精确 main 已验证 ZIP 绑定的公开规范 payload，但必须随后对完整 ZIP 严格验签核对。不得仅凭版本文字签名。
5. 正式格式为 ZIP/schema 2，签名嵌在 update.json 内。payload 原始 UTF-8 字节使用 RSA-2048／PKCS#1 v1.5／SHA-256；保持现有协议、启动检查、下载确认、空闲安装、配置与驱动保护及失败恢复。非必要不改更新机制。
6. 用真实上一正式版本及仍需支持的原身份旧版客户端，验证拒绝无效签名和损坏包、安装、健康重启及配置／笔记／驱动／用户文件保留。仅验证 JSON 数学签名不等于升级流程通过。
7. 通过 publish-release.yml 发布同一成功 main 构建的 ZIP 与同一签名 JSON，不重新压缩或改动签名后的 payload。参数只有公开 metadata_base64 等发布描述，不上传私钥。
8. 发布后重新下载两个资产，核对签名、完整 ZIP／文件清单、版本、main 构建来源、标签和哈希。最终 JSON 必须与旧客户端实测用的文件一致。
9. 向用户提供两个实际下载链接、简短修改说明、验证证据及明确未实测项。临时开发分支应已在合并后删除；最终再次核对远端仅 main 长期保留，不触碰历史正式标签及发布文件。

每次正式交付恰好提供：

```text
SerialCtl-Vx.y.z-Win7-x64.zip
update.json
```

用户不负责自行生成签名、运行 Python、拆包部署或维护额外 .sig/files 目录。签名失败不得用校验和文件或未签名 JSON 冒充正式交付。正式发布版本须正确递增；同版本换包不会触发现有客户端自动升级，历史身份修复不授权以后继续使用旧版本号。

现有主流程：windows-build.yml（构建及验收）、desktop-interaction.yml（匹配包真实桌面操作）、verify-release-upgrade.yml（原身份旧客户端升级）、publish-release.yml（核验后发布）。取证和手动触发方式以当前工作流及构建发布文档为准。

## 10. 2026-11-01 已交付快照，仅供追溯

- 正式版本 V2.0.18，标签 v2.0.18，当次发布 main 提交 `c7cdfff4c459f90ca99b88809b7525aac286dda7`。
- Windows 验证：<https://github.com/john2000smith7-del/SerialCtl/actions/runs/41269406013>。
- 独立桌面操作：<https://github.com/john2000smith7-del/SerialCtl/actions/runs/41271610540>。
- 原身份旧版升级：<https://github.com/john2000smith7-del/SerialCtl/actions/runs/41271621339>。
- 发布与测试说明：<https://github.com/john2000smith7-del/SerialCtl/releases/tag/v2.0.18>。
- 该轮 Windows CI 验证、原签名及发布后下载复核通过；真实 Win7、现场 SSH／模型网关、物理板卡／电源及八小时运行未执行。普通场景 P95 不构成所有负载的硬实时保证。

这是一条历史交付记录，不是新任务的“当前最新”或新代码验收证明。每次接手必须重新查询 main、VERSION、Release 和所需验证。
