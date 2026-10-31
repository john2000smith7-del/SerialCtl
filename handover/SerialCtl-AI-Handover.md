# SerialCtl 项目交接资料：源码、构建、验证与双文件发布

核对日期：2026-10-31。本文供接手项目的 AI 助手和维护者使用。命令需在对应操作系统执行；文中的下一版本示例不是已经发布的版本。

## 1. 仓库与当前真实状态

- 仓库：https://github.com/john2000smith7-del/SerialCtl
- 主分支：https://github.com/john2000smith7-del/SerialCtl/tree/main
- 正式版本：https://github.com/john2000smith7-del/SerialCtl/releases
- 构建记录：https://github.com/john2000smith7-del/SerialCtl/actions
- 当前环境源码：`/workspace/SerialCtl`
- 本次查询到的远端 main：`cdc4c90bffe959a2f3626a3aa5a9ee2ffb75c238`
- main 的 `VERSION`：`V2.0.15`
- 最新正式 Release：`v2.0.15`

当前正式交付下载地址：

1. https://github.com/john2000smith7-del/SerialCtl/releases/download/v2.0.15/SerialCtl-V2.0.15-Win7-x64.zip
2. https://github.com/john2000smith7-del/SerialCtl/releases/download/v2.0.15/update.json

本次使用仓库发布工具验证了本地 V2.0.15 ZIP 与 update.json，签名及归档描述匹配。ZIP SHA-256：

```text
c5b9106fbc0b3ead480bafaa9640089de981cda856d744329569e24948e97548
```

### 本地还有未完成代码，不能当成正式发布

当前工作分支为 `feat/v216-ui-platform-test`，HEAD 与上述 main 相同，但工作区有未提交修改。这些修改涉及 AI 工作空间、平台工号端口转换、SSH 历史、Test 电源解锁、按钮样式、侧栏和滚动等，尚不能当作已完成或已通过正式 Windows 发布验收的版本。

随交接资料提供 `SerialCtl-local-uncommitted.patch` 和 `SerialCtl-worktree-status.txt`。补丁包含当时的受跟踪文件修改及三个新源码文件，不包含私钥、编译目录或用户配置。接手前先审查，不要把未完成补丁直接合并或发布。

如果接手的是同一个环境，先检查工作区，不要直接 reset、clean 或覆盖。若接手的是新环境，只 clone main 不会得到这些未提交代码。需要继续这些改动时，可在与基线相同的干净分支先检查补丁，再应用：

```bash
git switch -c continue/v216 cdc4c90bffe959a2f3626a3aa5a9ee2ffb75c238
git apply --check /path/to/SerialCtl-local-uncommitted.patch
git apply /path/to/SerialCtl-local-uncommitted.patch
```

同一工作区已经有这些修改时不要重复应用。该基线用于恢复补丁；新的独立需求仍应重新获取最新 main。

## 2. 必须先阅读的项目资料

依次阅读：

1. `AGENTS.md`：开发、兼容、验证、密钥和发布约束。
2. `README.md`：当前真实功能和构建入口。
3. `docs/architecture/README.md`：模块职责和数据流。
4. `docs/architecture/UI-DESIGN-GUIDE.md`：共享样式、布局及交互规则。
5. `docs/architecture/build-release.md`：构建和打包。
6. `docs/architecture/AUTO-UPDATE.md`：签名封装与更新协议。
7. `docs/package/AUTO-UPDATE.md`：管理员部署及客户端使用。
8. 与修改相关的 `docs/testing/` 验收说明。
9. `release-signing/README.md`、`release-signing/identity.json`：当前签名身份。

旧文档可能描述历史版本。遇到冲突应核对 VERSION、当前源码、脚本和用户最新明确要求，不能照抄历史行为。

## 3. 程序架构与修改入口

程序为 Windows 原生 C++17 / Win32，仅交付 x64，最低系统 Windows 7 SP1，同时面向 Windows 10/11。使用静态运行库 `/MT`，PE 子系统 `6.01`，通过 YY-Thunks 维持兼容。不增加 .NET 或 WebView 依赖。

