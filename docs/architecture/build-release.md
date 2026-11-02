# 构建与发布

SerialCtl 的交付链路固定为：

```text
check → clean build → test → PE compatibility check → package → extract and verify → bin
```

## 环境前提

- 在 Windows 开发机或 windows-2022 CI 执行生产构建。安装 Visual Studio 2022／C++ 桌面开发组件、x64 MSVC 与 Windows SDK；开发机无需是 Win7，但产物必须保持 Win7 SP1 兼容。
- CMake 至少3.20，cmake.exe 与 ctest.exe 可调用；脚本使用 Visual Studio generator，兼容性检查还需 dumpbin.exe，common.ps1 可通过 vswhere 定位已安装工具。
- Python 3 用于 CTest 中的客户端测试及完整 ZIP 的更新提取回归；Git 用于来源追踪，OpenSSL 用于隔离测试签名和发布端签名／验签。Windows ZIP 回归可回退到 Git for Windows 的 `C:/Program Files/Git/usr/bin/openssl.exe`；正式签名工具也可显式指定 `--openssl`，先确认实际路径。
- 打包需要下载 third_party/ni-visa/files.json 固定的官方 NI-VISA ZIP，或通过 SERIALCTL_VISA_CACHE 指定摘要匹配的已有归档；不可达时报告阻碍，不省略驱动或放宽摘要检查。构建 GUI 本身与完整驱动打包是不同阶段。
- CI 桌面夹具使用 Python 和固定版本 imageio-ffmpeg；这是测试依赖，不是 Windows 产品运行依赖。仅在隔离测试账号运行会修改桌面／测试配置的夹具。
- GitHub CLI 的私有仓库读取、Actions 调度／下载、推送及发布需要实际授权，clone 不会自动转移旧环境的凭据。原签名私钥在仓库外，位置、原身份指纹与缺失时向用户索取的流程见根 AGENTS.md；禁止上传到 CI。

## 目录职责

- `build/`：CMake cache、对象、测试程序、依赖临时解包和发布 staging，可随时删除。
- `bin/`：本地只保存最新且完成重新解压验证的正式压缩包；压缩包不进入 Git，完整驱动使其超过 GitHub 单文件限制。远端二进制保存于 Actions artifact 和 Releases。
- `third_party/putty/prebuilt/`：生产运行时输入，不是中间产物。
- `third_party/yy-thunks/*.nupkg`：固定依赖输入；CMake 只在 `build/` 下临时解包。

## 标准入口

```powershell
.\scripts\build.ps1 -Architecture All -Configuration Release -Clean
.\scripts\test.ps1 -Architecture All -Configuration Release
.\scripts\check.ps1 -Architecture All -Configuration Release
.\scripts\package.ps1
```

`release.ps1` 按上述顺序执行完整流程，正式调用为 `.\scripts\release.ps1`。任何 mandatory step 失败都不得在 `bin/` 留下新的正式外观文件。脚本产出的验证 ZIP 尚不等于正式双文件交付，仍需精确 main 验收、原私钥签名和实际升级验证；同版本 CI 输出不得覆盖已发布资产。

`clean.ps1` 只清理可重新生成的 `build/`。首次目录迁移后可额外使用
`clean.ps1 -RepositoryHygiene` 清除旧原型的 `bin/obj`、依赖包历史展开副本和已经迁空的目录；它不会删除正式源码、固定依赖输入或 `bin` 中的已验证发布包。

## 发布内容

打包采用显式 allowlist，不复制整个仓库。内容范围见 [RELEASE-CONTENTS.md](RELEASE-CONTENTS.md)：保留 x64 运行程序、既有接入工具、完整离线驱动、用户文档、版本、第三方声明/许可证、MANIFEST 和包内 SHA-256 清单；开发验收文档只留源码仓库。PDB、OBJ、测试程序、未批准源码、CMake cache、Git metadata、本机配置及 Secret 一律禁止进入。三个已批准 Python 客户端是运行工具例外，Windows GUI 不需要 Python。

