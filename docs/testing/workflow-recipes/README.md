# 本地运行原远端测试

`windows-build.yml` 在主分支和开发分支只编译和打包生产程序。GitHub 的升级验证仍从同一提交已完成的构建恢复精确 ZIP。此目录的 YAML 保留原真机测试的命令、依赖和取证要求，可作为本地执行参考。

| 范围 | 本地入口 |
| --- | --- |
| Windows 编译、CTest、兼容和临时签名提取 | `./scripts/release.ps1 -TestFixtureDirectory <仓库外的新目录>` |
| 仓库策略与发布工具 | `python tools/repository_policy.py --branch <开发分支> --base origin/main`，以及 `repository-policy.yml` 里的测试命令 |
| 完整包界面冒烟 | `./scripts/smoke-ui.ps1 -OutputDirectory <证据目录>` |
| 桌面输入和交互 | `desktop-interaction.yml` 内的 Python 与 PowerShell 入口，使用本地包和隔离 Windows 桌面 |
| 性能、长稳、内存诊断 | 对照相应 YAML 内的基线、测试参数和证据文件运行现有脚本 |

先设置本地证据目录，使用该次构建生成的 ZIP 和测试夹具。记录源码 SHA、包哈希、测试输出及实际使用的 Windows 环境。修改系统设置或驱动桌面输入的夹具在隔离测试账户运行。