| 路径 | 责任 |
|---|---|
| `CMakeLists.txt` | 唯一生产 CMake 构建入口 |
| `VERSION` | 唯一机器可读版本源 |
| `src/app/MainWindow.*` | 主窗口、会话、布局、弹窗和交互编排 |
| `src/app/UiStyle.h`、`UiMetrics.h` | 共享绘制、主题、尺寸和间距 |
| `src/app/UiDialogViewport.h`、`UiPathField.h` | 弹窗视口与路径显示 |
| `src/app/CommandNotebook.*` | 文本式命令笔记与执行入口 |
| `src/app/AiPane.*` | AI 聊天、历史、连接表单与实时终端 |
| `src/app/AiPetPane.*` | AI 宠物窗口和动画 |
| `src/ai/` | Codex SSH/App Server 客户端及协议处理 |
| `src/connections/` | 串口、远程串口、SSH、Telnet、CMD、发送队列与主机身份 |
| `src/terminal/` | VT/xterm、主/备用屏、回滚与终端状态模型 |
| `src/sftp/` | PSFTP 客户端与文件操作 |
| `src/network/` | SessionService 与统一 WebSocket 服务 |
| `src/app/PowerPane.*`、`src/power/` | 电源界面及设备服务 |
| `src/update/` | 清单验签、下载、ZIP 提取、安装与恢复 |
| `tests/unit/`、`tests/integration/` | 单元及受控集成测试 |
| `tests/performance/` | 性能测量工具 |
| `scripts/` | 标准构建、测试、检查、打包及冒烟入口 |
| `tools/new_update_feed.py` | 从已验证 ZIP 生成签名更新文件，或核验配套关系 |
| `third_party/` | 固定依赖、许可证、补丁和运行工具 |
| `docs/package/` | 打包文档、更新地址模板和版本说明 |
| `build/` | 可重建中间产物 |
| `bin/` | 最新验证通过的正式 ZIP 和配套签名 JSON |
| `legacy/` | 历史参考，不参与生产构建 |

SSH/SFTP 使用定制 PuTTY `plink.exe`、`psftp.exe`，必须与 serialctl.exe 同目录。不要直接换成其他版本，相关补丁关系到密码管道、尺寸变化和兼容性。

AI 引擎运行在 Linux Codex App Server，Windows 通过 SSH stdio 通信，复用服务器模型和认证配置。远程串口使用 SerialCtl 自身 WebSocket 服务，自动发现范围 7000–7015；当前正式版本不提供 UDP 6333 识别。

## 4. Windows 编译环境与日常构建

建议使用 Windows 10/11 开发机或 GitHub Actions 的 windows-2022，与现有 CI 保持一致。Win7 是产品运行目标，不要求在 Win7 上执行构建工具链。

准备：

- Git。
- Visual Studio 2022 或 Build Tools，安装“使用 C++ 的桌面开发”、MSVC x64 工具和 Windows SDK。
- CMake 3.20 或更新的兼容版本，保证 cmake.exe、ctest.exe 可用。
- PowerShell、Python 3。
- OpenSSL：签名和更新工具测试需要；Git for Windows 附带版本可作为候选，但应确认 PATH。
- 正式包包含 NI-VISA 离线驱动；检查 `scripts/stage-visa-driver.ps1` 的来源、摘要和缓存要求。

先确认本地依赖可用，保留仓库固定的第三方输入。不要自行重新生成图标、宠物资源或更换第三方程序。

### 获取源码并建立开发分支

全新目录示例：

```powershell
git clone https://github.com/john2000smith7-del/SerialCtl.git
Set-Location SerialCtl
git fetch origin
git switch main
git pull --ff-only origin main
git switch -c feat/your-change
```

已有工作区先执行 `git status`，保留未完成修改，再决定分支或 worktree 策略。

### 推荐构建命令

在仓库根目录执行：

```powershell
# 源码、版本和依赖预检查
.\scripts\check.ps1 -Architecture x64 -Configuration Release -SourceOnly

# 首次或完整验证采用干净构建
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Clean

# 测试
.\scripts\test.ps1 -Architecture x64 -Configuration Release

# 二进制、版本、导入及兼容性检查
.\scripts\check.ps1 -Architecture x64 -Configuration Release
```