正式包主目录直接包含 serialctl.exe、plink.exe、psftp.exe 和用户说明，驱动位于 drivers 子目录。旧 ADR 0001 记录的双架构外层目录是历史决策，当前 x64 平铺规则以本文件、AGENTS 和实际 package.ps1 为准。

## 云端构建

`.github/workflows/windows-build.yml` 在 windows-2022 运行同一 release.ps1 流程，再启动解压后的 x64 程序进行 UI 冒烟检查并上传便携包。实际 Win7 验收按 docs/testing 执行。

发布工作流使用成功的 main 分支 Windows 构建 run ID、完整源码 SHA 和压缩包 SHA-256，从 Actions 下载已经验证的包再发布，不重新编译，不要求大压缩包进入 Git。正式版本 Vx.y.z 对应 vx.y.z 标签，保留历史发布；新正式交付递增版本。

### 从 Linux 调度 Windows 验证

以下为获得本次授权后的命令模板。先确认仓库权限、最新 main、VERSION 和 GitHub Releases；不要复制旧默认 build_run 或把未完成的 run 当作成功。

```bash
gh workflow run windows-build.yml --repo john2000smith7-del/SerialCtl --ref main
gh run list --repo john2000smith7-del/SerialCtl --workflow windows-build.yml --branch main --limit 10 --json databaseId,event,headSha,status,conclusion,url
gh run view <本次构建ID> --repo john2000smith7-del/SerialCtl --json headSha,event,status,conclusion,jobs,url
# 当前主流程由本地复合 action 自动恢复精确 run 的候选与收据，见下一节。
gh workflow run desktop-interaction.yml --repo john2000smith7-del/SerialCtl --ref main -f build_run=<同一成功构建ID>
```

main 成功构建也会触发 workflow_run 桌面审核；核对存在匹配证据后避免重复启动。实际构建在 Windows 托管虚拟机运行，截图和操作夹具也在其桌面执行；Linux 下载和检查 PNG／JSON／录像，不能把 Linux 查看图片写成 Windows／Win7 实测。下载便携包仅供匹配验证，检查来源提交和完整 ZIP 哈希后再选定。

### Actions 附件配额受限时的证据传递

Windows主流程给release.ps1传入TestFixtureDirectory，将已通过PE检查的Xfer原生辅助程序及源码／哈希收据保留到仓库外RUNNER_TEMP，供后续真实打包程序UI验收使用。release.ps1仍清理整个build目录；辅助程序不进入bin或用户ZIP。普通本地发布不指定该参数即可，实际UI验证仍按当次环境执行。

V2.0.19 主流程使用固定版本的官方 actions/cache save/restore，通过 .github/actions/ci-evidence 和 tools/ci_evidence.py 传递候选 ZIP、原始截图、录像及收据，不扩删历史附件、不增加付费预算。缓存与 artifact 使用不同存储机制；这项调整不是测试跳过或失败豁免。

键绑定运行 ID、完整源码 SHA、逻辑资料名和执行 attempt；仅恢复同仓库、同 ref、同来源运行的精确缓存。容器内清单再次核对身份、大小和每个文件 SHA-256，拒绝路径穿越、重复名称、篡改和覆盖不同文件。应用 ZIP 保持原字节，不重新压缩应用内容。发布器仍要求所有实际测试与完整工作流成功，并验证原公钥签名和两个真实旧客户端的六种升级场景。

缓存会因 GitHub 的容量／闲置策略被自动淘汰，不承诺原 artifact 的14／30／90天保留期，更不是正式档案。缺失时明确失败并重新验证构建，不借用其他源码／旧包。正式发布后重新下载并核对两个 Release 文件，必要测试报告另存仓库；历史 artifact 的读取与保留规则不变。

### 原私钥签名与正式发布

签名在授权的原私钥环境执行，使用已验证完整 ZIP：

```bash
python tools/new_update_feed.py --archive <最终完整ZIP> --output-dir <不存在的双文件目录> --private-key <原私钥受控路径>
python tools/new_update_feed.py --verify-archive <同一个最终完整ZIP> --metadata <配套update.json>
```

