# Third-party notices

## PuTTY / Plink / PSFTP 0.85

SSH 和 SFTP 功能使用 PuTTY 项目的 `plink.exe` 与 `psftp.exe`。其中 `plink.exe` 应用了项目内保存的终端尺寸同步补丁，`psftp.exe` 应用了精确文件名列表、安全重命名和 UTF-8 本地路径补丁；补丁及可复现构建说明位于 `third_party/putty`。许可证全文随交付包保存为 `PUTTY-LICENCE.txt`。

来源：https://www.chiark.greenend.org.uk/~sgtatham/putty/

## YY-Thunks 1.2.1

用于兼容 Windows 7，MIT License。许可证全文随交付包保存为 `YY-THUNKS-LICENSE.txt`。

来源：https://github.com/Chuyu-Team/YY-Thunks

## nlohmann/json 3.11.3

MIT; `JSON-LICENSE.txt`。来源：https://github.com/nlohmann/json/releases/tag/v3.11.3

## NI-VISA 18.0 Runtime

Copyright © 2018 National Instruments Corporation. All Rights Reserved.

原始离线运行时安装包随应用提供，许可证、专利及全部原始第三方声明保留在 `drivers/ni-visa18`。安装及使用受该目录 `license/NI Released License Agreement - English.rtf` 等许可证约束。本程序通过原生 VISA C API 访问 PC USB 总线，不分发 NI 开发环境，不使用第三方 GPIB/PXI/VXI 控制器。发布依据运行时随附许可第 12 节及 NI 的 PC 总线部署说明。

- 下载：https://www.ni.com/en/support/downloads/drivers/download/unpackaged.ni-visa.306118.html
- Win7 支持：https://download.ni.com/support/softlib/visa/NI-VISA/18.0/Windows/readme.html
- 部署说明：https://knowledge.ni.com/KnowledgeArticleDetails?id=kA0VU00000043tl0AA&l=en-US

此安装包包含厂商所需的 x86/x64 组件；SerialCtl 与 PuTTY 运行程序仅提供 x64。