当前 `-Architecture All` 也仅构建 x64。常规开发增量构建可以去掉 `-Clean`，正式交付仍走完整流程。

标准输出位置：

```text
build/windows/x64/cmake/Release/serialctl.exe
```

运行时还需匹配的 plink.exe 和 psftp.exe；完整分发以正式打包结果为准。

### 直接 CMake，仅用于理解或调试

```powershell
cmake -S . -B .\build\windows\x64\cmake -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build .\build\windows\x64\cmake --config Release --parallel
ctest --test-dir .\build\windows\x64\cmake -C Release --output-on-failure --no-tests=error
```

直接 CMake 成功不能替代正式发布脚本的全部检查。Linux 交叉编译也不能替代 MSVC、Windows 原生测试和实际 GUI 验收。

## 5. 如何修改并验证

修改前先定位真实入口、共享机制和相关测试，避免在 MainWindow 中继续堆叠无关逻辑，也不要为了整理架构改动业务行为。

建议一次完成一个清晰的问题，按“复现 → 确认原因 → 修改 → 验证 → 记录证据”推进。

### 自动验证

- 执行上述 build/test/check。
- 并发、协议、设备控制、持久化和更新逻辑变更，应补充能验证行为的测试。
- 不通过删断言、跳过检查或伪造通过标记掩盖失败。
- UI 改动导致旧截图或几何断言变化时，先核对批准的设计，再准确调整测试。

主要人工验收文档：

- `docs/testing/MANUAL-REGRESSION.md`
- `docs/testing/GLOBAL-UI-ACCEPTANCE.md`
- `docs/testing/AI-ASSISTANT-ACCEPTANCE.md`
- `docs/testing/AI-PET-ACCEPTANCE.md`
- `docs/testing/COMMAND-NOTEBOOK.md`
- `docs/testing/DIALOG-LATENCY.md`
- `docs/testing/AUTO-UPDATE-ACCEPTANCE.md`
- `docs/testing/WIN7-HARDWARE-TEST.md`

### 真实 GUI 验证

至少覆盖改动页面的深浅主题、悬停/按下/焦点/禁用、下拉选择、窄窗口、边框拖动、滚动和不同 DPI。性能问题应测量真实点击到首帧可交互时间；闪烁和拖动问题应保留连续帧或录屏，不只看静止截图。

原有冒烟脚本示例，在可交互 Windows 桌面执行：

```powershell
# 建议在 release.ps1 前完成，或按工作流现有顺序运行
.\scripts\smoke-ui.ps1 -OutputDirectory .\build\evidence\ui -ActualMouse
.\scripts\smoke-update.ps1 -OutputDirectory .\build\evidence\update
```

这些脚本依赖相应程序产物和桌面状态。`release.ps1` 成功后会清理 build，不能假设原测试 exe 仍在。完整顺序以 `.github/workflows/windows-build.yml` 为准。

Windows Server 2022 CI、导入表检查、仿真电源测试和受控 SSH 测试不能证明 Win7 真机、真实板卡或真实电源已经验收。交付说明应明确实际通过、未运行及需现场验证的内容。

## 6. 版本管理与完整 ZIP

`VERSION` 是唯一机器版本源，两个字段必须一致。下一版示例：

```ini
PROJECT_VERSION=V2.0.16
CPP_VERSION=V2.0.16
```

同时维护：

- 根 README 的当前版本及压缩包名称。
- `VERSION.md` 的新版本记录。
- `docs/package/README.md`。
- `docs/package/RELEASE-NOTES-V2.0.16.md`。
- 受影响的架构、验收或使用说明。

不得只改标题或 exe 文件名。自动更新要求服务端版本高于已安装版本；同版本替换包不会自动触发升级。用户要求保留版本号时，应明确说明这一限制，不能假称会同版本自动升级。

正式本地构建入口：

```powershell
.\scripts\release.ps1
```

该脚本依次执行源码预检查、干净构建、CTest、PE/Win7 检查、白名单打包、重开 ZIP 验证、完整归档的原生更新提取测试，通过后把完整 ZIP 放入 bin，并清理 build。

