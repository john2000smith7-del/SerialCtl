# ADR 0001：单一 Windows 便携包包含双架构

## 状态

Accepted

## 背景

SerialCtl 同时支持 32 位和 64 位 Windows 7。通用工程规范倾向为每个架构生成独立 Artifact，但现场交付要求 `bin` 最终只有一个可复制压缩包。

## 决策

发布流程生成唯一的 `SerialCtl-V1.0.0-Win7.zip`。压缩包具有单一根目录，并在内部按以下路径隔离架构：

```text
windows/x86/native/
windows/x64/native/
```

每个目录只包含同架构的 `serialctl.exe`、`plink.exe` 和 `psftp.exe`。用户文档必须明确选择规则，MANIFEST 和 SHA-256 清单必须分别标识六个程序。

## 后果

- 现场只需复制和保存一个文件。
- 两个架构不会在同一运行目录中混用。
- 外层 ZIP 是 distribution bundle，而其中每个架构目录才是可运行 payload。
- 发布验证必须分别检查两个架构，任一架构失败都会阻止整个 bundle 发布。
