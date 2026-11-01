# 仓库保护与开发发布流程

本页区分服务器实际限制、仓库内可执行流程和待拥有者启用的配置。文件存在不代表 GitHub 已执行对应规则。当前机器版本仍为 V2.0.18，本次不发布新版、不覆盖正式资产，也不改变 Win7 或原身份更新兼容。

用户已明确选择保持当前套餐，先按PR／CI规范运行，不购买Pro。本文Rulesets配置为未来可选预案，不是当前必须升级的待办，也不反复索要付费确认。免费方案的流程约定不能阻止拥有者直接推送或手动绕过，不能把它描述为服务器已强制保护。

## 当前基础保护状态（2026-11-01 检查快照）

- 仓库实际 private，默认分支 main；正式标签和历史 Release 保留。
- 当前集成修改仓库设置返回403 Resource not accessible by integration；关闭fork、开启PR合并后删分支、限制合并方式等尚未实际应用。
- Rulesets 接口明确返回需要 GitHub Pro（不能以改公开来绕过）。main／标签规则 JSON 已准备，但不宣称服务器保护已经开启，也不自动购买套餐。
- Actions 默认令牌、动作允许列表、跨仓库共享等管理接口亦受权限限制。所有仓库内工作流保持显式最小权限；发布作业才取得 contents:write。
- 拥有者审查 Apps、OAuth、Webhooks、Deploy keys 和待处理邀请的管理权限未由当前集成取得。没有读取结果时不删除未知授权，也不声称授权已清理。

要核对最新状态而不修改设置：

```bash
python tools/configure_repository.py --output /path/to/settings-check.json
```

拥有者具备相应管理权限，审查下列 JSON 后可应用免费的仓库／Actions设置（默认跳过Rulesets，使用其现有gh登录，不向AI或聊天发送令牌）：

```bash
python tools/configure_repository.py --apply --output /path/to/settings-application.json
```

每项修改后重新读取 GitHub 验证，全部匹配才标为 ENFORCED；403、套餐限制或配置不同返回非零状态。脚本不改变其他仓库、不改标签、不上传私钥。

付费规则在默认报告中明确为DEFERRED_CURRENT_PLAN，含义是未启用；其他设置须真实匹配才为ENFORCED。未来若拥有者主动改变套餐选择并有管理权限，才显式加--include-rulesets检查／应用这两份规则，不从本次授权推断购买服务。

## 精确配置与拥有者设置入口