它不自动使用生产私钥生成正式 update.json；测试环节的临时测试签名不是生产签名。生成 JSON 见下一节。

包要求：

- 名称为 `SerialCtl-V<major>.<minor>.<patch>-Win7-x64.zip`。
- 没有外层包裹目录，根目录直接包含 serialctl.exe、plink.exe、psftp.exe 和运行文档。
- 保留脚本要求的驱动目录、许可证、MANIFEST 和 SHA256SUMS。
- 不包含 PDB、OBJ、测试程序、CMake 缓存、Git 元数据、任意本地配置或密钥。
- 不允许把重新压缩过的不同 ZIP 与原 JSON 混用。

## 7. 活动签名私钥与信任身份

当前环境已确认活动私钥存在：

```text
/workspace/.serialctl-signing/serialctl-update-v2-rsa2048.pem
```

目录应为 0700，文件为 0600。它不在 Git 仓库中，clone 仓库不会得到该文件。本交接资料也不包含私钥。

当前活动公钥 SPKI SHA-256：

```text
196b703ac1274da483936513794f0c03eb7be806c302db4d932b30378a78a21a
```

已通过导出公钥计算摘要确认上述私钥匹配此身份，未输出私钥内容。客户端信任公钥位于 `src/update/UpdateTrustKey.h`。

必须继续使用该原密钥，不能因为新环境找不到就生成新密钥，否则既有原信任链客户端不能自动升级。换服务器地址不会改变客户端信任根。

仓库中 `release-signing/serialctl-update-v2.0.4.encrypted.pem` 属于历史替换身份，不能当成当前原私钥的加密备份。活动身份没有加密仓库备份；工作区更换前应由拥有者私密保管原钥匙，或保留能使用原钥匙的受控签名环境。

不要输出私钥、提交 Git、放入发布包、更新服务器或 CI artifact。CI 只接收已经签好的公开 update.json。若另一 AI 所在环境无法访问原私钥，仍可完成代码、构建和测试，再由原签名环境签署已验证产物；不能交付未签名 JSON 冒充正式版本。

## 8. 从验证 ZIP 生成恰好两个升级文件

在能访问原私钥的签名环境，使用与构建版本对应的仓库工具。以下为 Linux 签名环境示例，V2.0.16 需替换为实际版本：

```bash
cd /workspace/SerialCtl

# --output-dir 必须尚不存在，并位于输入归档以外
python tools/new_update_feed.py \
  --archive /path/to/SerialCtl-V2.0.16-Win7-x64.zip \
  --output-dir /workspace/output/SerialCtl-V2.0.16-deploy \
  --private-key /workspace/.serialctl-signing/serialctl-update-v2-rsa2048.pem

# 对最终待交付文件再核验一次
python tools/new_update_feed.py \
  --verify-archive /workspace/output/SerialCtl-V2.0.16-deploy/SerialCtl-V2.0.16-Win7-x64.zip \
  --metadata /workspace/output/SerialCtl-V2.0.16-deploy/update.json
```

工具检查私钥与客户端公钥是否匹配，校验包描述并签名、复验，输出目录恰好包含：

```text
SerialCtl-V2.0.16-Win7-x64.zip
update.json
```

新发布必须使用 `--archive`；不要使用历史 `--package-dir` 模式，它会生成外置签名和 files 目录，不符合当前交付要求。

update.json 采用 schema 2，签名嵌在 JSON 内。签署对象是 payload 字符串的原始 UTF-8 字节，算法为 RSA-2048 / PKCS#1 v1.5 / SHA-256。不要手工修改签名后的版本、URL、摘要或 payload 内容。

## 9. main 合并与 GitHub 发布流程

正常流程：

1. 在独立功能分支完成修改和本地验证。
2. 推送功能分支，确认 Windows 工作流通过。
3. 用户授权后合并 main，不覆盖其他人的修改。
4. 等待 main 对应完整提交 SHA 的 Windows 工作流全部成功。
5. 使用这个 main 构建已经验证的 ZIP，不能从其他分支拿相似产物替代。
6. 用原私钥生成其配套 JSON，核验旧版本升级。
7. 通过已有发布工作流发布这两个文件。
8. 下载发布后的资产，再核验签名、ZIP 摘要和版本，向用户提供两个链接及简短修改说明。

