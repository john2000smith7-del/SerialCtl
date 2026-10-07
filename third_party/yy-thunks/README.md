# YY-Thunks 1.2.1

SerialCtl 使用 YY-Thunks 的 Windows 7 兼容对象，以避免较新 MSVC 工具链产生目标系统不存在的直接导入。

源码树只保存原始 NuGet 包和许可证。CMake 配置时把所需对象临时解包到当前 `build/` 树；不要把展开后的 `build/`、`package/` 或 NuGet metadata 提交到仓库。
