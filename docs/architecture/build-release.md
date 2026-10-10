# 构建与发布

SerialCtl 的交付链路固定为：

```text
check → clean build → test → PE compatibility check → package → extract and verify → bin
```

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

`release.ps1` 按上述顺序执行完整流程。任何 mandatory step 失败都不得在 `bin/` 留下新的正式外观文件。

`clean.ps1` 只清理可重新生成的 `build/`。首次目录迁移后可额外使用
`clean.ps1 -RepositoryHygiene` 清除旧原型的 `bin/obj`、依赖包历史展开副本和已经迁空的目录；它不会删除正式源码、固定依赖输入或 `bin` 中的已验证发布包。

## 发布内容

打包采用显式 allowlist，不复制整个仓库。压缩包只包含 x64 运行程序、用户文档、版本、第三方声明/许可证、MANIFEST 和包内 SHA-256 清单。PDB、OBJ、测试程序、源码、CMake cache、Git metadata、本机配置及 Secret 一律禁止进入。

正式包主目录直接包含 serialctl.exe、plink.exe、psftp.exe 和用户说明，驱动位于 drivers 子目录。该项目特例见 `docs/decisions/0001-single-win7-release-bundle.md`。

## 云端构建

`.github/workflows/windows-build.yml` 在 windows-2022 运行同一 release.ps1 流程，再启动解压后的 x64 程序进行 UI 冒烟检查并上传便携包。实际 Win7 验收按 docs/testing 执行。

发布工作流使用成功的 main 分支 Windows 构建 run ID、完整源码 SHA 和压缩包 SHA-256，从 Actions 下载已经验证的包再发布，不重新编译，不要求大压缩包进入 Git。发布标签 V1.0.4 对应 v1.0.4，保留历史发布。