本次交接不执行上述推送、合并或发布操作。后续权限以用户明确指令为准。

### 已有 Windows 构建工作流

`.github/workflows/windows-build.yml` 名称为 `Windows Win7 release build`，在 main、feat/**、feature/** 推送时运行，也支持手动触发。运行于 windows-2022，正式 ZIP artifact 名为 `SerialCtl-Win7-portable`。

GitHub CLI 示例：

```bash
gh workflow run windows-build.yml --repo john2000smith7-del/SerialCtl --ref main
gh run list --repo john2000smith7-del/SerialCtl --workflow windows-build.yml --branch main
gh run watch <BUILD_RUN_ID> --repo john2000smith7-del/SerialCtl --exit-status
gh run download <BUILD_RUN_ID> --repo john2000smith7-del/SerialCtl --name SerialCtl-Win7-portable --dir ./verified-build
```

核对 run 的 conclusion、head_branch、head_sha、工作流名称，不要只看某一个打包步骤成功。

### 已有发布工作流

`.github/workflows/publish-release.yml` 只发布指定成功 main 构建的 ZIP，不重新编译。输入如下：

| 输入 | 内容 |
|---|---|
| version | 如 V2.0.16，必须匹配包和源码 |
| release_tag | 通常 v2.0.16，必要时才用已支持的 -rN 修订标签 |
| archive_sha256 | 已验证原始 ZIP 的完整 SHA-256 |
| source_commit | 已验证 main 构建的完整 40 位 SHA |
| build_run_id | 对应成功的 main Windows 构建 ID |
| metadata_base64 | 最终签名 update.json 原始字节的 Base64，属于公开数据 |

可以 workflow_dispatch 提交这些参数，也可以在 release/** 分支提交包含同名六个字符串字段的 `release-request.json`。不要上传私钥。

PowerShell 调用结构示例，变量应先填入已验证的真实数据：

```powershell
$repo = 'john2000smith7-del/SerialCtl'
$version = 'V2.0.16'
$tag = 'v2.0.16'
$zip = 'C:\verified\SerialCtl-V2.0.16-Win7-x64.zip'
$json = 'C:\verified\update.json'
$sourceCommit = '<完整 main 提交 SHA>'
$buildRunId = '<成功的 main Windows 构建 ID>'
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$metadataBase64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($json))

gh workflow run publish-release.yml --repo $repo --ref main `
  -f version="$version" -f release_tag="$tag" `
  -f archive_sha256="$zipHash" -f source_commit="$sourceCommit" `
  -f build_run_id="$buildRunId" -f metadata_base64="$metadataBase64"
```

运行后仍需等待发布工作流成功，并确认 Release 下恰好是正式 ZIP 和 update.json。不要绕过已有工作流直接发布未经验证的本地产物。

若当前环境无法直接下载 Actions artifact，可在受控本地环境签署经过来源与摘要核验的公开候选 payload，再由发布工作流核对完整 ZIP。必须保留 main 构建、源码 SHA、归档 SHA、payload SHA 的绑定，不能凭文字版本号签名。旧环境参考脚本在 `/workspace/work/serialctl-v215/sign_verified_descriptor.py`，它硬编码了历史版本和路径，只供审查流程，不可原样用于新版。

GitHub 认证失效时检查登录状态或安全配置，不在聊天、文档或仓库中放令牌。

## 10. 旧版升级验证与服务器部署

每次正式发布至少验证上一正式版到候选版本。还应覆盖用户实际仍使用的原信任链 V2.0.6，确认可以跨多个补丁版本升级。

验证必须包含：

- 真实旧版程序读取新版 JSON，并成功验签。
- 识别更高版本，按旧客户端已有的流程确认和下载。
- 下载候选原 ZIP，完成提取、安装与重启。
- 没有连接或任务时才安装，不强制中断设备操作。
- 配置、命令笔记、日志、update.ini 和驱动保持。
- 无效签名、损坏 ZIP、不可达服务器不能安装不可信内容。

V1.10.0 及以前不支持当前 ZIP 双文件交付，首次需手动安装支持版本。历史替换公钥 V2.0.4 是另一信任群体，不能声称仅恢复原私钥就能覆盖所有已安装客户端。

当前环境历史真实升级验证参考：

```text
/workspace/work/serialctl-v215/verify_release_upgrade.py
/workspace/work/serialctl-v215/prepare_verification.py
```

这些脚本含旧版本固定值，需审查并更新到候选版本；不是可直接复用的新版本验证命令。它们也不一定存在于新环境或 main 中。测试应在 Windows 运行真实旧版 exe，而不是只比较 JSON。

用户服务器固定更新地址：

```text
http://172.16.3.188/external/serial/update.json
```

客户端 `serialctl.exe` 旁 `update.ini`：

```ini
[update]
url=http://172.16.3.188/external/serial/update.json
```

管理员把本次 ZIP 与 JSON 放在该静态目录。先上传版本化 ZIP，确认可访问后再替换 update.json，避免客户端先读到清单却下载不到包。JSON 内的 ZIP 相对 URL 必须保持原样，不能随意改名。

每次交付给用户的正式软件始终只有：

1. 完整 `SerialCtl-Vx.y.z-Win7-x64.zip`。
2. 与其严格匹配、内嵌数字签名的 `update.json`。

用户不应被要求运行 Python、生成签名、拆包部署或维护 update.json.sig/files 目录。内部构建日志和测试证据可以保留，但不能冒充第三个必需部署文件。

## 11. 最新需求与接手边界

交接包另附此前整理的完整优化需求文档（若当前环境存在）。它是需求和计划，不是完工证明。

新增重点包括：

- 连接弹窗先显示，查询后执行。
- SSH 成功后可选保存历史；密码错误与主机身份变化分别处理。
- AI 平台工号转换为 10000 + 工号，默认用户 linux。
- AI 默认 `/home/linux/workspace`，目录管理迁至页头；切换工作空间新建会话，新建会话继承当前目录。
- 输入区终端胶囊、实际模型名称、分析操作菜单、返回最新。
- 胶囊按钮、命令工具栏、有限侧栏拖动及固定日志区。
- Test 默认关闭，正确输入小写 `jj123123` 后解锁电源；用户当前要求与旧版默认禁用规则需要在实现和文档中一致处理。
- 电源滚动花屏、AI/电源断开和宠物展示及漫游优化。

未完成代码需要重新审查，尤其关注：SSH 历史是否真的等到认证成功、Test 是否所有入口一致且关闭后停止后续指令、工作空间会话边界、绘制与拖动行为、兼容旧 UI 测试及正式原生构建。不要因本地交叉编译成功就宣布这些需求已交付。

## 12. 可直接交给另一个 AI 的任务说明

> 请维护 https://github.com/john2000smith7-del/SerialCtl。先阅读 AGENTS.md、README、架构、UI、构建发布和自动更新文档，核对最新 main，并先检查本地未提交修改。本交接包可能包含尚未完成的优化补丁，不能直接当成正式版本。保持原生 C++17/Win32、x64、Win7 SP1、/MT 和子系统 6.01，保留定制 PuTTY 与兼容依赖。按用户需求在独立分支修改，使用 scripts/build.ps1、test.ps1、check.ps1 验证，UI 用真实程序检查深浅主题、缩放、拖动、滚动及响应。正式版本走 release.ps1，在获准合并后等待 main 的完整 Windows 构建成功。必须沿用原升级签名身份，原私钥在 /workspace/.serialctl-signing/serialctl-update-v2-rsa2048.pem；不在新环境时需使用拥有者提供的安全签名途径，不能生成新钥匙替代。每次正式交付恰好为完整 SerialCtl-Vx.y.z-Win7-x64.zip 和内嵌签名 update.json，验证候选归档、旧版升级、发布资产与源码 SHA 一致，最后给用户两个下载链接和简短修改说明。不要将未经运行的硬件或 Windows 版本测试宣称通过。
