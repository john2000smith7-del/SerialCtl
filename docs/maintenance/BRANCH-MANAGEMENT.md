# 分支管理与历史保留

仓库仅长期保留 main。新任务统一使用 `feat/<topic>_YYYYMMDD_HHMM`，时间为北京时间。例：`feat/ui_capsules_20261031_0908`。

该命名匹配 `.github/workflows/windows-build.yml` 已有的 `feat/**` push 规则；无需为了命名更改生产构建或 CI 触发器。单独 `xxx_20261031_0908` 不匹配现有规则。获准推送后还应核对实际 Actions 事件、分支名和提交 SHA，不能把“名字正确”当作“已编译通过”。

分支完成适用验证后合并 main，确认提交已包含后删除临时远端分支及可安全删除的本地分支。main 的正式 Windows 验收继续执行，后续修复另开带时间分支。维护过程按顶层 AGENTS.md 执行。

## 清理前历史快照

2026-11-01 盘点时，main 为 `c7cdfff4c459f90ca99b88809b7525aac286dda7`，正式版本 V2.0.18。远端共82个分支，其中81个非 main 分支：34个完全合并，47个有独有提交；合计170个提交不在保留的 main 或32个正式标签中。

完整清单见 [清理前分支盘点](2026-11-01-branch-inventory.json) 和 [保留的标签快照](2026-11-01-tags.json)。这些文件是历史定位信息，不代表当前分支数量，不能代替 GitHub 最新状态或完整备份。

删除前已制作仓库外完整 Git bundle，并从该文件在独立本地目录恢复，核对82个分支、32个标签和所有对象，`git fsck --full --strict` 通过：

```text
SerialCtl-before-branch-cleanup-20261101.bundle
153421996 bytes
SHA-256 b879deeacb0bbc89f45270604292f7e2c09fcdfaa33ea76f96ea78aaf73836ab
```

该 bundle 保留原仓库历史，不包含仓库外的原更新签名私钥或用户工作目录。它是仓库维护备份，不是软件升级包，不加入正式软件 Release 的两个部署文件。

拥有者应把维护备份保存到自己的持久存储。当前执行环境中的参考位置为 `/workspace/work/serialctl-repository-governance-20261101/`，换环境后不保证存在，不能凭这段路径假称已经获得备份。

需要查看历史时，可在独立本地目录恢复：

```bash
git clone --mirror /path/to/SerialCtl-before-branch-cleanup-20261101.bundle recovered-SerialCtl.git
git --git-dir=recovered-SerialCtl.git fsck --full --strict
```

恢复用于审查旧历史，不授权把所有旧分支 mirror 推回清理后的远端。需要恢复某项内容时单独审查，避免覆盖最新 main 或重放旧发布请求。

## 已保留的历史资料

- [交接资料与旧手册归档](../archive/README.md)保留37个原文件；外层说明区分历史基线，原始数据保持不变。旧交接补丁不应用。
- 源码诊断原型、旧测试工作流和一次性发布请求通过完整历史备份保留，不整体合并进产品。
- 既有正式 tags、Releases、签名 ZIP／update.json 保持原样。清理只涉及分支引用和已核对的维护文档，不授权删除正式版本或改动原更新签名身份。
- 新增维护文档不改变 V2.0.18 的产品源码和已签名发布包。V2.0.18 原构建来源仍是 c7cdfff，不把后续文档提交当作旧包来源。

用户要求仓库为私有。维护者必须检查实际 visibility；权限不足时报告真实错误，不能假称已经修改。GitHub 原来包含旧分支名的链接在分支删除后可能失效，应使用 main 下的归档链接。源码与 GitHub Release 的私有访问需要授权；现有内网更新文件不会因 GitHub 可见性设置自动被替换。