| 项目 | 已准备的配置 | GitHub入口 |
| --- | --- | --- |
| 私有、关闭fork、PR合并后删分支、仅squash | [.github/repository-settings.json](../../.github/repository-settings.json) | [General](https://github.com/john2000smith7-del/SerialCtl/settings) |
| main：禁止删除／强推，线性历史，PR与检查 | [main.json](../../.github/rulesets/main.json) | [Rules](https://github.com/john2000smith7-del/SerialCtl/settings/rules) |
| v*标签禁止删除和改指向 | [release-tags.json](../../.github/rulesets/release-tags.json) | [Rules](https://github.com/john2000smith7-del/SerialCtl/settings/rules) |
| GITHUB_TOKEN默认只读，禁止自动批准PR | [workflow-token-permissions.json](../../.github/workflow-token-permissions.json) | [Actions](https://github.com/john2000smith7-del/SerialCtl/settings/actions) |
| 禁止其他仓库复用私有动作／工作流 | [actions-sharing.json](../../.github/actions-sharing.json) | [Actions](https://github.com/john2000smith7-del/SerialCtl/settings/actions) |
| 允许GitHub官方动作及固定的MSVC初始化动作 | [actions-permissions.json](../../.github/actions-permissions.json)、[allowed-actions.json](../../.github/allowed-actions.json) | [Actions](https://github.com/john2000smith7-del/SerialCtl/settings/actions) |

main 规则的检查名称对应实际 GitHub Actions，限定 app ID15368：Repository policy、Workflow tools on Windows、build、Native callback destruction memory safety、Packaged frequent-click and layout endurance，以及五个 UI screenshots 页面作业。Windows流程工具单独运行流程回归和PowerShell语法解析，不等待完整应用性能测试；它不代表原生应用验收。严格检查最新 main，不用失败后仍能继续运行的截图作业代替完整构建成功。

当前只有拥有者一位维护者，PR规则要求PR和讨论解决，但不设置必须由另一人批准，以免无法自批而锁死。CODEOWNERS负责归属和通知，不等于审批已发生；将来增加维护者后再明确所需批准人数和代码所有者审查。

## 日常开发与PR

1. 按 [AGENTS.md](../../AGENTS.md) 阅读规则、获取最新main，检查工作区，使用 feat/topic_vx.y.z_YYYYMMDD_HHMM 临时分支（北京时间）。
2. commit与PR标题为 类型(Vx.y.z): 修改摘要；正文有“修改内容、验证情况、兼容性”三节。PR模板支持Markdown标题。
3. 推送触发现有Windows流程与Repository policy。PR检查版本、分支和提交格式、固定Actions SHA及基本秘密模式；它不是完整秘密扫描，更不能替代真实Windows或硬件验收。
4. 查看全部适用验证及真实截图，保留失败和未执行记录；禁止跳过、删除断言、放宽100ms门槛来制造通过。只读诊断不构成产品修复授权。
5. 所需检查成功、差异审查及用户授权满足后，通过PR squash合并；PR标题／正文生成结构化main提交。新main重新执行完整正式验收；合并后实际删除远端和对应可安全删除的本地分支。
6. 若CI或审查未通过，保留未合并PR和临时分支供修复；“只保留main”指唯一长期主线，不授权删除未合并工作或绕过失败检查。未启用服务器规则时仍按同样规范操作，并如实报告其限制。

模板位于 [.github/pull_request_template.md](../../.github/pull_request_template.md) 与 [.github/ISSUE_TEMPLATE](../../.github/ISSUE_TEMPLATE)。发现疑似秘密问题按 [SECURITY.md](../../SECURITY.md) 私下处理，不在反馈中粘贴正文。

## 正式发布门槛

发布仅保留 main 的 workflow_dispatch 入口，拥有者身份明确触发；不再通过 release/** 分支中的请求文件自动发版。触发是一次明确发布操作，不等于购买服务或换钥授权；AI仍须有本次正式发布授权。

1. 正式版本必须高于最新Release；保持原签名身份、原客户端兼容、Win7 SP1／x64／MT／6.01和双文件交付。此维护提交的V2.0.18不能再次作为新版本发布。
2. windows-build.yml须在精确当前main全部成功，含8个必需作业；单次native编译或部分成功不够。
3. desktop-interaction.yml手动输入本次build_run，不再提供旧编号默认值。测试源码和程序构建SHA必须完全一致。实际鼠标／键盘测试全部成功后生成SerialCtl-release-desktop-receipt。
4. 在仓库外用原私钥签署最终ZIP。verify-release-upgrade.yml自动选择最新正式Release为previous，也允许显式previous_tag但必须与最新一致；较老原身份客户端仍为V2.0.6。测试从实际归档VERSION读取版本，要求older < previous < candidate。
5. 两个真实旧客户端各执行无效签名、损坏ZIP、有效安装／健康重启／配置和用户数据保留，共六种情形。全部通过才生成SerialCtl-release-upgrade-receipt，记录ZIP SHA-256、原始update.json字节SHA-256、来源构建及真实版本。
6. publish-release.yml增加desktop_run_id、upgrade_run_id，下载对应收据，与build_run_id、精确main、ZIP哈希和JSON字节哈希共同核对。任一失败、跳过、过期、缺收据或不匹配都拒绝发布。现有签名工具再验证原公钥与完整ZIP，最后才创建新的Release。
7. 发布后重新下载两个资产，核对签名、文件、版本、标签与构建来源，并向用户说明未运行的Win7实机、现场SSH／AI网关、板卡、电源及长时间测试。客户端更新协议及信任头未作修改。

发布在同一并发组串行执行；不覆盖旧标签和资产。GitHub Environment人工审批的可用性受私有仓库套餐限制，本次没有建立一个无人审批的Environment来冒充批准门槛。当前控制为拥有者从main手动触发、用户授权和真实证据校验；不宣称GitHub已强制第三人审批。

## 产物保留与已有失败

新上传的普通诊断资料保留14天；可运行候选ZIP与桌面原始资料保留30天；正式桌面／升级收据保留90天。收据不是用户应用交付文件，正式Release仍只有ZIP与update.json。旧959个产物不在本轮自动删除；正式交付必要证据须另行持久保存，Actions不是永久档案。

已有50f5fbb分支run41292894215在native编译／47项CTest／ZIP验证之后，因dark/serial responsive warm P95=109ms超过100ms失败。相关失败及原始样本保留，未声明修复，也未调整门槛。新流程不能把这次构建当作成功正式发版来源。当前实际CI结果必须重新查询，历史失败不是未来代码自动失败的证明，历史通过也不是未来代码验收凭证。

流程PR的30bcdf5构建run41297573105中，47项CTest有46项通过，ssh_lifecycle报告“concurrent resize writer did not run”。原夹具没有先确认并发线程已获得调度，就开始500ms的进度等待，存在把启动延迟算入工作时间的误报路径；原日志没有调用次数，不能据此断言生产SSH故障或认定唯一根因。只补测试启动握手和诊断，原500ms进度、100ms UI及1500ms停止断言保持；增加一次600ms受控启动延迟，与原夹具作前后对照，正常及延迟证据分别保留。CMake仅新增延迟测试注册，生产源码不改；仍需实际Windows回归证明，不把夹具改动当作现场SSH验收。