工具核对原客户端信任头；私钥缺失时按 AGENTS 向用户索取受控位置或安全提供方式，不能生成替代私钥。输出目录只有原字节 ZIP 和其匹配的 update.json。

旧客户端验证与发布使用 workflow_dispatch 的公开 JSON 输入文件，防止长 Base64 在命令行中损坏。用 JSON 序列化工具生成文件，模板中的占位值须替换；不手拼带秘密的 shell 命令、不包含私钥或访问令牌：

```json
{"build_run_id":"<成功main构建ID>","metadata_base64":"<最终update.json原始字节的Base64>"}
```

```bash
gh workflow run verify-release-upgrade.yml --repo john2000smith7-del/SerialCtl --ref main --json < /path/to/upgrade-inputs.json
```

升级工作流自动从最新正式Release选择previous，显式previous_tag也必须等于最新，较老原身份客户端保留V2.0.6。测试从实际ZIP的VERSION读取并记录版本，要求older < previous < candidate；不要把性能比较的固定V2.0.17基线一起替换。桌面和六个真实旧客户端场景均成功后生成与当前main、build_run_id和ZIP绑定的收据，升级收据还绑定原始update.json字节SHA-256。升级验证通过后，发布 JSON 输入为：

```json
{"version":"Vx.y.z","release_tag":"vx.y.z","archive_sha256":"<最终完整ZIP的SHA-256>","source_commit":"<成功main构建的完整40位SHA>","build_run_id":"<同一构建ID>","desktop_run_id":"<匹配桌面成功run>","upgrade_run_id":"<匹配真实旧版升级成功run>","metadata_base64":"<旧客户端实测的同一个update.json的Base64>"}
```

```bash
gh workflow run publish-release.yml --repo john2000smith7-del/SerialCtl --ref main --json < /path/to/publish-inputs.json
```

发布只允许拥有者身份从main手动触发，串行执行，不再支持release/**请求文件自动发版。tools/release_gate.py核对全部必需Windows作业和完整工作流、精确当前main、桌面／升级收据、ZIP与JSON字节哈希、真实上一正式版；失败或跳过不发布。参数准确不等于测试已通过，收据门槛回归也不等于实际执行了Windows升级。后续仍由原工具复核原公钥签名及完整ZIP，再创建更高版本Release；发布后重新下载两个资产核对。V2.0.18历史发布与维护提交分别追溯，不覆盖旧版本。

分支／标签保护和PR流程的实际状态及拥有者启用入口见 [仓库保护说明](../maintenance/REPOSITORY-PROTECTION.md)。当前管理权限／套餐阻碍未因提交JSON消失，不宣称服务端保护或第三人审批已启用。

## V2.0.3 已审核的系统依赖

圆角资源合成使用 MSIMG32!AlphaBlend；完整串口名称读取使用 SETUPAPI 的 SetupDiGetClassDevsW、SetupDiEnumDeviceInfo、SetupDiOpenDevRegKey、SetupDiGetDeviceRegistryPropertyW、SetupDiDestroyDeviceInfoList。这些接口在 Windows 7 之前已提供，不添加用户运行库。对应程序、UI 测试及解压后程序的精确 DLL 清单同步更新；新增 UDP 测试也纳入二进制检查。禁止较新 API、API-set、动态 MSVC／.NET 依赖和非6.01子系统的检查保留。

接口最低系统说明可查 [AlphaBlend](https://learn.microsoft.com/windows/win32/api/wingdi/nf-wingdi-alphablend) 和 [SetupDiGetClassDevsW](https://learn.microsoft.com/windows/win32/api/setupapi/nf-setupapi-setupdigetclassdevsw)。静态接口核对不替代 Windows 7 实机验收。

V2.0.12 的 `ui_dialog_viewport_test` 新增实际下拉列表测试，使用共享图标资源装载器的 OLE32!CreateStreamOnHGlobal（Windows 2000 起支持）。仅该测试目标补充显式 OLE32 链接及精确导入清单，和既有 `ui_combo_paint_test` 一致；生产程序原本已使用此系统库，生产依赖清单不变，Win7 禁止导入与子系统检查继续执行。
